/*
 * Installation ueber BGFT (libSceBgft).
 *
 * Ablauf: BGFT initialisieren, Task fuer die lokale PKG registrieren, Task
 * starten, Fortschritt pollen. Alle Rueckgabecodes gehen ins Kernel-Log,
 * damit ein Fehlschlag auf Hardware nicht im Dunkeln bleibt.
 *
 * ACHTUNG: Auf Retail-Firmware prueft BGFT die Paketsignatur. Selbstgebaute
 * Homebrew-PKGs sind nicht signiert; ob der Task angenommen wird, ist ohne
 * Hardware ungeklaert.
 */

#pragma once

#include <stdint.h>
#include <string.h>

#include <orbis/Bgft.h>
#include <orbis/libkernel.h>

#include "diag.h"
#include "pkg.h"

#define INSTALL_PHASE_IDLE 0
#define INSTALL_PHASE_UPLOAD 1
#define INSTALL_PHASE_CHECK 2
#define INSTALL_PHASE_REGISTER 3
#define INSTALL_PHASE_RUNNING 4
#define INSTALL_PHASE_DONE 5
#define INSTALL_PHASE_FAILED 6

struct InstallController
{
    int phase;
    int percent;
    int error;
    bool inited;
    int task;
    char name[80];
    char path[160];
    char title[68];
    char content_id[40];
    char note[72];
    unsigned long long size;
    unsigned long long last_poll_ms;
};

static InstallController g_install;
static unsigned char g_bgft_heap[1024 * 1024];

static const char *install_phase_name(int phase)
{
    switch (phase)
    {
        case INSTALL_PHASE_UPLOAD: return "UPLOAD";
        case INSTALL_PHASE_CHECK: return "PRUEFUNG";
        case INSTALL_PHASE_REGISTER: return "REGISTRIERUNG";
        case INSTALL_PHASE_RUNNING: return "INSTALLATION";
        case INSTALL_PHASE_DONE: return "FERTIG";
        case INSTALL_PHASE_FAILED: return "FEHLER";
        default: return "BEREIT";
    }
}

static void install_note(const char *text)
{
    int index = 0;

    while (text[index] != 0 && index < (int)sizeof(g_install.note) - 1)
    {
        g_install.note[index] = text[index];
        index++;
    }
    g_install.note[index] = 0;
}

static void install_fail(const char *stage, int code)
{
    g_install.phase = INSTALL_PHASE_FAILED;
    g_install.error = code;
    install_note(stage);
    diag_code(stage, code);
}

static void install_mark_failed_note(int code, const char *note)
{
    g_install.phase = INSTALL_PHASE_FAILED;
    g_install.error = code;
    g_install.percent = -1;
    install_note(note);
}

static bool install_bgft_init(void)
{
    OrbisBgftInitParams params;
    int result;

    if (g_install.inited)
    {
        return true;
    }

    memset(&params, 0, sizeof(params));
    params.heap = g_bgft_heap;
    params.heapSize = sizeof(g_bgft_heap);

    result = sceBgftServiceIntInit(&params);
    diag_code("sceBgftServiceIntInit", result);
    if (result < 0)
    {
        install_fail("BGFT-Init", result);
        return false;
    }

    g_install.inited = true;
    return true;
}

static void install_reset(const char *name)
{
    bool initialized = g_install.inited;

    memset(&g_install, 0, sizeof(g_install));
    g_install.inited = initialized;
    g_install.phase = INSTALL_PHASE_IDLE;
    g_install.percent = -1;
    g_install.task = -1;

    if (name != 0)
    {
        int used = 0;
        while (name[used] != 0 && used < (int)sizeof(g_install.name) - 1)
        {
            g_install.name[used] = name[used];
            used++;
        }
        g_install.name[used] = 0;
    }
}

/*
 * Registriert die lokale Datei und startet die Installation.
 * contentUrl zeigt per file:// auf die bereits heruntergeladene PKG.
 */
