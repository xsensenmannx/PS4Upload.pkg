/*
 * Hosttest fuer ps4app/install_core.h: die Zustandsmaschine der Installation
 * mit einem Fake-Backend. Kein Sony-Code, keine Uhr, kein Warten - die Zeit
 * kommt als Parameter, die Datei liegt im Arbeitsverzeichnis des Laufs.
 */

#include <stdio.h>
#include <string.h>

#include "install_core.h"

static int checks;
static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

struct Fake
{
    int init_result;
    int register_result;
    int start_result;
    int progress_result;
    int stop_result;
    int unregister_result;

    int init_calls;
    int register_calls;
    int start_calls;
    int progress_calls;
    int stop_calls;
    int unregister_calls;

    int task_seen;
    int next_error;
    unsigned long long done;
    unsigned long long total;
    unsigned long long length_only;
    unsigned long long transferred_only;
    unsigned int bits;
    int preparing;
    int local_copy;

    char seen_name[32];
    char seen_title_id[16];
    char seen_package_type[8];
    int seen_user;
    unsigned long long seen_size;
};

static Fake g_fake;
static InstallController g_state;
static InstallCore g_core;
static int g_log_calls;
static int g_last_log_code;
static char g_last_log_stage[64];
static int g_previous_log_code;
static char g_previous_log_stage[64];

static void fake_reset(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.task_seen = -1;
}

static int fake_init(void *ctx)
{
    Fake *fake = (Fake *)ctx;

    fake->init_calls++;
    return fake->init_result;
}

static int fake_register(void *ctx, const InstallRequest &request, int *task)
{
    Fake *fake = (Fake *)ctx;

    fake->register_calls++;
    snprintf(fake->seen_name, sizeof(fake->seen_name), "%s",
             request.name != 0 ? request.name : "");
    snprintf(fake->seen_title_id, sizeof(fake->seen_title_id), "%s",
             request.title_id != 0 ? request.title_id : "");
    snprintf(fake->seen_package_type, sizeof(fake->seen_package_type), "%s",
             request.package_type != 0 ? request.package_type : "");
    fake->seen_user = request.user_id;
    fake->seen_size = request.size;

    fake->task_seen = 7;
    *task = 7;
    return fake->register_result;
}

static int fake_start(void *ctx, int task)
{
    Fake *fake = (Fake *)ctx;

    fake->start_calls++;
    fake->task_seen = task;
    return fake->start_result;
}

static int fake_progress(void *ctx, int task, InstallProgress *progress)
{
    Fake *fake = (Fake *)ctx;

    fake->progress_calls++;
    fake->task_seen = task;
    if (fake->progress_result < 0)
    {
        return fake->progress_result;
    }

    progress->bits = fake->bits;
    progress->error = fake->next_error;
    progress->length = fake->length_only;
    progress->transferred = fake->transferred_only;
    progress->length_total = fake->total;
    progress->transferred_total = fake->done;
    progress->num_index = 1;
    progress->num_total = 1;
    progress->preparing_percent = fake->preparing;
    progress->local_copy_percent = fake->local_copy;
    return 0;
}

static int fake_stop(void *ctx, int task)
{
    Fake *fake = (Fake *)ctx;

    fake->stop_calls++;
    fake->task_seen = task;
    return fake->stop_result;
}

static int fake_unregister(void *ctx, int task)
{
    Fake *fake = (Fake *)ctx;

    fake->unregister_calls++;
    fake->task_seen = task;
    return fake->unregister_result;
}

static const InstallOps g_fake_ops = {
    "fake",
    fake_init,
    fake_register,
    fake_start,
    fake_progress,
    fake_stop,
    fake_unregister,
    (void *)&g_fake,
};

static void test_log(void *log_ctx, const char *stage, int code)
{
    (void)log_ctx;
    g_log_calls++;
    snprintf(g_previous_log_stage, sizeof(g_previous_log_stage), "%s",
             g_last_log_stage);
    g_previous_log_code = g_last_log_code;
    g_last_log_code = code;
    snprintf(g_last_log_stage, sizeof(g_last_log_stage), "%s",
             stage != 0 ? stage : "");
}

static void core_setup(bool inited)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.inited = inited;
    g_state.task = -1;
    g_core.state = &g_state;
    g_core.ops = &g_fake_ops;
    g_core.log = test_log;
    g_core.log_ctx = 0;
    g_log_calls = 0;
    g_last_log_code = 0;
    g_last_log_stage[0] = 0;
    g_previous_log_code = 0;
    g_previous_log_stage[0] = 0;
}

