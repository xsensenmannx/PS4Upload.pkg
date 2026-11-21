/*
 * Die sechs BGFT-Funktionen, die der Payload per sceKernelDlsym holt - an
 * einer Stelle und ohne orbis-Header. Das dlsym-Backend (install_bgft.h) und
 * GET /diagnostics (payload/ps4/main.cpp) lesen dieselbe Liste, damit sie
 * nicht auseinanderlaufen. Die Reihenfolge ist Teil des Vertrags: die
 * PS4U_BGFT_SYMBOL_*-Indizes benennen sie, der Hosttest haelt sie fest.
 */

#pragma once

#define PS4U_BGFT_SYMBOL_COUNT 6

/* Indizes in ps4u_bgft_symbols[]; sprechende Namen statt 0..5. */
enum
{
    PS4U_BGFT_SYMBOL_INIT = 0,
    PS4U_BGFT_SYMBOL_REGISTER_STORAGE,
    PS4U_BGFT_SYMBOL_START,
    PS4U_BGFT_SYMBOL_PROGRESS,
    PS4U_BGFT_SYMBOL_STOP,
    PS4U_BGFT_SYMBOL_UNREGISTER
};

static const char *const ps4u_bgft_symbols[PS4U_BGFT_SYMBOL_COUNT] = {
    "sceBgftServiceIntInit",
    "sceBgftServiceIntDownloadRegisterTaskByStorageEx",
    "sceBgftServiceIntDownloadStartTask",
    "sceBgftServiceIntDownloadGetProgress",
    "sceBgftServiceIntDownloadStopTask",
    "sceBgftServiceIntDownloadUnregisterTask",
};
