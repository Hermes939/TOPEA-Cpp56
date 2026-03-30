// ============================================================
//  TOP4EA — top4ea_dll.h
//  API C exportée — interface DLL C++20
//
//  Compatible :
//   - Linux  : libtop4ea.so  (dlopen)
//   - Windows: top4ea.dll    (LoadLibrary / MQL4 #import)
//
//  Pattern d'utilisation :
//    void* h = TOP4EA_Create("top4ea.conf");
//    TOP4EA_Start(h);
//    // ... boucle principale ...
//    TOP4EA_Stop(h);
//    TOP4EA_Destroy(h);
// ============================================================
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ─── Cycle de vie ───────────────────────────────────────────
void*  TOP4EA_Create (const char* config_path);
void   TOP4EA_Destroy(void* handle);
void   TOP4EA_Start  (void* handle);
void   TOP4EA_Stop   (void* handle);

// ─── État ───────────────────────────────────────────────────
int    TOP4EA_IsConnected(void* handle);
int    TOP4EA_IsRunning  (void* handle);
void   TOP4EA_GetStatus  (void* handle, char* buf, int buf_len);

// ─── Injection de tick (mode MT4 DLL) ───────────────────────
//     Appelé par le EA MQL4 à chaque OnTick()
//     time_ms = timestamp Unix en millisecondes
void   TOP4EA_OnTick(void* handle,
                     const char* symbol,
                     double bid,
                     double ask,
                     long long time_ms);

// ─── Commandes de trading directes (mode MT4 DLL) ───────────
//     Retournent le ticket (>0) ou un code erreur (<0)
int    TOP4EA_PlaceOrder(void* handle,
                         int    order_type,
                         double lots,
                         double price,
                         double sl,
                         double tp,
                         int    magic,
                         int    slippage,
                         const char* comment);

int    TOP4EA_CloseOrder (void* handle, int ticket, double lots,
                          double price, int slippage);
int    TOP4EA_DeleteOrder(void* handle, int ticket);
int    TOP4EA_ModifyOrder(void* handle, int ticket, double price,
                          double sl, double tp);

// ─── Infos compte (MT4 DLL) ─────────────────────────────────
double TOP4EA_GetBalance    (void* handle);
double TOP4EA_GetEquity     (void* handle);
double TOP4EA_GetFreeMargin (void* handle);
double TOP4EA_GetDailyPnL   (void* handle);
int    TOP4EA_GetOrderCount (void* handle);

// ─── Version ────────────────────────────────────────────────
const char* TOP4EA_Version(void);

// ─── Order types (enum compatible C) ────────────────────────
#define TOP4EA_OP_BUY        0
#define TOP4EA_OP_SELL       1
#define TOP4EA_OP_BUYLIMIT   2
#define TOP4EA_OP_SELLLIMIT  3
#define TOP4EA_OP_BUYSTOP    4
#define TOP4EA_OP_SELLSTOP   5

#ifdef __cplusplus
}
#endif