static void install_begin(const char *path, const PkgInfo &info, int user_id)
{
    OrbisBgftDownloadParam param;
    char url[200];
    int used = 0;
    int task = -1;
    int result;

    g_install.phase = INSTALL_PHASE_REGISTER;
    g_install.percent = 0;
    g_install.error = 0;
    g_install.size = info.size;
    g_install.task = -1;

    used = 0;
    {
        const char *prefix = "file://";
        int index = 0;

        while (prefix[index] != 0)
        {
            url[used++] = prefix[index++];
        }
        index = 0;
        while (path[index] != 0 && used < (int)sizeof(url) - 1)
        {
            url[used++] = path[index++];
        }
    }
    url[used] = 0;

    {
        int index = 0;
        while (path[index] != 0 && index < (int)sizeof(g_install.path) - 1)
        {
            g_install.path[index] = path[index];
            index++;
        }
        g_install.path[index] = 0;
    }
    {
        int index = 0;
        while (info.title[index] != 0 && index < (int)sizeof(g_install.title) - 1)
        {
            g_install.title[index] = info.title[index];
            index++;
        }
        g_install.title[index] = 0;
    }
    {
        int index = 0;
        while (info.content_id[index] != 0 && index < (int)sizeof(g_install.content_id) - 1)
        {
            g_install.content_id[index] = info.content_id[index];
            index++;
        }
        g_install.content_id[index] = 0;
    }

    if (!install_bgft_init())
    {
        return;
    }

    memset(&param, 0, sizeof(param));
    param.userId = user_id;
    param.entitlementType = 0;
    param.id = g_install.content_id;
    param.contentUrl = url;
    param.contentName = g_install.title;
    param.packageType = "PS4GD";
    param.packageSubType = "GD";
    param.option = ORBIS_BGFT_TASK_OPT_NONE;
    param.packageSize = (uint32_t)(info.size > 0xFFFFFFFFull ? 0xFFFFFFFFull : info.size);

    result = sceBgftServiceIntDownloadRegisterTask(&param, &task);
    diag_code("sceBgftServiceIntDownloadRegisterTask", result);
    if (result < 0)
    {
        install_fail("BGFT-Registrierung", result);
        return;
    }

    g_install.task = task;

    result = sceBgftServiceIntDownloadStartTask(task);
    diag_code("sceBgftServiceIntDownloadStartTask", result);
    if (result < 0)
    {
        install_fail("BGFT-Start", result);
        return;
    }

    g_install.phase = INSTALL_PHASE_RUNNING;
    g_install.last_poll_ms = 0;
}

static void install_poll(unsigned long long now_ms)
{
    OrbisBgftTaskProgress progress;
    unsigned int total;
    unsigned int done;
    int percent;
    int result;

    if (g_install.phase != INSTALL_PHASE_RUNNING || g_install.task < 0)
    {
        return;
    }

    if (g_install.last_poll_ms != 0 && now_ms - g_install.last_poll_ms < 500)
    {
        return;
    }
    g_install.last_poll_ms = now_ms;

    memset(&progress, 0, sizeof(progress));
    result = sceBgftServiceIntDownloadGetProgress(g_install.task, &progress);
    if (result < 0)
    {
        install_fail("BGFT-Fortschritt", result);
        return;
    }

    if (progress.errorResult != 0)
    {
        install_fail("BGFT-Installation", progress.errorResult);
        return;
    }

    total = progress.lengthTotal != 0 ? progress.lengthTotal : progress.length;
    done = progress.lengthTotal != 0 ? progress.transferredTotal : progress.transferred;

    percent = 0;
    if (total > 0)
    {
        percent = (int)(((unsigned long long)done * 100ull) / total);
    }
    if (progress.preparingPercent > percent)
    {
        percent = progress.preparingPercent;
    }
    if (percent > 100)
    {
        percent = 100;
    }
    g_install.percent = percent;

    if (total > 0 && done >= total)
    {
        g_install.phase = INSTALL_PHASE_DONE;
        g_install.percent = 100;
        diag_code("BGFT fertig, Task", g_install.task);
    }
}

static void install_mark_check(void)
{
    g_install.phase = INSTALL_PHASE_CHECK;
    g_install.percent = -1;
    g_install.error = 0;
}

static void install_mark_failed(int code)
{
    g_install.phase = INSTALL_PHASE_FAILED;
    g_install.error = code;
    g_install.percent = -1;
}

static void install_mark_upload(unsigned long long received, unsigned long long total)
{
    g_install.phase = INSTALL_PHASE_UPLOAD;
    g_install.size = total;
    g_install.error = 0;
    g_install.percent = total > 0 ? (int)((received * 100ull) / total) : -1;
}