static void make_file(const char *path)
{
    FILE *file = fopen(path, "wb");

    if (file != 0)
    {
        fputs("pkg", file);
        fclose(file);
    }
}

static bool file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (file == 0)
    {
        return false;
    }
    fclose(file);
    return true;
}

static InstallRequest make_request(const char *path)
{
    InstallRequest request;

    request.path = path;
    request.name = "game.pkg";
    request.title = "PS4Upload Hosttest";
    request.title_id = "BREW00090";
    request.content_id = "IV0000-BREW00090_00-PS4UPLOAD0000000";
    request.package_type = "PS4GD";
    request.size = 4096;
    request.user_id = 1000;
    return request;
}

/* Voller Durchlauf: registrieren, starten, Fortschritt bis 100 %. */
static void test_success(void)
{
    const char *path = "ok.pkg";
    InstallRequest request;

    fake_reset();
    make_file(path);
    core_setup(false);
    request = make_request(path);
    install_core_begin(&g_core, request);

    CHECK(g_state.phase == INSTALL_PHASE_RUNNING);
    CHECK(g_state.percent == 0);
    CHECK(g_state.task == 7);
    CHECK(g_fake.init_calls == 1);
    CHECK(g_fake.register_calls == 1);
    CHECK(g_fake.start_calls == 1);
    CHECK(g_fake.task_seen == 7);
    CHECK(strcmp(g_state.path, path) == 0);
    CHECK(strcmp(g_state.title, "PS4Upload Hosttest") == 0);
    CHECK(strcmp(g_state.content_id,
                 "IV0000-BREW00090_00-PS4UPLOAD0000000") == 0);
    CHECK(strcmp(g_fake.seen_name, "game.pkg") == 0);
    CHECK(strcmp(g_fake.seen_title_id, "BREW00090") == 0);
    CHECK(strcmp(g_fake.seen_package_type, "PS4GD") == 0);
    CHECK(g_fake.seen_user == 1000);
    CHECK(g_fake.seen_size == 4096);
    CHECK(install_core_busy(&g_core));
    CHECK(strcmp(install_core_phase_name(g_state.phase), "INSTALLATION") == 0);

    g_fake.total = 1000;
    g_fake.done = 0;
    install_core_poll(&g_core, 1000);
    CHECK(g_fake.progress_calls == 1);
    CHECK(g_state.phase == INSTALL_PHASE_RUNNING);
    CHECK(g_state.percent == 0);

    /* Naechster Poll erst nach INSTALL_POLL_INTERVAL_MS. */
    g_fake.done = 500;
    install_core_poll(&g_core, 1200);
    CHECK(g_fake.progress_calls == 1);
    install_core_poll(&g_core, 1500);
    CHECK(g_fake.progress_calls == 2);
    CHECK(g_state.percent == 50);
    CHECK(install_core_busy(&g_core));

    g_fake.done = 1000;
    install_core_poll(&g_core, 2000);
    CHECK(g_state.phase == INSTALL_PHASE_DONE);
    CHECK(g_state.percent == 100);
    CHECK(!file_exists(path));
    CHECK(g_state.path[0] == 0);
    CHECK(!install_core_busy(&g_core));
    CHECK(strcmp(install_core_phase_name(g_state.phase), "FERTIG") == 0);
    /* Erst der Task-Abschluss mit der Task-Nummer, dann das Aufraeumen. */
    CHECK(g_previous_log_code == 7);
    CHECK(strcmp(g_previous_log_stage, "BGFT fertig, Task") == 0);
    CHECK(g_last_log_code == 0);
    CHECK(strcmp(g_last_log_stage, "PKG nach Installation aufgeraeumt") == 0);
}

/* Init scheitert: kein Register, keine Datei mehr, Phase FEHLER. */
static void test_init_failure(void)
{
    const char *path = "init_fail.pkg";
    InstallRequest request;

    fake_reset();
    g_fake.init_result = -2147418112;
    make_file(path);
    core_setup(false);
    request = make_request(path);
    install_core_begin(&g_core, request);

    CHECK(g_state.phase == INSTALL_PHASE_FAILED);
    CHECK(g_state.error == -2147418112);
    CHECK(strcmp(g_state.note, "BGFT-Init") == 0);
    CHECK(g_fake.register_calls == 0);
    CHECK(g_fake.start_calls == 0);
    CHECK(g_fake.stop_calls == 0);
    CHECK(!g_state.inited);
    CHECK(!file_exists(path));
}

