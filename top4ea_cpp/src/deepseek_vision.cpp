// ============================================================
//  TOP4EA — deepseek_vision.cpp  C++20
//
//  Client LLM DeepSeek V3 — Analyse institutionnelle XAUUSD
//  Appels REST asynchrones vers api.deepseek.com
//  Prompt contextualisé : session, structure, indicateurs MTF
//  Résultat : DSSignal + confidence + reasoning texte
// ============================================================
#include "../include/deepseek_vision.hpp"
#include <curl/curl.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <thread>

namespace top4ea {

// ─── Callback libcurl ────────────────────────────────────────
static size_t curlWriteDS(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* buf = static_cast<std::string*>(ud);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// ============================================================
//  Constructeur — lit la clé API depuis l'environnement
// ============================================================
DeepSeekVision::DeepSeekVision() {
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (key && std::strlen(key) > 8) {
        apiKey_ = key;
        std::cout << "[DeepSeekVision] Clé API chargée ("
                  << apiKey_.size() << " chars)\n";
    } else {
        std::cerr << "[DeepSeekVision] AVERTISSEMENT : "
                     "DEEPSEEK_API_KEY non trouvée — analyse LLM désactivée\n";
    }
    lastCallTime_ = std::chrono::steady_clock::now()
                    - std::chrono::seconds(MIN_CALL_INTERVAL_SEC + 1);
}

DeepSeekVision::~DeepSeekVision() {}

// ============================================================
//  forceWake — réveil forcé si le signal est périmé
//  Bypasse le throttling si lastSignal est vieux > FORCE_WAKE_SEC
// ============================================================
void DeepSeekVision::forceWake(const DSMarketContext& ctx) {
    if (!hasApiKey()) return;

    // Vérifier si le signal est réellement périmé
    {
        std::lock_guard lk(signalMu_);
        if (lastSignal_.signal != DSSignal::UNKNOWN) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - lastSignal_.timestamp).count();
            if (age < FORCE_WAKE_SEC) return;  // Signal encore frais, pas besoin
        }
    }

    // Forcer le reset du throttle pour provoquer un appel immédiat
    {
        std::lock_guard lk(callMu_);
        if (callInProgress_) return;  // Déjà un appel en cours, patience
        // Réinitialiser lastCallTime_ pour bypass throttle
        lastCallTime_ = std::chrono::steady_clock::now()
                        - std::chrono::seconds(MIN_CALL_INTERVAL_SEC + 1);
    }

    std::cout << "[DeepSeekVision] RÉVEIL FORCÉ — signal périmé, nouvel appel lancé\n";
    requestAnalysis(ctx);
}

