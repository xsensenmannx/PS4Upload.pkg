/*
 * BGFT-Backend fuer install_core.h - ohne Link gegen libSceBgft.
 *
 * Das Modul wird zur Laufzeit geladen ("libSceBgft.sprx", alternativ mit
 * vollem Systempfad) und die sechs benoetigten Funktionen werden per
 * sceKernelDlsym geholt. Damit traegt der Payload kein DT_NEEDED auf BGFT:
 * fehlt das Modul oder verweigert es das Laden, schlaegt der Init-Aufruf mit
 * dem Ladecode fehl statt das ELF unladbar zu machen.
 *
 * Nicht auf Hardware geprueft: ob GoldHEN/libkernel dieses Modul in einen
 * Payload-Prozess laedt, ist offen. AppInstUtil (Slot-Ermittlung) wird direkt
 * benutzt; ohne Initialisierung faellt der Slot auf 0 zurueck.
 */

#pragma once

#include <string.h>

#include <orbis/AppInstUtil.h>
#include <orbis/Bgft.h>
#include <orbis/libkernel.h>

#include "bgft_symbols.h"
#include "install_core.h"

struct InstallBgft
{
    int32_t handle;
    bool loaded;
    bool app_inst_ready;
    unsigned char heap[1024 * 1024];

    int32_t (*init)(OrbisBgftInitParams *params);
    int32_t (*register_storage)(OrbisBgftDownloadParamEx *param,
                                OrbisBgftTaskId *task);
    int32_t (*start)(OrbisBgftTaskId task);
    int32_t (*progress)(OrbisBgftTaskId task,
                        OrbisBgftTaskProgress *progress);
    int32_t (*stop)(OrbisBgftTaskId task);
    int32_t (*unregister)(OrbisBgftTaskId task);
};

/* Laedt das Modul und loest alle sechs Symbole auf. 0 = bereit, sonst der
   erste brauchbare Fehlercode (Modul nicht ladbar oder Symbol fehlt). */
static int32_t install_bgft_open(InstallBgft *bgft)
{
    static const char *const candidates[] = {
        "/system/common/lib/libSceBgft.sprx",
        "libSceBgft.sprx",
    };
    size_t index;
    int32_t last = -1;

    if (bgft == 0)
    {
        return -1;
    }
    if (bgft->loaded)
    {
        return 0;
    }

    for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++)
    {
        void *symbol = 0;
        int32_t handle = (int32_t)sceKernelLoadStartModule(candidates[index], 0,
                                                           0, 0, 0, 0);

        if (handle < 0)
        {
            last = handle;
            continue;
        }
        bgft->handle = handle;

        if (sceKernelDlsym(handle, ps4u_bgft_symbols[PS4U_BGFT_SYMBOL_INIT],
                           &symbol) != 0)
        {
            last = -1;
            continue;
        }
        bgft->init =
            reinterpret_cast<int32_t (*)(OrbisBgftInitParams *)>(symbol);

        if (sceKernelDlsym(handle,
                           ps4u_bgft_symbols[PS4U_BGFT_SYMBOL_REGISTER_STORAGE],
                           &symbol) != 0)
        {
            last = -1;
            continue;
        }
        bgft->register_storage = reinterpret_cast<int32_t (*)(
            OrbisBgftDownloadParamEx *, OrbisBgftTaskId *)>(symbol);

        if (sceKernelDlsym(handle, ps4u_bgft_symbols[PS4U_BGFT_SYMBOL_START],
                           &symbol) != 0)
        {
            last = -1;
            continue;
        }
        bgft->start = reinterpret_cast<int32_t (*)(OrbisBgftTaskId)>(symbol);

        if (sceKernelDlsym(handle, ps4u_bgft_symbols[PS4U_BGFT_SYMBOL_PROGRESS],
                           &symbol) != 0)
        {
            last = -1;
            continue;
        }
        bgft->progress = reinterpret_cast<int32_t (*)(
            OrbisBgftTaskId, OrbisBgftTaskProgress *)>(symbol);

        if (sceKernelDlsym(handle, ps4u_bgft_symbols[PS4U_BGFT_SYMBOL_STOP],
                           &symbol) != 0)
        {
            last = -1;
            continue;
        }
        bgft->stop = reinterpret_cast<int32_t (*)(OrbisBgftTaskId)>(symbol);

        if (sceKernelDlsym(handle,
                           ps4u_bgft_symbols[PS4U_BGFT_SYMBOL_UNREGISTER],
                           &symbol) != 0)
        {
            last = -1;
            continue;
        }
        bgft->unregister =
            reinterpret_cast<int32_t (*)(OrbisBgftTaskId)>(symbol);

        bgft->loaded = true;
        return 0;
    }

    return last;
}

