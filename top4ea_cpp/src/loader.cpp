// ============================================================
//  TOP4EA — loader.cpp  C++20
//  Chargeur DLL thin — point d'entrée principal
//
//  Architecture 100% DLL :
//    libtop4ea.so  ← toute la logique (DLL)
//    top4ea_loader ← ce fichier (thin wrapper)
//
//  Ce fichier ne contient AUCUNE logique de trading.
//  Il charge la DLL, démarre le moteur et attend l'arrêt.
// ============================================================
#include "../include/top4ea_dll.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

static std::atomic<bool> g_running { true };

static void sigHandler(int) {
    g_running.store(false);
}

// ── Verrou mono-instance ──────────────────────────────────────
//  Empêche plusieurs moteurs de trader simultanément le même compte
//  (ordres dupliqués, tickets SS/BS en conflit, état grille corrompu).
//  Le verrou est libéré automatiquement par l'OS si le process meurt
//  (flock lié au file descriptor, pas au contenu du fichier).
static int g_lockFd = -1;

static bool acquireSingleInstanceLock(const char* lockPath) {
    g_lockFd = ::open(lockPath, O_CREAT | O_RDWR, 0644);
    if (g_lockFd < 0) {
        std::cerr << "[Loader] ERREUR : impossible d'ouvrir le lockfile " << lockPath << "\n";
        return false;
    }
    if (::flock(g_lockFd, LOCK_EX | LOCK_NB) != 0) {
        std::cerr << "[Loader] ERREUR : une autre instance de TOP4EA tourne déjà "
                   << "(lock " << lockPath << " détenu). Arrêt pour éviter le trading "
                   << "en double sur le même compte.\n";
        ::close(g_lockFd);
        g_lockFd = -1;
        return false;
    }
    // Écrire le PID courant pour diagnostic (best-effort)
    if (::ftruncate(g_lockFd, 0) == 0) {
        std::string pidStr = std::to_string(::getpid()) + "\n";
        if (::write(g_lockFd, pidStr.c_str(), pidStr.size()) < 0) { /* best-effort */ }
    }
    return true;
}

static void releaseSingleInstanceLock() {
    if (g_lockFd >= 0) {
        ::flock(g_lockFd, LOCK_UN);
        ::close(g_lockFd);
        g_lockFd = -1;
    }
}

static void printBanner() {
    std::cout <<
        "╔══════════════════════════════════════════════════╗\n"
        "║      TOP4EA  C++20  DLL  v1.0                    ║\n"
        "║   Grid Recovery EA — Architecture 100% DLL       ║\n"
        "║   Connexion : TickerAll / MetaApi (libcurl)       ║\n"
        "╚══════════════════════════════════════════════════╝\n";
    std::cout << "[Loader] " << TOP4EA_Version() << "\n";
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);

    const char* cfgPath = "top4ea.conf";
    if (argc > 1) cfgPath = argv[1];

    // ── Verrou mono-instance (évite le double trading sur le compte) ──
    if (!acquireSingleInstanceLock("/tmp/top4ea.lock")) {
        return 1;
    }

    printBanner();
    std::cout << "[Loader] Config : " << cfgPath << "\n";

    // ── Créer l'instance DLL ─────────────────────────────────
    void* handle = TOP4EA_Create(cfgPath);
    if (!handle) {
        std::cerr << "[Loader] ERREUR : TOP4EA_Create a échoué\n";
        releaseSingleInstanceLock();
        return 1;
    }

    // ── Démarrer ─────────────────────────────────────────────
    TOP4EA_Start(handle);
    std::cout << "[Loader] Moteur DLL démarré — Ctrl+C pour arrêter\n";

    // ── Boucle principale ────────────────────────────────────
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ── Arrêt propre ─────────────────────────────────────────
    std::cout << "\n[Loader] Arrêt en cours...\n";
    TOP4EA_Stop(handle);
    TOP4EA_Destroy(handle);
    releaseSingleInstanceLock();
    std::cout << "[Loader] Arrêté proprement.\n";

    return 0;
}