// ============================================================
//  requestAnalysis — lance un appel asynchrone non-bloquant
// ============================================================
void DeepSeekVision::requestAnalysis(const DSMarketContext& ctx) {
    if (!hasApiKey()) return;

    // Vérification throttling
    {
        std::lock_guard lk(callMu_);
        if (callInProgress_) return;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                           now - lastCallTime_).count();
        if (elapsed < MIN_CALL_INTERVAL_SEC) return;
        callInProgress_ = true;
        lastCallTime_   = now;
    }

    // Lancer dans un thread détaché pour ne pas bloquer le tick
    std::thread([this, ctx]() {
        std::string prompt = buildPrompt(ctx);

        // Corps JSON pour l'API DeepSeek (compatible OpenAI)
        std::ostringstream body;
        body << "{"
             << "\"model\":\"deepseek-chat\","
             << "\"messages\":["
             <<   "{"
             <<     "\"role\":\"system\","
             <<     "\"content\":\"Tu es un expert en analyse technique "
                               "institutionnelle pour le trading de l'or (XAUUSDm). "
                               "Analyse les données de marché fournies et retourne "
                               "UNIQUEMENT un objet JSON valide sur une seule ligne "
                               "avec les champs : signal (BUY_BIAS|SELL_BIAS|NEUTRAL|WAIT|LOCK), "
                               "confidence (0-100), reason (string courte en français). "
                               "Pas d'explication, pas de markdown, seulement le JSON.\""
             <<   "},"
             <<   "{"
             <<     "\"role\":\"user\","
             <<     "\"content\":\"" << prompt << "\""
             <<   "}"
             << "],"
             << "\"temperature\":0.1,"
             << "\"max_tokens\":150,"
             << "\"stream\":false"
             << "}";

        std::string resp = httpPost(endpoint_, body.str(), 10000);
        DeepSeekSignal sig = parseResponse(resp);

        // Mise à jour du cache et compteur de répétitions sous verrou
        {
            std::lock_guard lk(signalMu_);
            auto now = std::chrono::steady_clock::now();
            sig.timestamp = now;
            sig.fresh     = (sig.signal != DSSignal::UNKNOWN);

            // Détection de signal identique répété (même direction + confidence ±5%)
            bool isIdentical = (sig.signal != DSSignal::UNKNOWN &&
                                lastSignal_.signal == sig.signal &&
                                lastSignal_.signal != DSSignal::UNKNOWN &&
                                std::abs(lastSignal_.confidence - sig.confidence) <= 5.0);

            if (isIdentical) {
                ++consecutiveIdenticalCount_;
                // firstIdenticalTimestamp_ reste inchangé (conserve le moment du 1er signal identique)
            } else {
                consecutiveIdenticalCount_ = 1;
                firstIdenticalTimestamp_   = now;
            }

            lastSignal_ = sig;
        }

        if (sig.fresh) {
            int cnt = 0;
            { std::lock_guard lk(signalMu_); cnt = consecutiveIdenticalCount_; }
            std::string repeatInfo = (cnt > 1) ? " [×" + std::to_string(cnt)
                                                + (cnt >= DS_REPEAT_DECAY_START ? " ⚠]" : "]")
                                               : "";
            std::cout << "[DeepSeekVision] Signal: " << sig.signalName()
                      << " conf=" << (int)sig.confidence
                      << "%" << repeatInfo << " | " << sig.reason << "\n";
        }

        {
            std::lock_guard lk(callMu_);
            callInProgress_ = false;
        }

        ++callCount_;
    }).detach();
}

// ============================================================
//  getLastSignal — accès thread-safe
//  Applique le decay de répétitions et le hard-stale timeout.
// ============================================================
DeepSeekSignal DeepSeekVision::getLastSignal() const {
    std::lock_guard lk(signalMu_);
    DeepSeekSignal sig = lastSignal_;

    // Vérifier fraîcheur (TTL 45s)
    if (sig.signal != DSSignal::UNKNOWN) {
        auto now = std::chrono::steady_clock::now();
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
                       now - sig.timestamp).count();
        sig.fresh = (age < CACHE_TTL_SEC);

        // ── Decay factor basé sur le nombre de répétitions ────
        // Commence à 1.0, diminue linéairement après DS_REPEAT_DECAY_START
        // jusqu'à DS_REPEAT_DECAY_FLOOR après DS_REPEAT_DECAY_N_MAX répétitions.
        double decay = 1.0;
        int cnt = consecutiveIdenticalCount_;
        if (cnt > DS_REPEAT_DECAY_START) {
            double progress = std::min(
                (double)(cnt - DS_REPEAT_DECAY_START) /
                (double)(DS_REPEAT_DECAY_N_MAX - DS_REPEAT_DECAY_START),
                1.0);
            decay = 1.0 - progress * (1.0 - DS_REPEAT_DECAY_FLOOR);
        }
        sig.effectiveConfidence = sig.confidence * decay;
        sig.consecutiveCount    = cnt;

        // ── Hard stale timeout — plafond absolu indépendant du TTL ──
        // Si le même signal est répété depuis plus de dsStaleHardSec_,
        // forcer fresh=false et effectiveConfidence=0 pour neutraliser l'impact.
        if (dsStaleHardSec_ > 0 && cnt >= 1) {
            auto firstAge = std::chrono::duration_cast<std::chrono::seconds>(
                                now - firstIdenticalTimestamp_).count();
            if (firstAge >= (long)dsStaleHardSec_) {
                sig.fresh               = false;
                sig.effectiveConfidence = 0.0;
                // Garder sig.confidence et sig.signal pour affichage diagnostique
            }
        }
    } else {
        sig.effectiveConfidence = 0.0;
        sig.consecutiveCount    = 0;
    }
    return sig;
}