/* Bei Updates liefert AppInstUtil den vorhandenen Slot; sonst 0. */
static uint32_t install_bgft_primary_slot(InstallBgft *bgft,
                                          const char *title_id)
{
    int32_t slot = -1;

    if (bgft == 0)
    {
        return 0;
    }
    if (!bgft->app_inst_ready)
    {
        if (sceAppInstUtilInitialize() == 0)
        {
            bgft->app_inst_ready = true;
        }
    }
    if (!bgft->app_inst_ready || title_id == 0)
    {
        return 0;
    }
    if (sceAppInstUtilGetPrimaryAppSlot(title_id, &slot) == 0 && slot >= 0)
    {
        return (uint32_t)slot;
    }
    return 0;
}

static int install_bgft_init(void *ctx)
{
    InstallBgft *bgft = (InstallBgft *)ctx;
    OrbisBgftInitParams params;
    int32_t result;

    if (bgft == 0)
    {
        return -1;
    }
    result = install_bgft_open(bgft);
    if (result < 0)
    {
        return (int)result;
    }

    memset(&params, 0, sizeof(params));
    params.heap = bgft->heap;
    params.heapSize = sizeof(bgft->heap);

    return (int)bgft->init(&params);
}

static int install_bgft_register(void *ctx, const InstallRequest &request,
                                 int *task)
{
    InstallBgft *bgft = (InstallBgft *)ctx;
    OrbisBgftDownloadParamEx param;
    OrbisBgftTaskId id = -1;
    int32_t result;

    if (bgft == 0 || task == 0 || request.path == 0 || request.name == 0 ||
        request.package_type == 0)
    {
        return -1;
    }

    /*
     * BGFT erwartet fuer eine lokale Datei den Pfad, keine file://-URL.
     * Die Content-ID bleibt absichtlich leer: Sonst antwortet BGFT bei einem
     * bereits installierten Titel mit 0x80990088, bevor der Force-Pfad greift.
     * Die StorageEx-Variante bekommt den vorhandenen Ziel-Slot.
     */
    memset(&param, 0, sizeof(param));
    param.params.userId = request.user_id;
    param.params.entitlementType = 5;
    param.params.id = "";
    param.params.contentUrl = request.path;
    param.params.contentName = request.name;
    param.params.iconPath = "";
    param.params.playgoScenarioId = "0";
    param.params.option = ORBIS_BGFT_TASK_OPT_DISABLE_CDN_QUERY_PARAM;
    param.params.packageType = request.package_type;
    /* Die Beispiele lassen den Subtyp leer; der Typ steht im Header. */
    param.params.packageSubType = "";
    param.params.packageSize =
        (uint32_t)(request.size > 0xFFFFFFFFull ? 0xFFFFFFFFull : request.size);
    param.slot = install_bgft_primary_slot(bgft, request.title_id);

    result = bgft->register_storage(&param, &id);
    if (result < 0)
    {
        return (int)result;
    }
    *task = (int)id;
    return 0;
}

static int install_bgft_start(void *ctx, int task)
{
    InstallBgft *bgft = (InstallBgft *)ctx;

    if (bgft == 0 || task < 0)
    {
        return -1;
    }
    return (int)bgft->start((OrbisBgftTaskId)task);
}

static int install_bgft_progress(void *ctx, int task,
                                 InstallProgress *progress)
{
    InstallBgft *bgft = (InstallBgft *)ctx;
    OrbisBgftTaskProgress raw;
    int32_t result;

    if (bgft == 0 || progress == 0 || task < 0)
    {
        return -1;
    }

    memset(&raw, 0, sizeof(raw));
    result = bgft->progress((OrbisBgftTaskId)task, &raw);
    if (result < 0)
    {
        return (int)result;
    }

    memset(progress, 0, sizeof(*progress));
    progress->bits = raw.bits;
    progress->error = raw.errorResult;
    progress->length = raw.length;
    progress->transferred = raw.transferred;
    progress->length_total = raw.lengthTotal;
    progress->transferred_total = raw.transferredTotal;
    progress->num_index = raw.numIndex;
    progress->num_total = raw.numTotal;
    progress->preparing_percent = raw.preparingPercent;
    progress->local_copy_percent = raw.localCopyPercent;
    return 0;
}

static int install_bgft_stop(void *ctx, int task)
{
    InstallBgft *bgft = (InstallBgft *)ctx;

    if (bgft == 0 || task < 0)
    {
        return -1;
    }
    return (int)bgft->stop((OrbisBgftTaskId)task);
}

static int install_bgft_unregister(void *ctx, int task)
{
    InstallBgft *bgft = (InstallBgft *)ctx;

    if (bgft == 0 || task < 0)
    {
        return -1;
    }
    return (int)bgft->unregister((OrbisBgftTaskId)task);
}

static const InstallOps *install_bgft_ops(InstallBgft *bgft)
{
    static InstallOps ops;

    ops.name = "bgft";
    ops.init = install_bgft_init;
    ops.register_task = install_bgft_register;
    ops.start = install_bgft_start;
    ops.progress = install_bgft_progress;
    ops.stop = install_bgft_stop;
    ops.unregister = install_bgft_unregister;
    ops.ctx = bgft;
    return &ops;
}
