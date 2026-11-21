/*
 * Zustandsmaschine der Installation, backend-neutral.
 *
 * Kennt nur eine Tabelle aus sechs Aufrufen (init, register_task, start,
 * progress, stop, unregister); alles Sony-spezifische steckt in einem Backend
 * wie install_bgft.h. Keine orbis-Header, kein libkernel, keine Sockets - so
 * laeuft dieselbe Logik auf dem PC gegen ein Fake-Backend (siehe
 * payload/ps4/tests/install_hostcheck.cpp).
 *
 * Alle Puffer haben feste Groessen; die Zeit kommt als Parameter herein,
 * damit Tests ohne Uhr und ohne Warten laufen.
 */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stall.h"

#define INSTALL_PHASE_IDLE 0
#define INSTALL_PHASE_UPLOAD 1
#define INSTALL_PHASE_CHECK 2
#define INSTALL_PHASE_REGISTER 3
#define INSTALL_PHASE_RUNNING 4
#define INSTALL_PHASE_DONE 5
#define INSTALL_PHASE_FAILED 6

/* Fruehester Abstand zweier Fortschrittsabfragen. */
#define INSTALL_POLL_INTERVAL_MS 500ull

/* Was das Backend zum Registrieren braucht; Strings bleiben Eigentum des
   Aufrufers und werden nicht kopiert. */
struct InstallRequest
{
    const char *path;
    const char *name;
    const char *title;
    const char *title_id;
    const char *content_id;
    const char *package_type;
    unsigned long long size;
    int user_id;
};

/* Fortschritt in Backend-neutraler Form; entspricht den BGFT-Feldern, die
   fuer Prozent und Stillstandserkennung gebraucht werden. */
struct InstallProgress
{
    unsigned int bits;
    int error;
    unsigned long long length;
    unsigned long long transferred;
    unsigned long long length_total;
    unsigned long long transferred_total;
    unsigned int num_index;
    unsigned int num_total;
    int preparing_percent;
    int local_copy_percent;
};

struct InstallOps
{
    const char *name;
    int (*init)(void *ctx);
    int (*register_task)(void *ctx, const InstallRequest &request, int *task);
    int (*start)(void *ctx, int task);
    int (*progress)(void *ctx, int task, InstallProgress *progress);
    int (*stop)(void *ctx, int task);
    int (*unregister)(void *ctx, int task);
    void *ctx;
};

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
    InstallStallTracker stall;
};

struct InstallCore
{
    InstallController *state;
    const InstallOps *ops;
    void (*log)(void *log_ctx, const char *stage, int code);
    void *log_ctx;
};

static void install_core_log(InstallCore *core, const char *stage, int code)
{
    if (core != 0 && core->log != 0)
    {
        core->log(core->log_ctx, stage, code);
    }
}

static void install_core_copy(char *out, size_t capacity, const char *text)
{
    size_t index = 0;

    if (out == 0 || capacity == 0)
    {
        return;
    }
    if (text != 0)
    {
        while (text[index] != 0 && index + 1 < capacity)
        {
            out[index] = text[index];
            index++;
        }
    }
    out[index] = 0;
}

static void install_core_note(InstallController *state, const char *text)
{
    if (state == 0 || text == 0)
    {
        return;
    }
    install_core_copy(state->note, sizeof(state->note), text);
}

static const char *install_core_phase_name(int phase)
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

static bool install_core_busy(const InstallCore *core)
{
    if (core == 0 || core->state == 0)
    {
        return false;
    }
    return core->state->phase == INSTALL_PHASE_REGISTER ||
           core->state->phase == INSTALL_PHASE_RUNNING;
}

/* Die installierte PKG wird nicht mehr gebraucht. */
static void install_core_cleanup_file(InstallCore *core)
{
    InstallController *state;
    int result;

    if (core == 0 || core->state == 0)
    {
        return;
    }
    state = core->state;
    if (state->path[0] == 0)
    {
        return;
    }

    result = remove(state->path);
    install_core_log(core, result == 0 ? "PKG nach Installation aufgeraeumt"
                                       : "PKG-Aufraeumen fehlgeschlagen",
                     result);
    state->path[0] = 0;
}