// ============================================================
//  buildPrompt — construction du prompt institutionnel
// ============================================================
std::string DeepSeekVision::buildPrompt(const DSMarketContext& ctx) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    // Contexte de session et structure (échapper les guillemets)
    oss << "Session: " << ctx.session
        << " | Structure: " << ctx.structure
        << " | XAUUSDm Bid: " << ctx.bid
        << " Ask: " << ctx.ask
        << " | SpreadRatio: " << ctx.spreadRatio
        << " | ";

    oss << "M5(ATR=" << ctx.m5Atr
        << " RSI=" << ctx.m5Rsi
        << " MACD=" << ctx.m5Macd
        << " MACDhist=" << ctx.m5MacdHist
        << " BBwidth=" << ctx.m5BbWidth
        << "% mom=" << (ctx.m5Momentum * 100.0) << "%) | ";

    oss << "M30(ATR=" << ctx.m30Atr
        << " RSI=" << ctx.m30Rsi
        << " MACD=" << ctx.m30Macd
        << " MACDhist=" << ctx.m30MacdHist
        << ") | ";

    oss << "BasketPnL=" << ctx.basketPnl
        << "$ ordresMkt=" << ctx.openOrders
        << " | LotBalance: BS=" << ctx.bsLots
        << " SS=" << ctx.ssLots
        << " delta=" << (ctx.netDelta >= 0 ? "+" : "") << ctx.netDelta
        << (ctx.inRecovery ? " [RECOVERY]" : "");

    // Instruction explicite sur la balance : aider le LLM à orienter la recovery
    if (ctx.inRecovery) {
        if (ctx.netDelta > 0.05) {
            oss << " | IMPORTANT: BS surexpose de " << ctx.netDelta
                << " lots vs SS. Si tu vois une tendance baissiere, "
                   "le signal SELL_BIAS aidera a reduire l'exposition BS.";
        } else if (ctx.netDelta < -0.05) {
            oss << " | IMPORTANT: SS surexpose de " << (-ctx.netDelta)
                << " lots vs BS. Si tu vois une tendance haussiere, "
                   "le signal BUY_BIAS aidera a reduire l'exposition SS.";
        } else {
            oss << " | Balance BS/SS quasi neutre - orienter selon la structure.";
        }
    }

    // Échapper les caractères spéciaux JSON dans la chaîne
    std::string raw = oss.str();
    std::string escaped;
    escaped.reserve(raw.size());
    for (char c : raw) {
        if      (c == '"')  escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else if (c == '\n') escaped += "\\n";
        else                escaped += c;
    }
    return escaped;
}