static void test_register_failure(void)
{
    const char *path = "register_fail.pkg";
    InstallRequest request;

    fake_reset();
    g_fake.register_result = -3;
    make_file(path);
    core_setup(false);
    request = make_request(path);
    install_core_begin(&g_core, request);

    CHECK(g_state.phase == INSTALL_PHASE_FAILED);
    CHECK(g_state.error == -3);
    CHECK(strcmp(g_state.note, "BGFT-Registrierung") == 0);
    CHECK(g_fake.start_calls == 0);
    CHECK(g_fake.stop_calls == 0);
    CHECK(g_fake.unregister_calls == 0);
    CHECK(g_state.task == -1);
    CHECK(!file_exists(path));
    CHECK(g_state.inited);
}

/* Start scheitert: der registrierte Task wird wieder abgemeldet. */
static void test_start_failure(void)
{
    const char *path = "start_fail.pkg";
    InstallRequest request;

    fake_reset();
    g_fake.start_result = -5;
    make_file(path);
    core_setup(false);
    request = make_request(path);
    install_core_begin(&g_core, request);

    CHECK(g_state.phase == INSTALL_PHASE_FAILED);
    CHECK(g_state.error == -5);
    CHECK(strcmp(g_state.note, "BGFT-Start") == 0);
    CHECK(g_fake.stop_calls == 1);
    CHECK(g_fake.unregister_calls == 1);
    CHECK(g_fake.task_seen == 7);
    CHECK(g_state.task == -1);
    CHECK(!file_exists(path));
}

static void test_progress_failure(void)
{
    const char *path = "progress_fail.pkg";
    InstallRequest request;

    fake_reset();
    g_fake.progress_result = -9;
    make_file(path);
    core_setup(false);
    request = make_request(path);
    install_core_begin(&g_core, request);
    install_core_poll(&g_core, 1000);

    CHECK(g_state.phase == INSTALL_PHASE_FAILED);
    CHECK(g_state.error == -9);
    CHECK(strcmp(g_state.note, "BGFT-Fortschritt") == 0);
    CHECK(g_fake.stop_calls == 1);
    CHECK(g_fake.unregister_calls == 1);
    CHECK(!file_exists(path));
}

/* BGFT meldet im Fortschritt einen Fehler (z. B. Signaturpruefung). */
static void test_progress_error(void)
{
    const char *path = "progress_error.pkg";
    InstallRequest request;

    fake_reset();
    g_fake.next_error = -7;
    make_file(path);
    core_setup(false);
    request = make_request(path);
    install_core_begin(&g_core, request);
    install_core_poll(&g_core, 1000);

    CHECK(g_state.phase == INSTALL_PHASE_FAILED);
    CHECK(g_state.error == -7);
    CHECK(strcmp(g_state.note, "BGFT-Installation") == 0);
    CHECK(!file_exists(path));
}

/* Gleicher Fortschritt ueber die Timeout-Grenze gilt als Stillstand. */
static void test_stall(void)
{
    const char *path = "stall.pkg";
    InstallRequest request;
    unsigned long long now = 1000;

    fake_reset();
    make_file(path);
    core_setup(false);
    request = make_request(path);
    install_core_begin(&g_core, request);

    while (g_state.phase == INSTALL_PHASE_RUNNING && now <= 400000ull)
    {
        install_core_poll(&g_core, now);
        now += 1000;
    }

    CHECK(g_state.phase == INSTALL_PHASE_FAILED);
    CHECK(strcmp(g_state.note, "BGFT-Stillstand") == 0);
    CHECK(g_state.error == 0);
    CHECK(g_fake.stop_calls == 1);
    CHECK(!file_exists(path));
}

/* Prozent kommen notfalls aus length/transferred statt aus den Total-Feldern. */
static void test_percent_sources(void)
{
    const char *path = "percent.pkg";
    InstallRequest request;

    fake_reset();
    make_file(path);
    core_setup(false);
    request = make_request(path);
    install_core_begin(&g_core, request);

    g_fake.total = 0;
    g_fake.length_only = 1000;
    g_fake.transferred_only = 250;
    install_core_poll(&g_core, 1000);
    CHECK(g_state.percent == 25);
    CHECK(g_state.phase == INSTALL_PHASE_RUNNING);

    /* preparing/localCopy heben den Prozentwert, werden aber bei 100 gedeckelt. */
    g_fake.total = 1000;
    g_fake.done = 100;
    g_fake.preparing = 60;
    install_core_poll(&g_core, 2000);
    CHECK(g_state.percent == 60);

    g_fake.local_copy = 120;
    install_core_poll(&g_core, 3000);
    CHECK(g_state.percent == 100);
    CHECK(g_state.phase == INSTALL_PHASE_RUNNING);

    /* Ohne Gesamtgroesse gibt es kein "fertig". */
    g_fake.total = 0;
    g_fake.length_only = 0;
    g_fake.transferred_only = 0;
    g_fake.preparing = 0;
    g_fake.local_copy = 0;
    install_core_poll(&g_core, 4000);
    CHECK(g_state.phase == INSTALL_PHASE_RUNNING);
}

