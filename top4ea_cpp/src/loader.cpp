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

static std::atomic<bool> g_running { true };

static void sigHandler(int) {
    g_running.store(false);
}

static void printBanner() {
    std::cout <<
        "╔══════════════════════════════════════════════════╗\n"
        "║      TOP4EA  C++20  DLL  v1.0                    ║\n"
        "║   Grid Recovery EA — Architecture 100% DLL       ║\n"
        "║   Connexion : MetaApi REST (libcurl)              ║\n"
        "╚══════════════════════════════════════════════════╝\n";
    std::cout << "[Loader] " << TOP4EA_Version() << "\n";
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);

    const char* cfgPath = "top4ea.conf";
    if (argc > 1) cfgPath = argv[1];

    printBanner();
    std::cout << "[Loader] Config : " << cfgPath << "\n";

    // ── Créer l'instance DLL ─────────────────────────────────
    void* handle = TOP4EA_Create(cfgPath);
    if (!handle) {
        std::cerr << "[Loader] ERREUR : TOP4EA_Create a échoué\n";
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
    std::cout << "[Loader] Arrêté proprement.\n";

    return 0;
}
