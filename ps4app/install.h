/*
 * Installation ueber BGFT - Klebeschicht fuer die App.
 *
 * Die Zustandsmaschine steht in install_core.h (host-getestet, ohne
 * Sony-Header), das BGFT-Backend in install_bgft.h (Modul per dlsym, kein
 * Link gegen libSceBgft). Hier bleibt nur, was zur App gehoert: der globale
 * Zustand, die Log-Ausgabe ins Kernel-Log und der Vordergrund-Benutzer.
 *
 * ACHTUNG: Auf Retail-Firmware prueft BGFT die Paketsignatur. Selbstgebaute
 * Homebrew-PKGs sind nicht signiert; ob der Task angenommen wird, ist ohne
 * Hardware ungeklaert.
 */

#pragma once

#include <orbis/UserService.h>

#include "diag.h"
#include "install_bgft.h"
#include "install_core.h"
#include "pkg.h"

static InstallController g_install;
static InstallBgft g_install_bgft;

/* Kernel-Log statt printf: der Payload hat kein Terminal. */
static void install_log(void *log_ctx, const char *stage, int code)
{
    (void)log_ctx;
    diag_code(stage, code);
}

static InstallCore g_install_core = {&g_install, 0, install_log, 0};

static void install_ensure_backend(void)
{
    if (g_install_core.ops == 0)
    {
        g_install_core.ops = install_bgft_ops(&g_install_bgft);
    }
}

static void install_reset(const char *name)
{
    install_core_reset(&g_install_core, name);
}

static bool install_busy(void)
{
    return install_core_busy(&g_install_core);
}

static const char *install_phase_name(int phase)
{
    return install_core_phase_name(phase);
}

static void install_mark_upload(unsigned long long received,
                                unsigned long long total)
{
    install_core_mark_upload(&g_install_core, received, total);
}

static void install_mark_check(void)
{
    install_core_mark_check(&g_install_core);
}

static void install_mark_failed(int code)
{
    install_core_mark_failed(&g_install_core, code);
}

static void install_mark_failed_note(int code, const char *note)
{
    install_core_mark_failed_note(&g_install_core, code, note);
}

/* Registriert die bereits heruntergeladene lokale PKG und startet BGFT. */
static void install_begin(const char *path, const PkgInfo &info,
                          int fallback_user_id)
{
    InstallRequest request;
    int32_t user_id = (int32_t)fallback_user_id;
    int result;

    install_ensure_backend();

    result = sceUserServiceGetForegroundUser(&user_id);
    if (result < 0 || user_id < 0)
    {
        user_id = (int32_t)fallback_user_id;
        diag_code("sceUserServiceGetForegroundUser", result);
    }

    request.path = path;
    request.name = g_install.name;
    request.title = info.title;
    request.title_id = info.title_id;
    request.content_id = info.content_id;
    request.package_type = pkg_bgft_package_type(info.content_type);
    request.size = info.size;
    request.user_id = (int)user_id;

    install_core_begin(&g_install_core, request);
}

static void install_poll(unsigned long long now_ms)
{
    install_core_poll(&g_install_core, now_ms);
}