// ============================================================
//  parseResponse — extrait le JSON du LLM
// ============================================================
DeepSeekSignal DeepSeekVision::parseResponse(const std::string& json) {
    DeepSeekSignal sig;
    sig.rawResponse = json;

    if (json.empty()) {
        return sig;
    }

    // Extraire le content du premier message de la réponse
    // Structure : {"choices":[{"message":{"content":"{ ... }"}}]}
    std::string content;

    // Trouver "content":"..."
    size_t cpos = json.find("\"content\":");
    if (cpos == std::string::npos) {
        std::cerr << "[DeepSeekVision] Pas de champ 'content' dans la réponse\n";
        return sig;
    }
    cpos += 10; // sauter "content":

    // Sauter espaces
    while (cpos < json.size() && std::isspace((unsigned char)json[cpos])) ++cpos;

    if (cpos >= json.size() || json[cpos] != '"') return sig;
    ++cpos; // sauter le "

    // Lire jusqu'au prochain " non échappé
    std::string rawContent;
    while (cpos < json.size()) {
        char c = json[cpos++];
        if (c == '\\' && cpos < json.size()) {
            char nc = json[cpos++];
            if (nc == 'n') rawContent += '\n';
            else if (nc == '"') rawContent += '"';
            else if (nc == '\\') rawContent += '\\';
            else { rawContent += nc; }
        } else if (c == '"') {
            break;
        } else {
            rawContent += c;
        }
    }

    // rawContent contient maintenant la chaîne JSON retournée par le LLM
    // ex: {"signal":"BUY_BIAS","confidence":75,"reason":"trend haussier fort H1"}
    content = rawContent;

    // Extraire les champs
    std::string sigStr = jsonGet(content, "signal");
    double conf        = jsonGetD(content, "confidence");
    std::string reason = jsonGet(content, "reason");

    // Mapper signal
    if      (sigStr == "BUY_BIAS")  sig.signal = DSSignal::BUY_BIAS;
    else if (sigStr == "SELL_BIAS") sig.signal = DSSignal::SELL_BIAS;
    else if (sigStr == "NEUTRAL")   sig.signal = DSSignal::NEUTRAL;
    else if (sigStr == "WAIT")      sig.signal = DSSignal::WAIT;
    else if (sigStr == "LOCK")      sig.signal = DSSignal::LOCK;
    else {
        // Fallback : chercher les mots-clés dans la réponse brute
        if (content.find("BUY")    != std::string::npos) sig.signal = DSSignal::BUY_BIAS;
        else if (content.find("SELL")   != std::string::npos) sig.signal = DSSignal::SELL_BIAS;
        else if (content.find("LOCK")   != std::string::npos) sig.signal = DSSignal::LOCK;
        else if (content.find("WAIT")   != std::string::npos) sig.signal = DSSignal::WAIT;
        else sig.signal = DSSignal::NEUTRAL;
    }

    sig.confidence = (conf > 0.0) ? conf : 50.0;
    sig.reason     = reason.empty() ? content.substr(0, 80) : reason;

    return sig;
}

// ============================================================
//  httpPost — appel REST avec libcurl
// ============================================================
std::string DeepSeekVision::httpPost(const std::string& url,
                                     const std::string& body,
                                     int timeoutMs) {
    std::string response;
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[DeepSeekVision] curl_easy_init() échoué\n";
        ++errorCount_;
        return {};
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + apiKey_;
    headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST,            1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,      body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,   (long)body.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,      headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   curlWriteDS);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,      (long)timeoutMs);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,  1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,  2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  1L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        std::cerr << "[DeepSeekVision] HTTP erreur: "
                  << curl_easy_strerror(rc) << "\n";
        ++errorCount_;
        response.clear();
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

// ============================================================
//  Helpers JSON légers
// ============================================================
std::string DeepSeekVision::jsonGet(const std::string& json,
                                    const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
    if (pos >= json.size()) return {};

    if (json[pos] == '"') {
        ++pos;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\') ++pos;
            if (pos < json.size()) val += json[pos++];
        }
        return val;
    }
    // Valeur numérique ou booléenne
    size_t end = json.find_first_of(",}", pos);
    if (end == std::string::npos) end = json.size();
    std::string val = json.substr(pos, end - pos);
    while (!val.empty() && std::isspace((unsigned char)val.back())) val.pop_back();
    return val;
}

double DeepSeekVision::jsonGetD(const std::string& json,
                                const std::string& key) {
    std::string v = jsonGet(json, key);
    if (v.empty()) return 0.0;
    try { return std::stod(v); }
    catch (...) { return 0.0; }
}

} // namespace top4ea