static void test_reset_and_names(void)
{
    const char *path = "reset.pkg";
    InstallRequest request;
    char long_name[120];

    fake_reset();
    make_file(path);
    core_setup(false);
    request = make_request(path);

    install_core_begin(&g_core, request);
    CHECK(g_fake.init_calls == 1);
    CHECK(g_state.inited);

    install_core_mark_check(&g_core);
    CHECK(g_state.phase == INSTALL_PHASE_CHECK);
    CHECK(g_state.percent == -1);
    CHECK(!install_core_busy(&g_core));

    install_core_mark_upload(&g_core, 25, 100);
    CHECK(g_state.phase == INSTALL_PHASE_UPLOAD);
    CHECK(g_state.percent == 25);

    install_core_mark_failed_note(&g_core, -4, "DATEI NICHT ANLEGBAR");
    CHECK(g_state.phase == INSTALL_PHASE_FAILED);
    CHECK(g_state.percent == -1);
    CHECK(strcmp(g_state.note, "DATEI NICHT ANLEGBAR") == 0);

    install_core_mark_failed(&g_core, -6);
    CHECK(g_state.error == -6);

    /* reset behaelt die Backend-Initialisierung, leert aber den Rest. */
    install_core_reset(&g_core, "neu.pkg");
    CHECK(g_state.inited);
    CHECK(g_state.phase == INSTALL_PHASE_IDLE);
    CHECK(g_state.percent == -1);
    CHECK(g_state.task == -1);
    CHECK(g_state.error == 0);
    CHECK(g_state.path[0] == 0);
    CHECK(g_state.note[0] == 0);
    CHECK(strcmp(g_state.name, "neu.pkg") == 0);

    install_core_begin(&g_core, request);
    CHECK(g_fake.init_calls == 1); /* kein zweites Init */

    /* Zu lange Namen werden beschnitten, nicht ueberlaufen. */
    memset(long_name, 'A', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    install_core_reset(&g_core, long_name);
    CHECK(strlen(g_state.name) == sizeof(g_state.name) - 1);
}

/* Ohne Backend (oder mit fehlendem Init) endet der Versuch sauber. */
static void test_backend_missing(void)
{
    const char *path = "no_backend.pkg";
    InstallRequest request;

    fake_reset();
    make_file(path);
    core_setup(false);
    g_core.ops = 0;
    request = make_request(path);
    install_core_begin(&g_core, request);

    CHECK(g_state.phase == INSTALL_PHASE_FAILED);
    CHECK(strcmp(g_state.note, "Install-Backend") == 0);
    CHECK(!file_exists(path));

    /* Poll ohne laufenden Task tut nichts. */
    fake_reset();
    core_setup(true);
    install_core_poll(&g_core, 1000);
    CHECK(g_fake.progress_calls == 0);
}

/* Ruecksprung der Uhr blockiert den Poll nicht (kein Wrap-Deadlock). */
static void test_clock_backwards(void)
{
    const char *path = "clock.pkg";
    InstallRequest request;

    fake_reset();
    make_file(path);
    core_setup(false);
    request = make_request(path);
    install_core_begin(&g_core, request);
    g_fake.total = 1000;

    install_core_poll(&g_core, 5000);
    CHECK(g_fake.progress_calls == 1);
    install_core_poll(&g_core, 500);
    CHECK(g_fake.progress_calls == 2);
    install_core_poll(&g_core, 900);
    CHECK(g_fake.progress_calls == 2);
    install_core_poll(&g_core, 1000);
    CHECK(g_fake.progress_calls == 3);
}

int main(void)
{
    test_success();
    test_init_failure();
    test_register_failure();
    test_start_failure();
    test_progress_failure();
    test_progress_error();
    test_stall();
    test_percent_sources();
    test_reset_and_names();
    test_backend_missing();
    test_clock_backwards();

    printf("%d Checks, %d Fehler\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