static void install_core_unregister_task(InstallCore *core)
{
    InstallController *state;
    int result;

    if (core == 0 || core->state == 0 || core->ops == 0)
    {
        return;
    }
    state = core->state;
    if (state->task < 0)
    {
        return;
    }

    if (core->ops->stop != 0)
    {
        result = core->ops->stop(core->ops->ctx, state->task);
        install_core_log(core, "sceBgftServiceIntDownloadStopTask", result);
    }
    if (core->ops->unregister != 0)
    {
        result = core->ops->unregister(core->ops->ctx, state->task);
        install_core_log(core, "sceBgftServiceIntDownloadUnregisterTask", result);
    }
    state->task = -1;
}

static void install_core_fail(InstallCore *core, const char *stage, int code)
{
    InstallController *state = core->state;

    state->phase = INSTALL_PHASE_FAILED;
    state->error = code;
    install_core_note(state, stage);
    install_core_log(core, stage, code);
    install_core_unregister_task(core);
    install_core_cleanup_file(core);
}

static bool install_core_init(InstallCore *core)
{
    int result;

    if (core == 0 || core->state == 0)
    {
        return false;
    }
    if (core->state->inited)
    {
        return true;
    }
    if (core->ops == 0 || core->ops->init == 0)
    {
        install_core_fail(core, "Install-Backend", -1);
        return false;
    }

    result = core->ops->init(core->ops->ctx);
    install_core_log(core, "BGFT-Init", result);
    if (result < 0)
    {
        install_core_fail(core, "BGFT-Init", result);
        return false;
    }

    core->state->inited = true;
    return true;
}

/* Fortschritt und Task-Zustand in eine kompakte Signatur packen. */
static uint64_t install_core_signature(const InstallProgress &progress)
{
    uint64_t signature = 1469598103934665603ull;

    signature = stall_mix(signature, (uint64_t)progress.bits);
    signature = stall_mix(signature, (uint64_t)progress.length);
    signature = stall_mix(signature, (uint64_t)progress.transferred);
    signature = stall_mix(signature, (uint64_t)progress.length_total);
    signature = stall_mix(signature, (uint64_t)progress.transferred_total);
    signature = stall_mix(signature, (uint64_t)progress.num_index);
    signature = stall_mix(signature, (uint64_t)progress.num_total);
    /* restSec/restSecTotal sind Zeitschaetzungen, kein Fortschritt. */
    signature = stall_mix(signature,
                          (uint64_t)(unsigned int)progress.preparing_percent);
    signature = stall_mix(signature,
                          (uint64_t)(unsigned int)progress.local_copy_percent);

    return signature;
}

static void install_core_reset(InstallCore *core, const char *name)
{
    InstallController *state;
    bool initialized;

    if (core == 0 || core->state == 0)
    {
        return;
    }
    state = core->state;
    initialized = state->inited;

    memset(state, 0, sizeof(*state));
    state->inited = initialized;
    state->phase = INSTALL_PHASE_IDLE;
    state->percent = -1;
    state->task = -1;

    if (name != 0)
    {
        install_core_copy(state->name, sizeof(state->name), name);
    }
}

/*
 * Registriert die bereits heruntergeladene lokale PKG und startet die
 * Installation. Der Pfad geht direkt ans Backend; es kopiert die Datei in
 * den App-Slot und liest sie dabei.
 */
static void install_core_begin(InstallCore *core, const InstallRequest &request)
{
    InstallController *state;
    int task = -1;
    int result;

    if (core == 0 || core->state == 0)
    {
        return;
    }
    state = core->state;

    state->phase = INSTALL_PHASE_REGISTER;
    state->percent = 0;
    state->error = 0;
    state->size = request.size;
    state->task = -1;
    install_core_copy(state->path, sizeof(state->path), request.path);
    install_core_copy(state->title, sizeof(state->title), request.title);
    install_core_copy(state->content_id, sizeof(state->content_id),
                      request.content_id);

    if (!install_core_init(core))
    {
        return;
    }
    if (core->ops == 0 || core->ops->register_task == 0 ||
        core->ops->start == 0)
    {
        install_core_fail(core, "Install-Backend", -1);
        return;
    }

    result = core->ops->register_task(core->ops->ctx, request, &task);
    install_core_log(core, "BGFT-Registrierung", result);
    if (result < 0)
    {
        install_core_fail(core, "BGFT-Registrierung", result);
        return;
    }
    state->task = task;

    result = core->ops->start(core->ops->ctx, task);
    install_core_log(core, "BGFT-Start", result);
    if (result < 0)
    {
        install_core_fail(core, "BGFT-Start", result);
        return;
    }

    state->phase = INSTALL_PHASE_RUNNING;
    state->last_poll_ms = 0;
    stall_tracker_reset(&state->stall);
}

static void install_core_poll(InstallCore *core, unsigned long long now_ms)
{
    InstallController *state;
    InstallProgress progress;
    unsigned long long total;
    unsigned long long done;
    int percent;
    int result;

    if (core == 0 || core->state == 0)
    {
        return;
    }
    state = core->state;
    if (state->phase != INSTALL_PHASE_RUNNING || state->task < 0)
    {
        return;
    }

    if (state->last_poll_ms != 0 && now_ms >= state->last_poll_ms &&
        now_ms - state->last_poll_ms < INSTALL_POLL_INTERVAL_MS)
    {
        return;
    }
    state->last_poll_ms = now_ms;

    if (core->ops == 0 || core->ops->progress == 0)
    {
        install_core_fail(core, "Install-Backend", -1);
        return;
    }

    memset(&progress, 0, sizeof(progress));
    result = core->ops->progress(core->ops->ctx, state->task, &progress);
    if (result < 0)
    {
        install_core_fail(core, "BGFT-Fortschritt", result);
        return;
    }
    if (progress.error != 0)
    {
        install_core_fail(core, "BGFT-Installation", progress.error);
        return;
    }

    total = progress.length_total != 0 ? progress.length_total
                                       : progress.length;
    done = progress.length_total != 0 ? progress.transferred_total
                                      : progress.transferred;

    percent = 0;
    if (total > 0)
    {
        percent = (int)((done * 100ull) / total);
    }
    if (progress.preparing_percent > percent)
    {
        percent = progress.preparing_percent;
    }
    if (progress.local_copy_percent > percent)
    {
        percent = progress.local_copy_percent;
    }
    if (percent > 100)
    {
        percent = 100;
    }
    state->percent = percent;

    if (total > 0 && done >= total)
    {
        state->phase = INSTALL_PHASE_DONE;
        state->percent = 100;
        install_core_log(core, "BGFT fertig, Task", state->task);
        install_core_cleanup_file(core);
        return;
    }

    if (stall_tracker_check(&state->stall, install_core_signature(progress),
                            now_ms, INSTALL_STALL_TIMEOUT_MS))
    {
        install_core_fail(core, "BGFT-Stillstand", 0);
    }
}

static void install_core_mark_check(InstallCore *core)
{
    if (core == 0 || core->state == 0)
    {
        return;
    }
    core->state->phase = INSTALL_PHASE_CHECK;
    core->state->percent = -1;
    core->state->error = 0;
}

static void install_core_mark_failed(InstallCore *core, int code)
{
    if (core == 0 || core->state == 0)
    {
        return;
    }
    core->state->phase = INSTALL_PHASE_FAILED;
    core->state->error = code;
    core->state->percent = -1;
}

static void install_core_mark_failed_note(InstallCore *core, int code,
                                          const char *note)
{
    if (core == 0 || core->state == 0)
    {
        return;
    }
    core->state->phase = INSTALL_PHASE_FAILED;
    core->state->error = code;
    core->state->percent = -1;
    install_core_note(core->state, note);
}

static void install_core_mark_upload(InstallCore *core,
                                     unsigned long long received,
                                     unsigned long long total)
{
    if (core == 0 || core->state == 0)
    {
        return;
    }
    core->state->phase = INSTALL_PHASE_UPLOAD;
    core->state->size = total;
    core->state->error = 0;
    core->state->percent =
        total > 0 ? (int)((received * 100ull) / total) : -1;
}
