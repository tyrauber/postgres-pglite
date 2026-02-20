
// for handling REVOKE exception in initdb
#if defined(__wasi__) || defined(__EMSCRIPTEN__)
#define FIXME 1
#else
#define FIXME 0
#endif

#define PGL_MAIN
#define PGL_INITDB_MAIN
#define REPL 0
// #define PGDEBUG_STARTUP

// MEMFS files for os pipe simulation
#define IDB_PIPE_BOOT "/tmp/initdb.boot.txt"
#define IDB_PIPE_SINGLE "/tmp/initdb.single.txt"

#include "pgl_os.h"

// ============================================================================
// Debug logging control - disabled by default for performance
// Build with -DPGL_VERBOSE_LOGGING to enable all debug output
// ============================================================================
#ifndef PGL_VERBOSE_LOGGING
#define PGL_DEBUG_PRINT(...) ((void)0)
#define PGL_DEBUG_FLUSH(s) ((void)0)
#else
#define PGL_DEBUG_PRINT(...) fprintf(__VA_ARGS__)
#define PGL_DEBUG_FLUSH(s) fflush(s)
#endif

#ifdef PGL_MOBILE
#include <dirent.h>  /* For directory listing in debug code */
#include <unistd.h>  /* For unlink, rmdir */

/**
 * Recursively remove a directory and all its contents.
 * Returns 0 on success, -1 on failure.
 */
static int pgl_remove_directory_recursive(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        /* If it doesn't exist, that's success */
        if (errno == ENOENT) return 0;
        return -1;
    }

    struct dirent *entry;
    char child_path[2048];
    int result = 0;

    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);

        struct stat st;
        if (lstat(child_path, &st) == -1) {
            result = -1;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            /* Recursively remove subdirectory */
            if (pgl_remove_directory_recursive(child_path) == -1) {
                result = -1;
            }
        } else {
            /* Remove file */
            if (unlink(child_path) == -1) {
                result = -1;
            }
        }
    }

    closedir(dir);

    /* Remove the directory itself */
    if (rmdir(path) == -1) {
        result = -1;
    }

    return result;
}
#endif

// ============================================================================
// Log Callback System - Global State and API
// ============================================================================
// These are declared extern in pgl_os.h and defined here so they're exported
// from libpgcore_mobile.a for use by React Native, WASM, etc.

pgl_log_callback_t pgl_log_callback = NULL;
int pgl_log_min_level = PGL_LOG_LEVEL_DEBUG1; // Default: show DEBUG1 and above

void pgl_set_log_callback(pgl_log_callback_t callback)
{
    pgl_log_callback = callback;
}

void pgl_set_log_level(int level)
{
    pgl_log_min_level = level;
}

int pgl_get_log_level(void)
{
    return pgl_log_min_level;
}

// ============================================================================
// Global variables for IDB pipe simulation (declared extern in pgl_os.h)
// ============================================================================
FILE *IDB_PIPE_FP = NULL;
int IDB_STAGE = 0;

// ============================================================================

#ifdef __ANDROID__
#include <android/log.h>
#include <pthread.h>
#include <fcntl.h>
static int pgl_stderr_pipe[2] = {-1, -1};
static pthread_t pgl_stderr_thread;
static void *pgl_stderr_reader(void *arg)
{
    (void)arg;
    if (pgl_stderr_pipe[0] < 0)
    {
        PGL_LOG_ERROR("[pgl_stderr_reader] Invalid pipe fd, exiting thread");
        return NULL;
    }
    PGL_LOG_INFO("[pgl_stderr_reader] Thread started, entering read loop");
    char buf[1024];
    for (;;)
    {
        ssize_t n = read(pgl_stderr_pipe[0], buf, sizeof(buf) - 1);
        if (n > 0)
        {
            buf[n] = '\0';
            PGL_LOG_INFO("%s", buf);
            continue;
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                usleep(100000); // 100ms sleep to reduce spam
                continue;
            }
            PGL_LOG_ERROR("[pgl_stderr_reader] Read error errno=%d, breaking", errno);
            break;
        }
        // n == 0: pipe closed
        PGL_LOG_INFO("[pgl_stderr_reader] Pipe closed (EOF), exiting thread");
        break;
    }
    PGL_LOG_INFO("[pgl_stderr_reader] Thread exiting");
    return NULL;
}
static void pgl_install_android_stderr_redirect(void)
{
    if (pgl_stderr_pipe[0] >= 0)
    {
        PGL_DEBUG("[pgl_install_android_stderr_redirect] Already installed, skipping");
        return;
    }
    PGL_LOG_INFO("[pgl_install_android_stderr_redirect] Installing stderr redirect");
    if (pipe(pgl_stderr_pipe) == 0)
    {
        PGL_LOG_INFO("[pgl_install_android_stderr_redirect] Pipe created: read_fd=%d write_fd=%d", pgl_stderr_pipe[0], pgl_stderr_pipe[1]);

        // Make read end non-blocking to prevent hangs
        int flags = fcntl(pgl_stderr_pipe[0], F_GETFL);
        if (fcntl(pgl_stderr_pipe[0], F_SETFL, flags | O_NONBLOCK) < 0)
        {
            PGL_LOG_ERROR("[pgl_install_android_stderr_redirect] Failed to set non-blocking: errno=%d", errno);
        }
        else
        {
            PGL_LOG_INFO("%s", "[pgl_install_android_stderr_redirect] Set read end to non-blocking");
        }

        // Make stderr unbuffered and redirect
        setvbuf(stderr, NULL, _IONBF, 0);
        if (dup2(pgl_stderr_pipe[1], STDERR_FILENO) < 0)
        {
            PGL_LOG_ERROR("[pgl_install_android_stderr_redirect] dup2 failed: errno=%d", errno);
        }
        else
        {
            PGL_LOG_INFO("%s", "[pgl_install_android_stderr_redirect] stderr redirected to pipe");
        }

        if (pthread_create(&pgl_stderr_thread, NULL, pgl_stderr_reader, NULL) != 0)
        {
            PGL_LOG_ERROR("[pgl_install_android_stderr_redirect] pthread_create failed: errno=%d", errno);
        }
        else
        {
            PGL_LOG_INFO("%s", "[pgl_install_android_stderr_redirect] Reader thread created");
        }
    }
    else
    {
        PGL_LOG_ERROR("[pgl_install_android_stderr_redirect] pipe() failed: errno=%d", errno);
    }
}
#endif

// ----------------------- pglite ----------------------------
#include "postgres.h"
#include "utils/elog.h"

#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "tcop/tcopprot.h"
#include "lib/stringinfo.h"

#ifdef PGL_MOBILE
#include "storage/shmem.h"
#include "storage/proc.h"
#endif

/* Temporary log hook to surface bootstrap errors into stderr in C (no lambdas) */
static void pgl_boot_emit_hook(ErrorData *ed)
{
    if (ed && ed->elevel >= ERROR)
    {
        PGL_DEBUG_PRINT(stderr, "[pgl_boot] %s:%d %s: %s\n",
                ed->filename ? ed->filename : "?",
                ed->lineno,
                ed->funcname ? ed->funcname : "?",
                ed->message ? ed->message : "");
    }
}

#include <unistd.h>   /* chdir */
#include <sys/stat.h> /* mkdir */

// globals

#define MemoryContextResetAndDeleteChildren(...)
// #define SpinLockInit(...)

int g_argc;
char **g_argv;
extern char **environ;

volatile char *PREFIX;
volatile char *PGDATA;
volatile char *PGUSER;

const char *progname;

#ifdef PGL_MOBILE
/* Mobile: defined in sdk_port-mobile.c */
extern volatile bool is_repl;
extern volatile bool pgl_archiving_enabled;
#else
/* WASM: define here */
volatile bool is_repl = true;
#endif
volatile bool is_node = true;
volatile bool is_embed = false;
volatile int pgl_idb_status;

// now backend start manually after initdb.
// TODO: log sync start failures and ask to repair/clean up db.
volatile int async_restart = 1;

#ifdef PGL_MOBILE
/*
 * Track whether PostgreSQL backend has been fully initialized in this process.
 * This is separate from async_restart (which tracks if DB files exist).
 * 
 * On mobile, we may call pgl_backend() multiple times in the same process
 * (e.g., test harness, or app re-initializing after error). We need to know
 * if the backend is already running to avoid re-initialization issues.
 */
static volatile bool pgl_backend_initialized = false;
#endif

#define IDB_OK 0b11111110
#define IDB_FAILED 0b0001
#define IDB_CALLED 0b0010
#define IDB_HASDB 0b0100
#define IDB_HASUSER 0b1000

#define WASM_PGDATA WASM_PREFIX "/base"
#define CMA_FD 1

#include <setjmp.h>
extern bool IsPostmasterEnvironment;

/*
 * Error handling for mobile platforms (iOS/Android).
 *
 * PostgreSQL's proc_exit() normally calls exit() which would terminate the host app.
 * On mobile, we intercept proc_exit() via pgl_boot_jmp to longjmp back to a safe point.
 *
 * CRITICAL: We use STATIC jump buffers, not stack-local ones. Stack-local buffers
 * become invalid when the function returns, causing crashes if proc_exit() is called
 * later (e.g., during query execution when an error triggers proc_exit).
 *
 * There are three contexts where we need to catch proc_exit:
 * 1. Bootstrap mode (BootstrapModeMain) - during initdb
 * 2. Shared memory initialization - when PgStartTime == 0
 * 3. Backend initialization - AsyncPostgresSingleUserMain
 *
 * During normal query execution, pgl_boot_jmp should be NULL. PostgreSQL's normal
 * error handling (PG_exception_stack / sigsetjmp in pgl_sjlj.c) handles query errors.
 * Only fatal errors that call proc_exit() need the pgl_boot_jmp mechanism.
 */

/* Static jump buffers - these remain valid for the lifetime of the process */
static sigjmp_buf pgl_boot_jmp_buf;      /* For BootstrapModeMain */
static sigjmp_buf pgl_shmem_jmp_buf;     /* For shared memory init */
static sigjmp_buf pgl_backend_jmp_buf;   /* For AsyncPostgresSingleUserMain */

/* Global pointer to the currently active jump buffer (or NULL if none) */
volatile sigjmp_buf *pgl_boot_jmp = NULL;

/* Track which context we're in for debugging */
typedef enum {
    PGL_JMP_NONE = 0,
    PGL_JMP_BOOT,
    PGL_JMP_SHMEM,
    PGL_JMP_BACKEND
} PglJmpContext;
static volatile PglJmpContext pgl_jmp_context = PGL_JMP_NONE;

#define help(name)
#ifdef __ANDROID__
#include <android/log.h>
#include "utils/elog.h"
static void pgl_android_elog_hook(ErrorData *ed)
{
    if (!ed)
        return;
    int prio = ANDROID_LOG_INFO;
    if (ed->elevel >= ERROR)
        prio = ANDROID_LOG_ERROR;
    else if (ed->elevel >= WARNING)
        prio = ANDROID_LOG_WARN;
    else if (ed->elevel >= DEBUG1)
        prio = ANDROID_LOG_DEBUG;
    switch (prio)
    {
    case ANDROID_LOG_ERROR:
        PGL_LOG_ERROR("%s:%d %s: %s",
                      ed->filename ? ed->filename : "?",
                      ed->lineno,
                      ed->funcname ? ed->funcname : "?",
                      ed->message ? ed->message : "");
        break;
    case ANDROID_LOG_WARN:
        PGL_LOG_WARN("%s:%d %s: %s",
                     ed->filename ? ed->filename : "?",
                     ed->lineno,
                     ed->funcname ? ed->funcname : "?",
                     ed->message ? ed->message : "");
        break;
    case ANDROID_LOG_DEBUG:
        PGL_DEBUG("%s:%d %s: %s",
                  ed->filename ? ed->filename : "?",
                  ed->lineno,
                  ed->funcname ? ed->funcname : "?",
                  ed->message ? ed->message : "");
        break;
    default:
        PGL_LOG_INFO("%s:%d %s: %s",
                     ed->filename ? ed->filename : "?",
                     ed->lineno,
                     ed->funcname ? ed->funcname : "?",
                     ed->message ? ed->message : "");
        break;
    }
}
#endif

#define BREAKV(x)                          \
    {                                      \
        printf("BREAKV : %d\n", __LINE__); \
        return x;                          \
    }
#define BREAK                             \
    {                                     \
        printf("BREAK : %d\n", __LINE__); \
        return;                           \
    }

extern int pgl_initdb_main(void);
extern void pg_proc_exit(int code);
extern int BootstrapModeMain(int, char **, int);

// PostgresSingleUserMain / PostgresMain

#include "miscadmin.h"
#include "utils/timeout.h"
#include "access/xlog.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "utils/timestamp.h"
#include "utils/guc.h"
#include "pgstat.h"
#include "replication/walsender.h"
#include "libpq/pqformat.h"

volatile bool send_ready_for_query = true;
volatile bool idle_in_transaction_timeout_enabled = false;
volatile bool idle_session_timeout_enabled = false;

void pg_free(void *ptr)
{
    free(ptr);
}

#include "../backend/tcop/postgres.c"

// initdb + start on fd (pipe emulation)

#ifdef __ANDROID__
static void pgl_install_android_stderr_redirect_wrapper(void) { pgl_install_android_stderr_redirect(); }
static void __attribute__((constructor)) pgl_install_android_stderr_redirect_ctor(void) { pgl_install_android_stderr_redirect_wrapper(); }
#endif

static bool force_echo = false;

#include "pgl_mains.c"

#include "pgl_stubs.h"

#include "pgl_tools.h"

#include "pgl_initdb.c"

// interactive_one, heart of the async loop.
// On mobile, avoid calling pq_init() which expects a real socket; we operate in-process
// Return the existing MyProcPort so sites that assign from pq_init() remain valid

// Install mobile comm methods early when building for mobile
#ifdef PGL_MOBILE
extern void pgl_install_mobile_comm(void);
#endif

#include "./interactive_one.c"

static void
main_pre(int argc, char *argv[])
{
#ifdef __ANDROID__
    /* Ensure stderr is redirected to logcat early */
    pgl_install_android_stderr_redirect();
#endif

    char key[256];
    int i = 0;
    // extra env is always after normal args
    PDEBUG("# ============= extra argv dump ==================\n");
    {
        for (; i < argc; i++)
        {
            const char *kv = argv[i];
            for (int sk = 0; sk < strlen(kv); sk++)
                if (kv[sk] == '=')
                    goto extra_env;
#if PGDEBUG
            printf("arg[%d]: %s\n", i, kv);
#endif
        }
    }
extra_env:;
    PDEBUG("\n# ============= arg->env dump ==================\n");
    {
        for (; i < argc; i++)
        {
            const char *kv = argv[i];
            for (int sk = 0; sk < strlen(kv); sk++)
            {
                if (sk > 255)
                {
                    puts("buffer overrun on extra env at:");
                    puts(kv);
                    continue;
                }
                if (kv[sk] == '=')
                {
                    memcpy(key, kv, sk);
                    key[sk] = 0;
#if PGDEBUG
                    printf("%s='%s'\n", &(key[0]), &(kv[sk + 1]));
#endif
                    setenv(key, &kv[sk + 1], 1);
                }
            }
        }
    }

    // get default or set default if not set
    PREFIX = setdefault("PREFIX", WASM_PREFIX);
    PGL_DEBUG_PRINT(stderr, "[pgl_main] PREFIX=%s PGDATA=%s PGSYSCONFDIR=%s ANDROID_RUNTIME_DIR=%s\n", PREFIX, PGDATA ? PGDATA : "", getenv("PGSYSCONFDIR") ? getenv("PGSYSCONFDIR") : "", getenv("ANDROID_RUNTIME_DIR") ? getenv("ANDROID_RUNTIME_DIR") : "");
    argv[0] = strcat_alloc(PREFIX, "/bin/postgres");

#if defined(__EMSCRIPTEN__)
    EM_ASM({
           Module.is_worker = (typeof WorkerGlobalScope !== 'undefined') && self instanceof WorkerGlobalScope;
           Module.FD_BUFFER_MAX = $0; Module.emscripten_copy_to = console.warn; }, (CMA_MB * 1024 * 1024) / CMA_FD); /* ( global mem start / num fd max ) */

    if (is_node)
    {
        setenv("ENVIRONMENT", "node", 1);
        EM_ASM({
#if defined(PGDEBUG_STARTUP)
               console.warn("prerun(C-node) worker=", Module.is_worker);
#endif
               Module['postMessage'] = function custom_postMessage(event) {
               console.log("# pg_main_emsdk.c:544: onCustomMessage:", event);}; });
    }
    else
    {
        setenv("ENVIRONMENT", "web", 1);
#if defined(PGDEBUG_STARTUP)
        EM_ASM({ console.warn("prerun(C-web) worker=", Module.is_worker); });
#endif
        is_repl = true;
    }
    // *INDENT-OFF*
    EM_ASM({
        if (Module.is_worker)
        {
#if defined(PGDEBUG_STARTUP)
            console.log("Main: running in a worker, setting onCustomMessage");
#endif
            function onCustomMessage(event)
            {
                console.log("onCustomMessage:", event);
            };
            Module['onCustomMessage'] = onCustomMessage;
        }
        else
        {
#if defined(PGDEBUG_STARTUP)
            console.log("Running in main thread, faking onCustomMessage");
#endif
            Module['postMessage'] = function custom_postMessage(event)
            {
                switch (event.type)
                {
                case "raw":
                {
                    // stringToUTF8( event.data, shm_rawinput, Module.FD_BUFFER_MAX);
                    break;
                }

                case "stdin":
                {
                    stringToUTF8(event.data, 1, Module.FD_BUFFER_MAX);
                    break;
                }
                case "rcon":
                {
                    // stringToUTF8( event.data, shm_rcon, Module.FD_BUFFER_MAX);
                    break;
                }
                default:
                    console.warn("custom_postMessage?", event);
                }
            };
            // if (!window.vm)
            //   window.vm = Module;
        };
    });
// *INDENT-ON*
#endif // __EMSCRIPTEN__
    chdir("/");
    mkdirp("/tmp");
    mkdirp(PREFIX);

    // postgres does not know where to find the server configuration file.
    // also we store the fake locale file there.
    // postgres.js:1605 You must specify the --config-file or -D invocation option or set the PGDATA environment variable.

    /* enforce ? */
    setenv("PGSYSCONFDIR", PREFIX, 1);
    setenv("PGCLIENTENCODING", "UTF8", 1);

    // default now is no repl loop
    setenv("REPL", "N", 0);

    /*
     * we cannot run "locale -a" either from web or node. the file getenv("PGSYSCONFDIR") / "locale"
     * serves as popen output
     */
    setenv("LC_CTYPE", "en_US.UTF-8", 1);

    /* defaults */

    setenv("TZ", "UTC", 0);
    setenv("PGTZ", "UTC", 0);
    setenv("PGDATABASE", "template1", 0);
    setenv("PG_COLOR", "always", 0);

    /* defaults with possible user setup */
    PGUSER = setdefault("PGUSER", WASM_USERNAME);

    /* temp override for inidb */
    setenv("PGUSER", WASM_USERNAME, 1);

    strconcat(tmpstr, PREFIX, "/base");
    PGDATA = setdefault("PGDATA", tmpstr);

#if PGDEBUG
    puts("# ============= env dump ==================");
    for (char **env = environ; *env != 0; env++)
    {
        char *drefp = *env;
        printf("# %s\n", drefp);
    }
    puts("# =========================================");
#endif
} // main_pre

void main_post()
{
    PGL_LOG_ERROR("[main_post] *** ENTRY: main_post() function called ***");
    PDEBUG("# 280: main_post()");
    /*
     * Fire up essential subsystems: error and memory management
     *
     * Code after this point is allowed to use elog/ereport, though
     * localization of messages may not work right away, and messages won't go
     * anywhere but stderr until GUC settings get loaded.
     */
    PGL_LOG_ERROR("[main_post] *** About to call MemoryContextInit() ***");
    MemoryContextInit();
    PGL_LOG_ERROR("[main_post] *** MemoryContextInit() completed ***");

    /*
     * Set up locale information
     */
    /* Guard against NULL g_argv when running as a library (mobile) */
    const char *argv0_local = NULL;
    PGL_LOG_ERROR("[main_post] *** About to determine argv0_local, g_argv=%p ***", (void *)g_argv);
    if (g_argv && g_argv[0] && g_argv[0][0])
    {
        argv0_local = g_argv[0];
        PGL_LOG_ERROR("[main_post] *** Using g_argv[0]: %s ***", argv0_local);
    }
    else
    {
        static char __argv0_buf[STROPS_BUF];
        const char *pr = (PREFIX && ((const char *)PREFIX)[0]) ? (const char *)PREFIX : WASM_PREFIX;
        strconcat(__argv0_buf, pr, "/bin/postgres");
        argv0_local = __argv0_buf;
        PGL_DEBUG_PRINT(stderr, "[pgl_main] main_post fallback argv0=%s (g_argv missing)\n", argv0_local);
        PGL_LOG_ERROR("[main_post] *** Using fallback argv0: %s ***", argv0_local);
    }
    PGL_LOG_ERROR("[main_post] *** About to call set_pglocale_pgservice with argv0=%s ***", argv0_local ? argv0_local : "NULL");
    set_pglocale_pgservice(argv0_local, PG_TEXTDOMAIN("postgres"));
    PGL_LOG_ERROR("[main_post] *** set_pglocale_pgservice completed ***");

    /*
     * In the postmaster, absorb the environment values for LC_COLLATE and
     * LC_CTYPE.  Individual backends will change these later to settings
     * taken from pg_database, but the postmaster cannot do that.  If we leave
     * these set to "C" then message localization might not work well in the
     * postmaster.
     */
    init_locale("LC_COLLATE", LC_COLLATE, "");
    init_locale("LC_CTYPE", LC_CTYPE, "");

    /*
     * LC_MESSAGES will get set later during GUC option processing, but we set
     * it here to allow startup error messages to be localized.
     */
#ifdef LC_MESSAGES
    init_locale("LC_MESSAGES", LC_MESSAGES, "");
#endif

    /*
     * We keep these set to "C" always, except transiently in pg_locale.c; see
     * that file for explanations.
     */
    init_locale("LC_MONETARY", LC_MONETARY, "C");
    init_locale("LC_NUMERIC", LC_NUMERIC, "C");
    init_locale("LC_TIME", LC_TIME, "C");

    /*
     * Now that we have absorbed as much as we wish to from the locale
     * environment, remove any LC_ALL setting, so that the environment
     * variables installed by pg_perm_setlocale have force.
     */
    unsetenv("LC_ALL");
} // main_post

#ifdef PGL_MOBILE
/* Force-link extension symbols - must be called to prevent linker dead-stripping */
extern void pgl_mobile_force_link_extensions(void);
/* Initialize extensions that require _PG_init - fixes iOS device crash */
extern void pgl_mobile_init_extensions(void);
#endif

__attribute__((export_name("pgl_backend"))) int pgl_backend()
{
    PGL_PROFILE_DECL();
    PGL_PROFILE_START();

    /* IMMEDIATE logging - before anything else */
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B001: IMMEDIATE ENTRY - function called\n");
    fflush(stderr);

#ifdef PGL_MOBILE
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B002: About to call pgl_mobile_force_link_extensions()\n");
    fflush(stderr);

    /* CRITICAL: Call this first to ensure extension symbols are linked.
     * Without this call, the linker may strip plpgsql_call_handler, citext_eq, etc.
     * because they're only referenced via function pointers in the symbol table.
     */
    pgl_mobile_force_link_extensions();

    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B003: pgl_mobile_force_link_extensions() completed\n");
    fflush(stderr);

    PGL_LOG_ERROR("%s", "[pgl_backend] B004: ENTRY: pgl_backend function called");
    PGL_LOG_ERROR("%s", "[pgl_backend] B005: This confirms we reached pgl_backend after pgl_initdb");

    /*
     * Check if backend is already initialized in this process.
     * On mobile, pgl_backend() may be called multiple times (e.g., test harness,
     * or app re-initializing). If already initialized, just return - the backend
     * is ready to process queries via interactive_one().
     */
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B006: Checking pgl_backend_initialized=%d\n", pgl_backend_initialized);
    fflush(stderr);
    if (pgl_backend_initialized)
    {
        PGL_LOG_INFO("[pgl_backend] Backend already initialized, skipping re-init");
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] B007: Backend already initialized in this process, skipping\n");
        fflush(stderr);
        return 0; /* Success - already initialized */
    }

    /*
     * MOBILE FIX: Reset all shared memory and process state before initialization.
     *
     * On mobile platforms, when the app is killed and restarted, we get a new process
     * but the database files on disk may have stale references. Additionally, if the
     * backend is being re-initialized in the same process (e.g., after an error),
     * the static pointers to shared memory structures may be stale.
     *
     * We MUST reset these before any shared memory operations to ensure clean state.
     */
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B008: Resetting shared memory and process state\n");
    fflush(stderr);

    PglMobileResetShmemState();
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B009: PglMobileResetShmemState() done\n");
    fflush(stderr);

    PglMobileResetProcState();
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B010: PglMobileResetProcState() done\n");
    fflush(stderr);
#endif
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B011: Past mobile-specific init\n");
    fflush(stderr);
#ifdef __ANDROID__
    static int pgl_android_log_inited = 0;
    if (!pgl_android_log_inited)
    {
        pgl_install_android_stderr_redirect();
        emit_log_hook = pgl_android_elog_hook; /* direct ereport to logcat */
        pgl_android_log_inited = 1;
    }
#endif
    /* Guard required env/vars even if pgl_initdb didn't set globals as expected */
    const char *pr = (PREFIX && ((const char *)PREFIX)[0]) ? (const char *)PREFIX : WASM_PREFIX;
    if (!PGUSER || !((const char *)PGUSER)[0])
        PGUSER = (volatile char *)setdefault("PGUSER", WASM_USERNAME);
    if (!PGDATA || !((const char *)PGDATA)[0])
    {
        static char __pgbuf[STROPS_BUF];
        strconcat(__pgbuf, pr, "/base");
        PGDATA = (volatile char *)setdefault("PGDATA", __pgbuf);
    }
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] guards: PREFIX=%s PGDATA=%s PGUSER=%s\n", pr, PGDATA ? (const char *)PGDATA : "", PGUSER ? (const char *)PGUSER : "");
#if PGDEBUG
    print_bits(sizeof(pgl_idb_status), &pgl_idb_status);
#endif
    if (!(pgl_idb_status & IDB_CALLED))
    {
        puts("# 336: initdb must be called before starting/resuming backend");
        // abort();
    }

#ifdef PGL_MOBILE
    /* Initialize critical globals for mobile library mode */
    if (!progname)
    {
        progname = "postgres"; // Safe fallback
    }

    /* Ensure critical environment variables are set */
    if (!getenv("PGSYSCONFDIR"))
    {
        setenv("PGSYSCONFDIR", pr, 1);
    }
    if (!getenv("PGCLIENTENCODING"))
        setenv("PGCLIENTENCODING", "UTF8", 1);
    if (!getenv("LC_CTYPE"))
        setenv("LC_CTYPE", "en_US.UTF-8", 1);
    if (!getenv("TZ"))
        setenv("TZ", "UTC", 1);
    if (!getenv("PGTZ"))
        setenv("PGTZ", "UTC", 1);
    if (!getenv("PGDATABASE"))
        setenv("PGDATABASE", "template1", 1);

    PGL_LOG_INFO("[pgl_backend] Mobile environment initialization complete");
#endif

#ifdef PGL_MOBILE
    /* Mobile communication methods will be installed after backend initialization */
    PGL_LOG_ERROR("[pgl_backend] *** Deferring mobile comm installation until backend is ready ***");

    /* MOBILE: Initialize g_argv equivalent for library mode if not already done */
    /* TEMPORARILY DISABLED - this may be causing invalidation message corruption
    if (!g_argv) {
        static char* mobile_argv[4];
        static char mobile_argv0[STROPS_BUF];
        const char *pr = (PREFIX && ((const char*)PREFIX)[0]) ? (const char*)PREFIX : WASM_PREFIX;
        strconcat(mobile_argv0, pr, "/bin/postgres");
        mobile_argv[0] = mobile_argv0;
        mobile_argv[1] = NULL;
        g_argv = mobile_argv;
        g_argc = 1;
        PGL_LOG_INFO("[pgl_backend] Mobile: initialized g_argv[0]=%s", mobile_argv0);
    }
    */
#endif

    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B012: async_restart=%d\n", async_restart);
    fflush(stderr);
    if (async_restart)
    {
        // old 487
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] B013: Taking async_restart=1 path (new DB or mobile)\n");
        fflush(stderr);
        PGL_LOG_ERROR("[pgl_backend] *** Taking async_restart=1 path (new DB or mobile) ***");

#if PGDEBUG
        fprintf(stdout, "\n\n\n\n"
                        "=========================== BACKEND ====================================\n"
                        "# 346: FIXME: restarting in single mode after initdb with user '%s' instead of %s\n",
                PGUSER, getenv("PGUSER"));
        // main_post();
#endif
        // Optionally skip initdb single-user replay (still start backend later)
        const char *skip_single = getenv("PGL_SKIP_SINGLE");
        if (!(skip_single && skip_single[0] == '1'))
        {
            setenv("PGUSER", PGUSER, 1);
            // Build single-user argv dynamically to avoid empty args (e.g., empty WASM_PGOPTS)
            char *single_argv[32];
            int single_argc = 0;
            single_argv[single_argc++] = WASM_PREFIX "/bin/postgres";
            single_argv[single_argc++] = "--single";
            single_argv[single_argc++] = "-d";
            single_argv[single_argc++] = "1";
            single_argv[single_argc++] = "-B";
            single_argv[single_argc++] = "16";
            single_argv[single_argc++] = "-S";
            single_argv[single_argc++] = "512";
            single_argv[single_argc++] = "-f";
            single_argv[single_argc++] = "siobtnmh";
            single_argv[single_argc++] = "-D";
            single_argv[single_argc++] = (char *)PGDATA;
            // Disable startup progress timers to avoid timeout machinery during single-user replay
            single_argv[single_argc++] = "-c";
            single_argv[single_argc++] = "log_startup_progress_interval=0";
            // CRITICAL: Enable data_sync_retry to prevent PANIC on close() failures during startup.
            // On mobile, close() can fail during LruDelete when releasing file descriptors,
            // and the default behavior (PANIC) causes abort(). This is safe for single-user mode.
            single_argv[single_argc++] = "-c";
            single_argv[single_argc++] = "data_sync_retry=on";
            single_argv[single_argc++] = "-F";
            single_argv[single_argc++] = "-O";
            single_argv[single_argc++] = "-j";
            // CRITICAL: Match initdb's backend_options for correct schema placement.
            // Without search_path=pg_catalog, system views (pg_roles, pg_views, pg_indexes, etc.)
            // land in 'public' instead of 'pg_catalog', making them invisible to standard queries.
            // NOTE: exit_on_error=true is omitted because PGLite has known non-fatal errors
            // during initdb (OID counter, dict_snowball) that must be tolerated.
            single_argv[single_argc++] = "-c";
            single_argv[single_argc++] = "search_path=pg_catalog";
            if (WASM_PGOPTS[0] != '\0')
            {
                single_argv[single_argc++] = (char *)WASM_PGOPTS;
            }
            single_argv[single_argc++] = "template1";
            single_argv[single_argc] = NULL; // argc must NOT include NULL terminator
            optind = 1;
            // Log control file presence pre-single-user
            {
                char ctrl_path[1024];
                snprintf(ctrl_path, sizeof(ctrl_path), "%s/global/pg_control", PGDATA);
                struct stat st;
                int rc = stat(ctrl_path, &st);
                PGL_DEBUG_PRINT(stderr, "[pgl_main] pre-single ctrl=%s rc=%d errno=%d size=%lld\n",
                        ctrl_path, rc, errno, (long long)((rc == 0) ? st.st_size : 0));
            }
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B014: About to call RePostgresSingleUserMain(argc=%d)\n", single_argc);
            fflush(stderr);
            RePostgresSingleUserMain(single_argc, single_argv, PGUSER);
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B015: RePostgresSingleUserMain() returned\n");
            fflush(stderr);
            // Log control file presence post-single-user
            {
                char ctrl_path[1024];
                snprintf(ctrl_path, sizeof(ctrl_path), "%s/global/pg_control", PGDATA);
                struct stat st;
                int rc = stat(ctrl_path, &st);
                PGL_DEBUG_PRINT(stderr, "[pgl_main] post-single ctrl=%s rc=%d errno=%d size=%lld\n",
                        ctrl_path, rc, errno, (long long)((rc == 0) ? st.st_size : 0));
            }
        }
        else
        {
            PGL_DEBUG_PRINT(stderr, "[pgl_main] skipping initdb single-user replay due to PGL_SKIP_SINGLE=1\n");
        }
        //        AsyncPostgresSingleUserMain(single_argc, single_argv, PGUSER, async_restart);
        PDEBUG("# 365: initdb faking shutdown to complete WAL/OID states in single mode");

        /* CRITICAL: If RePostgresSingleUserMain returned early (e.g., initdb.single.txt not found),
         * shared memory may not be initialized. We need to ensure it's initialized before
         * proceeding to backend_started, otherwise RecoveryInProgress() will crash
         * accessing XLogCtl->SharedRecoveryState when XLogCtl is NULL.
         *
         * Check if PgStartTime is 0 (not yet set) to determine if init is needed.
         */
#ifdef PGL_MOBILE
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] B016: Checking PgStartTime=%lld\n", (long long)PgStartTime);
        fflush(stderr);
        if (PgStartTime == 0)
        {
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B017: PgStartTime==0, need to init shared memory\n");
            fflush(stderr);
            PGL_LOG_INFO("[pgl_backend] Shared memory not initialized, initializing now...");

            /* Set up pgl_boot_jmp to catch proc_exit during initialization.
             * This is critical because InitProcess may fail if shared memory is stale.
             * NOTE: We use a STATIC jump buffer to avoid dangling pointer issues. */
            pgl_boot_jmp = &pgl_shmem_jmp_buf;
            pgl_jmp_context = PGL_JMP_SHMEM;
            if (sigsetjmp(pgl_shmem_jmp_buf, 1) != 0)
            {
                pgl_boot_jmp = NULL;
                pgl_jmp_context = PGL_JMP_NONE;
                PGL_LOG_ERROR("[pgl_backend] proc_exit intercepted during shmem init, returning");
                PGL_DEBUG_PRINT(stderr, "[pgl_backend] proc_exit intercepted during shmem init, returning\n");
                return -1; /* Error - shmem init failed */
            }

            /* Follow the PostgresSingleUserMain() initialization sequence from postgres.c.
             * This is the proper PostgreSQL startup path for a standalone backend. */

            /* Step 1: Initialize memory context system */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B017a: Calling MemoryContextInit()\n");
            fflush(stderr);
            if (TopMemoryContext == NULL)
                MemoryContextInit();
            else
                CurrentMemoryContext = TopMemoryContext;
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B017b: MemoryContextInit() done\n");
            fflush(stderr);
            PGL_PROFILE_PHASE("MemoryContextInit");

            /* Step 2: Initialize standalone process (latches, signals, process globals).
             * This is CRITICAL - without it, CreateSharedMemoryAndSemaphores will hang
             * because latch/semaphore support is not initialized. */
            {
                const char *pr = (PREFIX && ((const char *)PREFIX)[0]) ? (const char *)PREFIX : WASM_PREFIX;
                char argv0_buf[256];
                snprintf(argv0_buf, sizeof(argv0_buf), "%s/bin/postgres", pr);
                PGL_DEBUG_PRINT(stderr, "[pgl_backend] B017c: Calling InitStandaloneProcess(%s)\n", argv0_buf);
                fflush(stderr);
                InitStandaloneProcess(argv0_buf);
                PGL_DEBUG_PRINT(stderr, "[pgl_backend] B017d: InitStandaloneProcess() done\n");
                fflush(stderr);
                PGL_PROFILE_PHASE("InitStandaloneProcess");
            }

            /* Step 3: Initialize GUC options */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B017e: Calling InitializeGUCOptions()\n");
            fflush(stderr);
            InitializeGUCOptions();
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B017f: InitializeGUCOptions() done\n");
            fflush(stderr);
            PGL_PROFILE_PHASE("InitializeGUCOptions");

            /* Step 4: Set DataDir and chdir */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B017g: Setting DataDir to %s\n", PGDATA);
            fflush(stderr);
            SetDataDir((const char *)PGDATA);
            if (chdir(PGDATA) != 0)
                PGL_DEBUG_PRINT(stderr, "[pgl_backend] WARNING: chdir(%s) failed errno=%d\n", PGDATA, errno);
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B017h: DataDir set, now=%s\n", DataDir ? DataDir : "NULL");
            fflush(stderr);

            /* Step 5: Load config files (postgresql.conf) */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B017i: Calling SelectConfigFiles()\n");
            fflush(stderr);
            if (!SelectConfigFiles(NULL, "postgres"))
            {
                PGL_DEBUG_PRINT(stderr, "[pgl_backend] WARNING: SelectConfigFiles failed, continuing anyway\n");
            }
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B017j: SelectConfigFiles() done\n");
            fflush(stderr);
            PGL_PROFILE_PHASE("SelectConfigFiles");

#ifdef PGL_MOBILE
            /* Enable WAL archiving if requested via pgl_enable_archiving().
             * archive_mode is PGC_POSTMASTER so it cannot be set via -c flag.
             * Must be set AFTER SelectConfigFiles to avoid being overridden by postgresql.conf. */
            if (pgl_archiving_enabled)
            {
                XLogArchiveMode = ARCHIVE_MODE_ON;
                PGL_DEBUG_PRINT(stderr, "[pgl_backend] archive_mode set to ON (pgl_enable_archiving)\n");
                fflush(stderr);
            }
#endif

            /* Read control file */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B018: Calling LocalProcessControlFile()\n");
            fflush(stderr);
            LocalProcessControlFile(false);
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B019: LocalProcessControlFile() done\n");
            fflush(stderr);
            PGL_PROFILE_PHASE("LocalProcessControlFile");

            /* Load preload libraries */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B020: Calling process_shared_preload_libraries()\n");
            fflush(stderr);
            process_shared_preload_libraries();
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B021: process_shared_preload_libraries() done\n");
            fflush(stderr);
            PGL_PROFILE_PHASE("process_shared_preload_libraries");

            /* Initialize MaxBackends - required for shared memory sizing */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B022: Calling InitializeMaxBackends()\n");
            fflush(stderr);
            InitializeMaxBackends();
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B023: InitializeMaxBackends() done\n");
            fflush(stderr);

            /* Process shared memory requests */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B024: Calling process_shmem_requests()\n");
            fflush(stderr);
            process_shmem_requests();
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B025: process_shmem_requests() done\n");
            fflush(stderr);

            /* Initialize shared memory GUCs */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B026: Calling InitializeShmemGUCs()\n");
            fflush(stderr);
            InitializeShmemGUCs();
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B027: InitializeShmemGUCs() done\n");
            fflush(stderr);

            /* Initialize WAL consistency checking */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B028: Calling InitializeWalConsistencyChecking()\n");
            fflush(stderr);
            InitializeWalConsistencyChecking();
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B029: InitializeWalConsistencyChecking() done\n");
            fflush(stderr);

            /* CRITICAL: Initialize shared memory and semaphores */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B030: Calling CreateSharedMemoryAndSemaphores()\n");
            fflush(stderr);
            CreateSharedMemoryAndSemaphores();
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B031: CreateSharedMemoryAndSemaphores() done\n");
            fflush(stderr);
            PGL_PROFILE_PHASE("CreateSharedMemoryAndSemaphores");

            /* Record startup time */
            PgStartTime = GetCurrentTimestamp();
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B032: PgStartTime set\n");
            fflush(stderr);

            /* Create per-backend PGPROC struct */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B033: Calling InitProcess()\n");
            fflush(stderr);
            InitProcess();
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B034: InitProcess() done\n");
            fflush(stderr);
            PGL_PROFILE_PHASE("InitProcess");

            /* Set processing mode - SetProcessingMode is a macro */
            SetProcessingMode(InitProcessing);
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B035: SetProcessingMode done\n");
            fflush(stderr);

            /* Early initialization - sets up smgr, buffer pool, VFD */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B036: Calling BaseInit()\n");
            fflush(stderr);
            BaseInit();
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B037: BaseInit() done\n");
            fflush(stderr);
            PGL_PROFILE_PHASE("BaseInit");

            /* Initialize timeout infrastructure - MUST be before InitPostgres
             * which calls RegisterTimeout for deadlock, statement, lock timeouts */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B037a1: Calling InitializeTimeouts()\n");
            fflush(stderr);
            InitializeTimeouts();
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B037a2: InitializeTimeouts() done\n");
            fflush(stderr);
            PGL_PROFILE_PHASE("InitializeTimeouts");

            /* Unblock signals - InitPostgres needs SIGALRM for timeouts */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B037a: Unblocking signals\n");
            fflush(stderr);
            {
                extern sigset_t UnBlockSig;
                sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);
            }

            /* Initialize the database - this is the big one.
             * InitPostgres sets up:
             *   - ProcArray (InitProcessPhase2)
             *   - Shared invalidation (SharedInvalBackendInit)
             *   - Signal handling (ProcSignalInit)
             *   - Timeouts (RegisterTimeout)
             *   - WAL / XLOG (StartupXLOG - WAL recovery happens here!)
             *   - Relation cache (RelationCacheInitialize)
             *   - Catalog cache (InitCatalogCache)
             *   - Plan cache (InitPlanCache)
             *   - Portal manager (EnablePortalManager)
             *   - Transaction system
             *   - Session user
             * Without this, DDL (CREATE TABLE etc.) will crash in hash_search
             * because catalog/relcache hash tables don't exist. */
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B037b: Calling InitPostgres()\n");
            fflush(stderr);
            {
                const char *dbname = "template1";
                const char *username = getenv("PGUSER");
                if (!username) username = "postgres";
                InitPostgres(dbname, InvalidOid, username, InvalidOid,
                             INIT_PG_LOAD_SESSION_LIBS, NULL);
            }
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B037c: InitPostgres() done\n");
            fflush(stderr);
            PGL_PROFILE_PHASE("InitPostgres");

            /* Switch to normal processing mode */
            SetProcessingMode(NormalProcessing);
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] B037d: NormalProcessing mode set\n");
            fflush(stderr);

            /* Clear jump buffer after successful init */
            pgl_boot_jmp = NULL;
            pgl_jmp_context = PGL_JMP_NONE;
            PGL_LOG_INFO("[pgl_backend] Full backend initialization complete (shmem + catalogs)");
            PGL_DEBUG_PRINT(stderr, "[pgl_backend] Full backend initialization complete\n");
        }
#endif

        PGL_DEBUG_PRINT(stderr, "[pgl_backend] B038: About to goto backend_started\n");
        fflush(stderr);
        goto backend_started;
    }

    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B039: Taking existing database path (async_restart=0)\n");
    fflush(stderr);

#ifdef PGL_MOBILE
    /*
     * MOBILE/DAEMON FIX: For existing databases (async_restart=0), use the same
     * cold-start initialization path as fresh databases. The old path through
     * main_post() + AsyncPostgresSingleUserMain is fragile — SelectConfigFiles,
     * CreateDataDirLockFile, etc. can call proc_exit() which triggers longjmp
     * and returns -2. The cold-start init path (PgStartTime==0 block) was
     * designed for the daemon use case and handles errors gracefully.
     */
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B039a: PGL_MOBILE existing DB — using cold-start init path\n");
    fflush(stderr);
    PGL_LOG_INFO("[pgl_backend] Existing DB on mobile/daemon — using cold-start init");

    /* Set up longjmp guard for proc_exit during initialization */
    pgl_boot_jmp = &pgl_shmem_jmp_buf;
    pgl_jmp_context = PGL_JMP_SHMEM;
    if (sigsetjmp(pgl_shmem_jmp_buf, 1) != 0)
    {
        pgl_boot_jmp = NULL;
        pgl_jmp_context = PGL_JMP_NONE;
        PGL_LOG_ERROR("[pgl_backend] proc_exit intercepted during existing DB init");
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] proc_exit intercepted during existing DB init\n");
        return -2;
    }

    /* Step 1: Memory context */
    if (TopMemoryContext == NULL)
        MemoryContextInit();
    else
        CurrentMemoryContext = TopMemoryContext;

    /* Step 2: Standalone process (latches, signals) */
    {
        const char *pr = (PREFIX && ((const char *)PREFIX)[0]) ? (const char *)PREFIX : WASM_PREFIX;
        char argv0_buf[256];
        snprintf(argv0_buf, sizeof(argv0_buf), "%s/bin/postgres", pr);
        InitStandaloneProcess(argv0_buf);
    }

    /* Step 3: GUC options */
    InitializeGUCOptions();

    /* Step 4: Set DataDir */
    SetDataDir((const char *)PGDATA);
    if (chdir(PGDATA) != 0)
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] WARNING: chdir(%s) failed errno=%d\n", PGDATA, errno);

    /* Step 5: Config files */
    if (!SelectConfigFiles(NULL, "postgres"))
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] WARNING: SelectConfigFiles failed, continuing\n");

#ifdef PGL_MOBILE
    /* Enable WAL archiving if requested via pgl_enable_archiving().
     * archive_mode is PGC_POSTMASTER so it cannot be set via -c flag.
     * Must be set AFTER SelectConfigFiles to avoid being overridden by postgresql.conf. */
    if (pgl_archiving_enabled)
    {
        XLogArchiveMode = ARCHIVE_MODE_ON;
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] archive_mode set to ON (pgl_enable_archiving)\n");
    }
#endif

    /* Step 6: Control file */
    LocalProcessControlFile(false);

    /* Step 7: Preload libraries */
    process_shared_preload_libraries();

    /* Step 8: MaxBackends */
    InitializeMaxBackends();

    /* Step 9: Shared memory requests */
    process_shmem_requests();

    /* Step 10: Shmem GUCs */
    InitializeShmemGUCs();

    /* Step 11: WAL consistency */
    InitializeWalConsistencyChecking();

    /* Step 12: Shared memory */
    CreateSharedMemoryAndSemaphores();

    /* Step 13: Startup time */
    PgStartTime = GetCurrentTimestamp();

    /* Step 14: Process struct */
    InitProcess();

    /* Step 15: Processing mode */
    SetProcessingMode(InitProcessing);

    /* Step 16: Base init (smgr, buffers, VFD) */
    BaseInit();

    /* Step 17: Timeouts */
    InitializeTimeouts();

    /* Step 18: Unblock signals */
    {
        extern sigset_t UnBlockSig;
        sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);
    }

    /* Step 19: InitPostgres */
    {
        const char *dbname = "template1";
        const char *username = getenv("PGUSER");
        if (!username) username = "postgres";
        InitPostgres(dbname, InvalidOid, username, InvalidOid,
                     INIT_PG_LOAD_SESSION_LIBS, NULL);
    }

    /* Step 20: Normal processing */
    SetProcessingMode(NormalProcessing);

    /* Clear jump buffer */
    pgl_boot_jmp = NULL;
    pgl_jmp_context = PGL_JMP_NONE;
    PGL_LOG_INFO("[pgl_backend] Existing DB cold-start init complete");
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B039b: Existing DB cold-start init complete\n");
    fflush(stderr);

    goto backend_started;
#endif /* PGL_MOBILE */

    PGL_LOG_ERROR("[pgl_backend] *** About to enter main_post() for existing database ***");
    PGL_DEBUG_PRINT(stderr, "[pgl_main] entering main_post (before single-user resume) g_argv=%p g_argv0=%s DataDir=%s\n",
            (void *)g_argv,
            (g_argv && g_argv[0]) ? g_argv[0] : "",
            DataDir ? DataDir : "");
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B061: Calling main_post()\n");
    fflush(stderr);
    PGL_LOG_ERROR("[pgl_backend] *** Calling main_post() now ***");
    main_post();
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B062: main_post() returned\n");
    fflush(stderr);
    PGL_LOG_ERROR("[pgl_backend] *** main_post() returned successfully ***");
    PGL_DEBUG_PRINT(stderr, "[pgl_main] returned from main_post\n");

    // Build resuming single-user argv dynamically to avoid empty args
    char *single_argv[24];
    char __argv0_buf[STROPS_BUF];
    const char *single_argv0;
    if (g_argv && g_argv[0] && g_argv[0][0])
    {
        single_argv0 = g_argv[0];
    }
    else
    {
        const char *pr = (PREFIX && ((const char *)PREFIX)[0]) ? (const char *)PREFIX : WASM_PREFIX;
        strconcat(__argv0_buf, pr, "/bin/postgres");
        single_argv0 = __argv0_buf;
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] Using fallback argv0: %s\n", single_argv0);
    }
    int single_argc = 0;
    single_argv[single_argc++] = (char *)single_argv0;
    single_argv[single_argc++] = "--single";
    single_argv[single_argc++] = "-d";
    single_argv[single_argc++] = "1";
    single_argv[single_argc++] = "-B";
    single_argv[single_argc++] = "16";
    single_argv[single_argc++] = "-S";
    single_argv[single_argc++] = "512";
    single_argv[single_argc++] = "-f";
    single_argv[single_argc++] = "siobtnmh";
    single_argv[single_argc++] = "-D";
    single_argv[single_argc++] = (char *)PGDATA;
    single_argv[single_argc++] = "-F";
    single_argv[single_argc++] = "-O";
    single_argv[single_argc++] = "-j";
    // Disable startup progress timers to avoid timeout machinery during existing db resume
    single_argv[single_argc++] = "-c";
    single_argv[single_argc++] = "log_startup_progress_interval=0";
    // CRITICAL: Enable data_sync_retry to prevent PANIC on close() failures during startup.
    // On mobile, close() can fail during LruDelete when releasing file descriptors,
    // and the default behavior (PANIC) causes abort(). This is safe for single-user mode.
    single_argv[single_argc++] = "-c";
    single_argv[single_argc++] = "data_sync_retry=on";
    if (WASM_PGOPTS[0] != '\0')
    {
        single_argv[single_argc++] = (char *)WASM_PGOPTS;
    }
    const char *db_for_resume = getenv("PGDATABASE");
    if (!db_for_resume || !db_for_resume[0])
        db_for_resume = "template1";
    single_argv[single_argc++] = (char *)db_for_resume;
    single_argv[single_argc] = NULL;
    int single_argc_save = single_argc;
    optind = 1;
#if PGDEBUG
    fprintf(stdout, "\n\n\n# 387: resuming db with user '%s' instead of %s\n", PGUSER, getenv("PGUSER"));
#endif
    setenv("PGUSER", PGUSER, 1);

#ifdef PGL_MOBILE
    /* Single-user mode runs in REPL mode by default (is_wire=false, is_repl=true) */
    PGL_LOG_INFO("[pgl_backend] Using default REPL mode for AsyncPostgresSingleUserMain");
    
    /* Set up pgl_boot_jmp to catch proc_exit during backend initialization.
     * This is critical because InitProcess may fail if shared memory is stale
     * (e.g., after app restart), and proc_exit would otherwise crash.
     * NOTE: We use a STATIC jump buffer to avoid dangling pointer issues. */
    
    /* Zero the jump buffer before use to ensure clean state */
    memset(&pgl_backend_jmp_buf, 0, sizeof(pgl_backend_jmp_buf));
    
    pgl_boot_jmp = &pgl_backend_jmp_buf;
    pgl_jmp_context = PGL_JMP_BACKEND;
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] *** pgl_boot_jmp set to %p (static buffer at %p, size=%zu, context=BACKEND) ***\n", 
            (void*)pgl_boot_jmp, (void*)&pgl_backend_jmp_buf, sizeof(pgl_backend_jmp_buf));
    
    /* Verify the pointer is valid before sigsetjmp */
    if ((uintptr_t)pgl_boot_jmp < 0x10000) {
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] FATAL: pgl_boot_jmp has invalid address %p!\n", (void*)pgl_boot_jmp);
        return -1;
    }
    
    if (sigsetjmp(pgl_backend_jmp_buf, 1) != 0)
    {
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] *** sigsetjmp returned non-zero (longjmp occurred) ***\n");
        pgl_boot_jmp = NULL;
        pgl_jmp_context = PGL_JMP_NONE;
        PGL_LOG_ERROR("[pgl_backend] proc_exit intercepted during backend init, returning");
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] proc_exit intercepted during backend init, returning\n");
        return -2; /* Error - backend init failed (proc_exit called) */
    }
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B057: sigsetjmp returned 0, proceeding to AsyncPostgresSingleUserMain\n");
    fflush(stderr);
#endif

    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B058: About to call AsyncPostgresSingleUserMain(argc=%d)\n", single_argc_save);
    fflush(stderr);
    AsyncPostgresSingleUserMain(single_argc_save, single_argv, PGUSER, async_restart);
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B059: AsyncPostgresSingleUserMain() returned\n");
    fflush(stderr);

#ifdef PGL_MOBILE
    /* Clear jump buffer after successful init - CRITICAL for query execution safety */
    pgl_boot_jmp = NULL;
    pgl_jmp_context = PGL_JMP_NONE;
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B060: pgl_boot_jmp cleared after AsyncPostgresSingleUserMain\n");
    fflush(stderr);
#endif

backend_started:;
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B040: Reached backend_started label\n");
    fflush(stderr);
    PGL_LOG_INFO("[pgl_backend] Reached backend_started label");
    IsPostmasterEnvironment = true;
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B041: IsPostmasterEnvironment = true\n");
    fflush(stderr);

#ifdef PGL_MOBILE
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B042: Starting mobile-specific backend state initialization\n");
    fflush(stderr);
    PGL_LOG_INFO("[pgl_mobile] Starting mobile-specific backend state initialization");

    /* Mobile: Initialize critical backend state that must persist across interactive_one() calls */
    /* These are normally set up in PostgresMain() but mobile needs them for wire protocol */
    extern MemoryContext row_description_context;
    extern StringInfoData row_description_buf;

    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B043: row_description_context = %p\n", (void *)row_description_context);
    fflush(stderr);
    PGL_LOG_INFO("[pgl_mobile] row_description_context = %p", (void *)row_description_context);

    if (row_description_context == NULL)
    {
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] B044: Creating row_description_context\n");
        fflush(stderr);
        PGL_LOG_INFO("[pgl_mobile] Initializing row_description_context for wire protocol");
        row_description_context = AllocSetContextCreate(TopMemoryContext,
                                                        "RowDescriptionContext",
                                                        ALLOCSET_DEFAULT_SIZES);
        MemoryContext oldcontext = MemoryContextSwitchTo(row_description_context);
        initStringInfo(&row_description_buf);
        MemoryContextSwitchTo(oldcontext);
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] B045: row_description_context created at %p\n", (void *)row_description_context);
        fflush(stderr);
        PGL_LOG_INFO("[pgl_mobile] row_description_context created at %p", (void *)row_description_context);
    }

    /* Ensure MessageContext exists for protocol message handling */
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B046: MessageContext = %p\n", (void *)MessageContext);
    fflush(stderr);
    PGL_LOG_INFO("[pgl_mobile] MessageContext = %p", (void *)MessageContext);

    if (MessageContext == NULL)
    {
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] B047: Creating MessageContext\n");
        fflush(stderr);
        PGL_LOG_INFO("[pgl_mobile] Initializing MessageContext for protocol handling");
        MessageContext = AllocSetContextCreate(TopMemoryContext,
                                               "MessageContext",
                                               ALLOCSET_DEFAULT_SIZES);
        PGL_DEBUG_PRINT(stderr, "[pgl_backend] B048: MessageContext created at %p\n", (void *)MessageContext);
        fflush(stderr);
        PGL_LOG_INFO("[pgl_mobile] MessageContext created at %p", (void *)MessageContext);
    }
    /* Initialize mobile communication methods before any backend processing */
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B049: Installing mobile communication methods\n");
    fflush(stderr);
    PGL_LOG_INFO("[pgl_mobile] Installing mobile communication methods");
    pgl_install_mobile_comm();
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B050: pgl_install_mobile_comm() done\n");
    fflush(stderr);
    PGL_LOG_INFO("[pgl_mobile] Mobile communication methods installed successfully");

    /*
     * CRITICAL: Initialize extensions that require _PG_init to be called.
     * This fixes the iOS device crash where plpgsql_HashTable is NULL because
     * the lazy initialization in load_external_function() may not trigger
     * correctly on physical devices.
     *
     * See: docs/issues/pglite-ios-device-crash.md
     */
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B051: Initializing extensions (plpgsql, etc.)\n");
    fflush(stderr);
    PGL_LOG_INFO("[pgl_mobile] Initializing extensions (plpgsql, etc.)");
    pgl_mobile_init_extensions();
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B052: pgl_mobile_init_extensions() done\n");
    fflush(stderr);
    PGL_LOG_INFO("[pgl_mobile] Extension initialization complete");

    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B053: Mobile backend state initialization complete\n");
    fflush(stderr);
    PGL_LOG_INFO("[pgl_mobile] Mobile backend state initialization complete");

    /* Mark backend as initialized - subsequent calls to pgl_backend() will skip re-init */
    pgl_backend_initialized = true;
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B054: pgl_backend_initialized = true\n");
    fflush(stderr);
    PGL_LOG_INFO("[pgl_backend] Backend initialization complete, pgl_backend_initialized=true");
#endif

    if (TransamVariables && TransamVariables->nextOid < ((Oid)FirstNormalObjectId))
    {
        /* IsPostmasterEnvironment is now true
           these will be executed when required in varsup.c/GetNewObjectId
           TransamVariables->nextOid = FirstNormalObjectId;
           TransamVariables->oidCount = 0;
         */
#if PGDEBUG
        puts("# 403: initdb done, oid base too low but OID range will be set because IsPostmasterEnvironment");
#endif
    }
#ifdef PGL_MOBILE
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B055: About to return 0 (success)\n");
    fflush(stderr);
    PGL_LOG_INFO("[pgl_backend] EXIT: function completing successfully");
#endif
    PGL_DEBUG_PRINT(stderr, "[pgl_backend] B056: RETURNING 0\n");
    fflush(stderr);
    PGL_PROFILE_TOTAL("pgl_backend");
    return 0; /* Success */
}

#if defined(__EMSCRIPTEN__)
EMSCRIPTEN_KEEPALIVE
#else
__attribute__((export_name("pgl_initdb")))
#endif
int pgl_initdb()
{
    PGL_PROFILE_DECL();
    PGL_PROFILE_START();

    PGL_LOG_INFO("[pgl_initdb] ENTRY: function called");
    PDEBUG("# 412: pg_initdb()");
    /* Ensure PREFIX/PGDATA/PGUSER defaults like wasm main_pre */
    if (!PREFIX || !*PREFIX)
    {
        PREFIX = setdefault("PREFIX", WASM_PREFIX);
    }
    if (!getenv("PGDATABASE"))
        setenv("PGDATABASE", "template1", 0);
    PGUSER = setdefault("PGUSER", WASM_USERNAME);
    setenv("PGUSER", WASM_USERNAME, 1);
    char _pgbuf[STROPS_BUF];
    strconcat(_pgbuf, (const char *)PREFIX, "/base");
    if (!PGDATA || !*PGDATA)
    {
        PGDATA = setdefault("PGDATA", _pgbuf);
    }
    PGL_DEBUG_PRINT(stderr, "[pgl_initdb] PREFIX=%s PGDATA=%s PGUSER=%s PGSYSCONFDIR=%s\n", PREFIX ? (const char *)PREFIX : "", PGDATA ? (const char *)PGDATA : "", PGUSER ? (const char *)PGUSER : "", getenv("PGSYSCONFDIR") ? getenv("PGSYSCONFDIR") : "");

    /* Initialize progname for mobile library mode */
    if (!progname)
    {
        progname = "postgres";
    }

    optind = 1;
    pgl_idb_status |= IDB_FAILED;

    /* Allow forcing initdb even if PG_VERSION exists */
    const char *__force_env = getenv("PGL_FORCE_INITDB");
    bool __force_initdb = (__force_env && __force_env[0] == '1');
    PGL_LOG_INFO("[pgl_initdb] About to check if database exists at PGDATA=%s", PGDATA ? (const char *)PGDATA : "");
    
#ifdef PGL_MOBILE
    /* DEBUG: List PGDATA directory contents BEFORE the check */
    {
        struct stat pgdata_stat;
        int stat_rc = stat((const char *)PGDATA, &pgdata_stat);
        PGL_LOG_ERROR("[pgl_initdb] PGDATA stat: path=%s rc=%d errno=%d", 
                      PGDATA ? (const char *)PGDATA : "NULL", stat_rc, stat_rc ? errno : 0);
        if (stat_rc == 0) {
            PGL_LOG_ERROR("[pgl_initdb] PGDATA exists: mode=%o size=%lld", 
                          pgdata_stat.st_mode, (long long)pgdata_stat.st_size);
            
            /* List directory contents */
            DIR *dir = opendir((const char *)PGDATA);
            if (dir) {
                struct dirent *entry;
                int file_count = 0;
                PGL_LOG_ERROR("[pgl_initdb] PGDATA directory contents:");
                while ((entry = readdir(dir)) != NULL) {
                    if (entry->d_name[0] == '.') continue;
                    char full_path[1024];
                    snprintf(full_path, sizeof(full_path), "%s/%s", (const char *)PGDATA, entry->d_name);
                    struct stat fst;
                    if (stat(full_path, &fst) == 0) {
                        PGL_LOG_ERROR("[pgl_initdb]   - %s (%s, %lld bytes)", 
                                      entry->d_name, 
                                      S_ISDIR(fst.st_mode) ? "DIR" : "FILE",
                                      (long long)fst.st_size);
                    } else {
                        PGL_LOG_ERROR("[pgl_initdb]   - %s (stat failed)", entry->d_name);
                    }
                    file_count++;
                }
                closedir(dir);
                PGL_LOG_ERROR("[pgl_initdb] Total items in PGDATA: %d", file_count);
            } else {
                PGL_LOG_ERROR("[pgl_initdb] Cannot opendir PGDATA: errno=%d", errno);
            }
        } else {
            PGL_LOG_ERROR("[pgl_initdb] PGDATA does not exist (fresh install expected)");
        }
        
        /* Also check PG_VERSION directly */
        char pg_version_path[1024];
        snprintf(pg_version_path, sizeof(pg_version_path), "%s/PG_VERSION", (const char *)PGDATA);
        struct stat pv_stat;
        int pv_rc = stat(pg_version_path, &pv_stat);
        PGL_LOG_ERROR("[pgl_initdb] PG_VERSION check: path=%s exists=%s size=%lld",
                      pg_version_path, pv_rc == 0 ? "YES" : "NO",
                      pv_rc == 0 ? (long long)pv_stat.st_size : 0);
    }
#endif

    if (!chdir(PGDATA))
    {
        int __has_pgversion = (access("PG_VERSION", F_OK) == 0);
        PGL_DEBUG_PRINT(stderr, "[pgl_initdb] chdir PGDATA ok; PG_VERSION=%s force=%d\n", __has_pgversion ? "yes" : "no", __force_initdb ? 1 : 0);
        PGL_LOG_INFO("[pgl_initdb] Database exists check: PG_VERSION=%s force=%d", __has_pgversion ? "yes" : "no", __force_initdb ? 1 : 0);
        
#ifdef PGL_MOBILE
        /* DEBUG: If PG_VERSION exists, read its contents */
        if (__has_pgversion) {
            FILE *pv_file = fopen("PG_VERSION", "r");
            if (pv_file) {
                char pv_content[64] = {0};
                size_t read_bytes = fread(pv_content, 1, sizeof(pv_content) - 1, pv_file);
                fclose(pv_file);
                /* Remove trailing newline */
                for (size_t i = 0; i < read_bytes; i++) {
                    if (pv_content[i] == '\n' || pv_content[i] == '\r') {
                        pv_content[i] = '\0';
                        break;
                    }
                }
                PGL_LOG_ERROR("[pgl_initdb] PG_VERSION content: '%s' (%zu bytes)", pv_content, read_bytes);
            }
            
            /* Also check pg_control */
            char ctrl_path[1024];
            snprintf(ctrl_path, sizeof(ctrl_path), "%s/global/pg_control", (const char *)PGDATA);
            struct stat ctrl_stat;
            int ctrl_rc = stat(ctrl_path, &ctrl_stat);
            PGL_LOG_ERROR("[pgl_initdb] pg_control: exists=%s size=%lld",
                          ctrl_rc == 0 ? "YES" : "NO",
                          ctrl_rc == 0 ? (long long)ctrl_stat.st_size : 0);
        }
#endif
        
        if (__has_pgversion && !__force_initdb)
        {
#ifdef PGL_MOBILE
            /*
             * CRITICAL: Validate that pg_control exists and has correct size before
             * attempting to use an existing database. A previous crash (e.g., during
             * PostGIS initialization) could leave the database in a corrupt state
             * where PG_VERSION exists but pg_control is missing or invalid.
             *
             * If pg_control is invalid, we MUST delete the corrupt database and
             * reinitialize, otherwise ReadControlFile() will PANIC and crash the app.
             *
             * See: docs/issues/pglite-ios-postgis-crash.md
             */
            struct stat ctrl_stat;
            int has_valid_control = 0;

            if (stat("global/pg_control", &ctrl_stat) == 0) {
                /* PG_CONTROL_FILE_SIZE is 8192 bytes */
                if (ctrl_stat.st_size >= 8192) {
                    has_valid_control = 1;
                    PGL_LOG_INFO("[pgl_initdb] pg_control valid: size=%lld", (long long)ctrl_stat.st_size);
                } else {
                    PGL_LOG_ERROR("[pgl_initdb] pg_control CORRUPT: size=%lld (expected >= 8192)", (long long)ctrl_stat.st_size);
                }
            } else {
                PGL_LOG_ERROR("[pgl_initdb] pg_control MISSING at global/pg_control - database is corrupt");
            }

            if (!has_valid_control) {
                /* Database is corrupt - must delete and reinitialize */
                PGL_LOG_ERROR("[pgl_initdb] *** CORRUPT DATABASE DETECTED - REMOVING AND REINITIALIZING ***");
                PGL_DEBUG_PRINT(stderr, "[pgl_initdb] Removing corrupt database at %s\n", (const char *)PGDATA);

                chdir("/");  /* Must exit PGDATA before removing it */

                int rm_result = pgl_remove_directory_recursive((const char *)PGDATA);
                if (rm_result == 0) {
                    PGL_LOG_INFO("[pgl_initdb] Corrupt database removed successfully");
                    PGL_DEBUG_PRINT(stderr, "[pgl_initdb] Corrupt database removed, will reinitialize\n");
                } else {
                    PGL_LOG_ERROR("[pgl_initdb] Failed to remove corrupt database (rc=%d)", rm_result);
                    PGL_DEBUG_PRINT(stderr, "[pgl_initdb] WARNING: Failed to fully remove corrupt database\n");
                }

                /* Fall through to run initdb - do NOT goto initdb_done */
                goto run_initdb;
            }
#endif
            chdir("/");

            pgl_idb_status |= IDB_HASDB;

            /* assume auth success for now */
            pgl_idb_status |= IDB_HASUSER;
            PGL_LOG_INFO("[pgl_initdb] Database already exists, skipping initdb");
#if PGDEBUG
            fprintf(stdout, "# 427: pg_initdb: db exists at : %s TODO: test for db name : %s \n", PGDATA, getenv("PGDATABASE"));
#endif // PGDEBUG

            async_restart = 0;
            goto initdb_done;
        }
#ifdef PGL_MOBILE
run_initdb:
#endif
        chdir("/");
        PGL_LOG_INFO("[pgl_initdb] No existing database found, will run initdb");
#if PGDEBUG
        fprintf(stderr, "# 435: pg_initdb no db found at : %s\n", PGDATA);
#endif // PGDEBUG
    }
    else
    {
        PGL_DEBUG_PRINT(stderr, "[pgl_initdb] chdir PGDATA failed (dir missing?) path=%s errno=%d\n", PGDATA ? (const char *)PGDATA : "", errno);
        PGL_LOG_INFO("[pgl_initdb] chdir PGDATA failed, will run initdb");
#if PGDEBUG
        fprintf(stderr, "# 439: pg_initdb db folder not found at : %s\n", PGDATA);
#endif // PGDEBUG
    }

    PGL_LOG_INFO("[pgl_initdb] Calling pgl_initdb_main()...");
#ifdef PGL_CATCH_EXIT
    // Use the safe wrapper that catches exit() calls and converts to longjmp
    extern int pgl_initdb_safe(void);
    int initdb_rc = pgl_initdb_safe();
    if (initdb_rc != 0)
    {
        PGL_DEBUG_PRINT(stderr, "[pgl_main] pgl_initdb_safe returned error: %d\n", initdb_rc);
        PGL_LOG_INFO("[pgl_initdb] initdb failed with exit code %d", initdb_rc);
        return pgl_idb_status | IDB_FAILED;
    }
#else
    int initdb_rc = pgl_initdb_main();
#endif
    PGL_DEBUG_PRINT(stderr, "[pgl_main] pgl_initdb_main rc=%d\n", initdb_rc);
    PGL_LOG_INFO("[pgl_initdb] pgl_initdb_main() returned %d", initdb_rc);
    PGL_PROFILE_PHASE("pgl_initdb_main");
    const char *skip_replay = getenv("PGL_SKIP_REPLAY");
    if (skip_replay && skip_replay[0] == '1')
    {
        PGL_DEBUG_PRINT(stderr, "[pgl_main] skipping boot replay due to PGL_SKIP_REPLAY=1\n");
        goto initdb_done;
    }

#if PGDEBUG
    fprintf(stderr, "\n\n# 444: " __FILE__ "pgl_initdb_main = %d\n", initdb_rc);
#endif // PGDEBUG
    PDEBUG("# 448:" __FILE__);
    // Log control file and bki presence pre-bootstrap
    {
        const char *sysconf = getenv("PGSYSCONFDIR");
        char ctrl_path[1024];
        snprintf(ctrl_path, sizeof(ctrl_path), "%s/global/pg_control", PGDATA);
        struct stat st;
        int rc = stat(ctrl_path, &st);
        PGL_DEBUG_PRINT(stderr, "[pgl_main] pre-boot ctrl=%s rc=%d errno=%d size=%lld\n",
                ctrl_path, rc, errno, (long long)((rc == 0) ? st.st_size : 0));
        if (sysconf)
        {
            char bki_path[1024];
            snprintf(bki_path, sizeof(bki_path), "%s/postgres.bki", sysconf);
            struct stat sb;
            int rcb = stat(bki_path, &sb);
            PGL_DEBUG_PRINT(stderr, "[pgl_main] pre-boot bki=%s rc=%d errno=%d size=%lld\n",
                    bki_path, rcb, errno, (long long)((rcb == 0) ? sb.st_size : 0));
        }
        else
        {
            PGL_DEBUG_PRINT(stderr, "[pgl_main] pre-boot PGSYSCONFDIR not set\n");
        }
    }
    /* save stdin and use previous initdb output to feed boot mode */
#if defined(__ANDROID__) || defined(PGL_MOBILE)
    /* On mobile/embedded platforms, STDIN_FILENO may not be properly initialized or may block.
     * Create a dummy stdin that points to /dev/null to avoid hanging.
     * This includes Android, iOS, and Linux daemon builds (all use PGL_MOBILE). */
    int saved_stdin = open("/dev/null", O_RDONLY);
    if (saved_stdin < 0)
    {
        PGL_DEBUG_PRINT(stderr, "[pgl_main] open(/dev/null) failed: %d\n", errno);
        return pgl_idb_status;
    }
    PGL_DEBUG_PRINT(stderr, "[pgl_main] PGL_MOBILE: using /dev/null as saved_stdin=%d\n", saved_stdin);
#else
    int saved_stdin = dup(STDIN_FILENO);
    if (saved_stdin < 0)
    {
        PGL_DEBUG_PRINT(stderr, "[pgl_main] dup(STDIN) failed: %d\n", errno);
        return pgl_idb_status;
    }
#endif
    {
        PDEBUG("# 450: restarting in boot mode for initdb");
#ifdef PGL_MOBILE
        char __pipe_path[1024];
        extern void pgl_get_pipe_path(int stage, char *out, size_t outsz);
        pgl_get_pipe_path(0, __pipe_path, sizeof(__pipe_path));
        PGL_DEBUG_PRINT(stderr, "[pgl_main] boot pipe path=%s\n", __pipe_path);
        struct stat __bp_st;
        int __bp_rc = stat(__pipe_path, &__bp_st);
        PGL_DEBUG_PRINT(stderr, "[pgl_main] boot pipe stat rc=%d errno=%d size=%lld\n", __bp_rc, errno, (long long)((__bp_rc == 0) ? __bp_st.st_size : 0));
        FILE *fr = freopen(__pipe_path, "r", stdin);
#else
        FILE *fr = freopen(IDB_PIPE_BOOT, "r", stdin);
#endif
        if (!fr)
        {
#ifdef PGL_MOBILE
            PGL_DEBUG_PRINT(stderr, "[pgl_main] freopen boot failed for %s errno=%d\n", __pipe_path, errno);
#else
            PGL_DEBUG_PRINT(stderr, "[pgl_main] freopen boot failed for %s errno=%d\n", IDB_PIPE_BOOT, errno);
#endif
            // attempt to restore STDIN before returning
            dup2(saved_stdin, STDIN_FILENO);
            close(saved_stdin);
            clearerr(stdin);  // reset EOF/error on stdin after dup2 restore
            return pgl_idb_status;
        }

        const char *boot_argv0;
        char __argv0_buf[STROPS_BUF];
        if (g_argv && g_argv[0] && g_argv[0][0])
        {
            boot_argv0 = g_argv[0];
        }
        else
        {
            const char *pr = (PREFIX && ((const char *)PREFIX)[0]) ? (const char *)PREFIX : WASM_PREFIX;
            strconcat(__argv0_buf, pr, "/bin/postgres");
            boot_argv0 = __argv0_buf;
        }
        PGL_DEBUG_PRINT(stderr, "[pgl_main] boot argv0=%s\n", boot_argv0);

        // Build argv dynamically to avoid inserting empty arguments (e.g., when WASM_PGOPTS is "")
        char *boot_argv[32];
        int boot_argc = 0;
        boot_argv[boot_argc++] = (char *)boot_argv0;
        boot_argv[boot_argc++] = "--boot";
        boot_argv[boot_argc++] = "-F";
        boot_argv[boot_argc++] = "-c";
        boot_argv[boot_argc++] = "log_checkpoints=false";
        boot_argv[boot_argc++] = "-c";
        boot_argv[boot_argc++] = "log_min_messages=error";
        boot_argv[boot_argc++] = "-c";
        boot_argv[boot_argc++] = "client_min_messages=error";
        boot_argv[boot_argc++] = "-c";
        boot_argv[boot_argc++] = "log_error_verbosity=terse";
        // Disable startup progress timers to avoid timeout machinery during bootstrap
        boot_argv[boot_argc++] = "-c";
        boot_argv[boot_argc++] = "log_startup_progress_interval=0";
        // CRITICAL: Enable data_sync_retry to prevent PANIC on close() failures during startup.
        // On mobile, close() can fail during LruDelete when releasing file descriptors,
        // and the default behavior (PANIC) causes abort(). This is safe for bootstrap mode.
        boot_argv[boot_argc++] = "-c";
        boot_argv[boot_argc++] = "data_sync_retry=on";
        // keep resource usage minimal and avoid dynamic shared memory on Android
        boot_argv[boot_argc++] = "-c";
        boot_argv[boot_argc++] = "shared_buffers=16";
        // Single-user: keep minimal connections; 1 is sufficient for bootstrap
        boot_argv[boot_argc++] = "-c";
        boot_argv[boot_argc++] = "max_connections=1";
        // Avoid GUCs that may be unavailable in bootstrap/mobile builds
        // Disable huge pages on mobile; fallback mmap is fine
        boot_argv[boot_argc++] = "-c";
        boot_argv[boot_argc++] = "huge_pages=off";
        // boot_argv[boot_argc++] = "-c"; boot_argv[boot_argc++] = "wal_level=minimal";
        boot_argv[boot_argc++] = "-D";
        boot_argv[boot_argc++] = (char *)PGDATA;
        boot_argv[boot_argc++] = "-d";
        boot_argv[boot_argc++] = "3";
        if (WASM_PGOPTS[0] != '\0')
        {
            boot_argv[boot_argc++] = (char *)WASM_PGOPTS;
        }
        boot_argv[boot_argc++] = "-X";
        boot_argv[boot_argc++] = "1048576";
        boot_argv[boot_argc] = NULL; // argc must NOT count the NULL terminator

        set_pglocale_pgservice(boot_argv[0], PG_TEXTDOMAIN("initdb"));

        // Reset getopt() globals defensively before calling into backend parsers
        optind = 1;
        opterr = 1;
        optopt = 0;
        optarg = NULL;

        // Log argv for diagnosis
        PGL_DEBUG_PRINT(stderr, "[pgl_main] boot argc=%d argv:", boot_argc);
        // Ensure working directory is PGDATA for any relative paths during bootstrap.
        // Also set backend DataDir explicitly so ChangeToDataDir() and path resolvers match.
        if (chdir((const char *)PGDATA) != 0)
        {
            PGL_DEBUG_PRINT(stderr, "[pgl_main] chdir(PGDATA) failed errno=%d\n", errno);
        }
        SetDataDir((const char *)PGDATA);

        for (int i = 0; i < boot_argc; i++)
        {
            fprintf(stderr, " %s", boot_argv[i]);
        }
        fputc('\n', stderr);

        // Append bootstrap stderr to the same initdb log used on Android, if set
        const char *appendLog = getenv("PGL_INITDB_LOG");
        FILE *__boot_log = NULL;
        // Install a temporary emit_log_hook to capture bootstrap errors
        emit_log_hook_type prev_hook = emit_log_hook;
        emit_log_hook = pgl_boot_emit_hook;
        int bootstrap_stderr_fd = -1;

        if (appendLog && appendLog[0])
        {
            __boot_log = freopen(appendLog, "a", stderr);
            if (__boot_log)
            {
                setvbuf(stderr, NULL, _IONBF, 0); // unbuffer stderr so crashes don't lose logs
                PGL_DEBUG_PRINT(stderr, "[pgl_main] appending bootstrap logs to %s\n", appendLog);
            }
        }

        // Protect against ereport(FATAL)/ERROR inside BootstrapModeMain from exiting the process
        sigjmp_buf __boot_jmp;

        bool __boot_err = false;
        if (sigsetjmp(__boot_jmp, 1) != 0)
        {
            /* Ensure we clear PG_exception_stack to avoid dangling pointer after longjmp */
            PG_exception_stack = NULL;
            __boot_err = true;
            PGL_DEBUG_PRINT(stderr, "[pgl_main] BootstrapModeMain exited via error (longjmp), continuing without proc_exit\n");
        }
        else
        {
            /* Defensive: ensure basic subsystems are initialized before calling BootstrapModeMain */
            if (CurrentMemoryContext == NULL)
                MemoryContextInit();
            const char *argv0_boot = (boot_argv && boot_argv[0] && boot_argv[0][0]) ? boot_argv[0] : (g_argv && g_argv[0] ? g_argv[0] : "postgres");
            set_pglocale_pgservice(argv0_boot, PG_TEXTDOMAIN("initdb"));
            PG_exception_stack = &__boot_jmp;
            PGL_DEBUG_PRINT(stderr, "[pgl_main] calling BootstrapModeMain\n");
            PGL_LOG_INFO("%s", "[pgl_main] About to call BootstrapModeMain");
            // Also redirect stderr to initdb.stderr.log so ereport lands there
            char errlog[1024];
            snprintf(errlog, sizeof(errlog), "%s/initdb.stderr.log", PREFIX ? (const char *)PREFIX : WASM_PREFIX);
            bootstrap_stderr_fd = open(errlog, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (bootstrap_stderr_fd >= 0)
            {
                PGL_LOG_INFO("[pgl_main] Redirecting stderr to %s (fd=%d)", errlog, bootstrap_stderr_fd);
                dup2(bootstrap_stderr_fd, STDERR_FILENO);
                setvbuf(stderr, NULL, _IONBF, 0); // unbuffer stderr
            }
            else
            {
                PGL_LOG_ERROR("[pgl_main] Failed to open stderr log %s: errno=%d", errlog, errno);
            }
            /* Intercept proc_exit during bootstrap to avoid PANIC in child context.
             * NOTE: We use a STATIC jump buffer to avoid dangling pointer issues. */
            pgl_boot_jmp = &pgl_boot_jmp_buf;
            pgl_jmp_context = PGL_JMP_BOOT;
            if (sigsetjmp(pgl_boot_jmp_buf, 1) != 0)
            {
                pgl_boot_jmp = NULL;
                pgl_jmp_context = PGL_JMP_NONE;
                PGL_DEBUG_PRINT(stderr, "[pgl_boot] proc_exit intercepted during bootstrap, continuing\n");
                PGL_LOG_INFO("%s", "[pgl_boot] proc_exit intercepted during bootstrap");
            }
            else
            {
                PGL_LOG_INFO("%s", "[pgl_main] Entering BootstrapModeMain");
                BootstrapModeMain(boot_argc, boot_argv, false);
                PGL_LOG_INFO("[pgl_main] BootstrapModeMain completed successfully");
            }
            pgl_boot_jmp = NULL;
            pgl_jmp_context = PGL_JMP_NONE;
            PGL_DEBUG_PRINT(stderr, "[pgl_main] BootstrapModeMain returned normally\n");
            PGL_LOG_ERROR("%s", "[pgl_main] *** BootstrapModeMain phase completed ***");
        }
        PGL_LOG_ERROR("%s", "[pgl_main] *** About to clear PG_exception_stack ***");
        PG_exception_stack = NULL;
        PGL_LOG_ERROR("%s", "[pgl_main] *** About to restore emit_log_hook ***");
        emit_log_hook = prev_hook;
        PGL_LOG_ERROR("%s", "[pgl_main] *** Hooks restored, about to restore stderr ***");

#ifdef __ANDROID__
        PGL_LOG_ERROR("%s", "[pgl_main] *** About to restore stderr after bootstrap ***");
        // CRITICAL: Restore stderr to Android pipe after bootstrap to prevent hang
        if (bootstrap_stderr_fd >= 0)
        {
            PGL_LOG_ERROR("%s", "[pgl_main] *** About to close bootstrap stderr fd ***");
            close(bootstrap_stderr_fd);
            PGL_LOG_ERROR("%s", "[pgl_main] *** Closed bootstrap stderr log file ***");
            // Restore stderr to the Android pipe for continued logging
            if (pgl_stderr_pipe[1] >= 0)
            {
                PGL_LOG_ERROR("%s", "[pgl_main] *** About to dup2 stderr back to pipe ***");
                if (dup2(pgl_stderr_pipe[1], STDERR_FILENO) < 0)
                {
                    PGL_LOG_ERROR("[pgl_main] Failed to restore stderr to pipe: errno=%d", errno);
                }
                else
                {
                    PGL_LOG_ERROR("%s", "[pgl_main] *** dup2 successful, about to setvbuf ***");
                    setvbuf(stderr, NULL, _IONBF, 0);
                    PGL_LOG_ERROR("%s", "[pgl_main] *** Successfully restored stderr to Android pipe ***");
                }
            }
            else
            {
                PGL_LOG_ERROR("%s", "[pgl_main] Android stderr pipe not available for restoration");
            }
        }
        else
        {
            PGL_LOG_ERROR("%s", "[pgl_main] *** bootstrap_stderr_fd was not opened ***");
        }
        PGL_LOG_ERROR("%s", "[pgl_main] *** Stderr restoration completed ***");
#endif

#if defined(__ANDROID__) || defined(PGL_MOBILE)
        // Restore stdin to /dev/null. Use freopen() which works on all libc
        // implementations (glibc, musl, bionic). Direct assignment to stdin
        // (stdin = fopen(...)) crashes on musl where stdin is a macro/constant.
        PGL_DEBUG_PRINT(stderr, "[pgl_main] PGL_MOBILE: freopen /dev/null for stdin\n");
        PGL_LOG_INFO("%s", "[pgl_main] Reopening /dev/null for stdin via freopen");
        if (!freopen("/dev/null", "r", stdin))
        {
            PGL_DEBUG_PRINT(stderr, "[pgl_main] freopen(/dev/null, stdin) failed errno=%d\n", errno);
            PGL_LOG_ERROR("[pgl_main] freopen(/dev/null, stdin) failed errno=%d", errno);
        }
        else
        {
            PGL_LOG_INFO("%s", "[pgl_main] Successfully reopened /dev/null for stdin");
        }
        close(saved_stdin);
        PGL_LOG_INFO("%s", "[pgl_main] Closed saved_stdin fd");
#else
        /* Desktop/WASM: restore original stdin from saved_stdin */
        if (dup2(saved_stdin, STDIN_FILENO) < 0)
        {
            PGL_DEBUG_PRINT(stderr, "[pgl_main] dup2 restore STDIN failed errno=%d\n", errno);
            close(saved_stdin);
            return pgl_idb_status;
        }
        close(saved_stdin);
        PGL_DEBUG_PRINT(stderr, "[pgl_main] restoring stdin after dup2\n");
        clearerr(stdin);  // reset EOF/error on stdin after dup2 restore
#endif
        PGL_DEBUG_PRINT(stderr, "[pgl_main] stdin restoration completed\n");
        PGL_LOG_INFO("%s", "[pgl_main] stdin restoration phase completed");
        // Do NOT exit the process; just continue to allow backend to start
        if (__boot_err)
        {
            PGL_DEBUG_PRINT(stderr, "[pgl_main] initdb boot replay completed with errors\n");
            // Diagnostics: current cwd, DataDir, and first few lines of boot file
            char cwd_buf[1024];
            if (getcwd(cwd_buf, sizeof(cwd_buf)))
            {
                PGL_DEBUG_PRINT(stderr, "[pgl_main] cwd=%s DataDir=%s\n", cwd_buf, DataDir ? DataDir : "");
            }
            // Log global dir existence
            {
                char gpath[1024];
                struct stat gst;
                snprintf(gpath, sizeof(gpath), "%s/global", PGDATA);
                int grc = stat(gpath, &gst);
                PGL_DEBUG_PRINT(stderr, "[pgl_main] global dir stat rc=%d errno=%d\n", grc, errno);
            }
            // Dump first 25 lines of boot stream
#ifdef PGL_MOBILE
            {
                char pipe_path[1024];
                extern void pgl_get_pipe_path(int stage, char *out, size_t outsz);
                pgl_get_pipe_path(0, pipe_path, sizeof(pipe_path));
                FILE *f = fopen(pipe_path, "r");
                if (f)
                {
                    PGL_DEBUG_PRINT(stderr, "[pgl_main] boot head:\n");
                    char line[256];
                    int n = 0;
                    while (n < 25 && fgets(line, sizeof(line), f))
                    {
                        fputs(line, stderr);
                        n++;
                    }
                    if (!feof(f))
                        PGL_DEBUG_PRINT(stderr, "[pgl_main] ... (truncated)\n");
                    fclose(f);
                }
                else
                {
                    PGL_DEBUG_PRINT(stderr, "[pgl_main] cannot open boot file for head: %s errno=%d\n", pipe_path, errno);
                }
            }
#else
            {
                FILE *f = fopen(IDB_PIPE_BOOT, "r");
                if (f)
                {
                    PGL_DEBUG_PRINT(stderr, "[pgl_main] boot head:\n");
                    char line[256];
                    int n = 0;
                    while (n < 25 && fgets(line, sizeof(line), f))
                    {
                        fputs(line, stderr);
                        n++;
                    }
                    if (!feof(f))
                        PGL_DEBUG_PRINT(stderr, "[pgl_main] ... (truncated)\n");
                    fclose(f);
                }
                else
                {
                    PGL_DEBUG_PRINT(stderr, "[pgl_main] cannot open boot file for head: %s errno=%d\n", IDB_PIPE_BOOT, errno);
                }
            }
#endif
        }
        else
        {
            PDEBUG("# 479: initdb boot replay done");
            PGL_LOG_INFO("%s", "[pgl_main] initdb boot replay completed successfully");
        }
        // Log control file presence post-bootstrap
        {
            char ctrl_path[1024];
            snprintf(ctrl_path, sizeof(ctrl_path), "%s/global/pg_control", PGDATA);
            struct stat st;
            int rc = stat(ctrl_path, &st);
            PGL_DEBUG_PRINT(stderr, "[pgl_main] post-boot ctrl=%s rc=%d errno=%d size=%lld\n",
                    ctrl_path, rc, errno, (long long)((rc == 0) ? st.st_size : 0));
            PGL_LOG_INFO("[pgl_main] post-boot ctrl=%s rc=%d errno=%d size=%lld",
                         ctrl_path, rc, errno, (long long)((rc == 0) ? st.st_size : 0));
        }
        PGL_DEBUG_PRINT(stderr, "[pgl_main] bootstrap section completed successfully\n");
        PGL_LOG_ERROR("%s", "[pgl_main] *** BOOTSTRAP SECTION COMPLETED SUCCESSFULLY ***");
        PGL_LOG_ERROR("%s", "[pgl_main] *** EXITING BOOTSTRAP BLOCK ***");
        PGL_PROFILE_PHASE("BootstrapModeMain");
    }

    PGL_LOG_ERROR("%s", "[pgl_initdb] *** PAST BOOTSTRAP SECTION, CONTINUING TO CLEANUP ***");

    /* use previous initdb output to feed single mode */

    /* or resume a previous db */
    // IsPostmasterEnvironment = true;
    if (TransamVariables && TransamVariables->nextOid < ((Oid)FirstNormalObjectId))
    {
#if PGDEBUG
        puts("# 482: warning oid base too low, will need to set OID range after initdb(bootstrap/single)");
#endif
    }
    /*
        {
    #if PGDEBUG
            fprintf(stdout, "\n\n\n# 483: restarting in single mode for initdb with user '%s' instead of %s\n", getenv("PGUSER"), PGUSER);
    #endif
            char *single_argv[] = {
                WASM_PREFIX "/bin/postgres",
                "--single",
                "-d", "1", "-B", "16", "-S", "512", "-f", "siobtnmh",
                "-D", PGDATA,
                "-c", "log_startup_progress_interval=0",
                "-F", "-O", "-j",
                WASM_PGOPTS,
                "template1",
                NULL
            };
            int single_argc = sizeof(single_argv) / sizeof(char*) - 1;
            optind = 1;
            RePostgresSingleUserMain(single_argc, single_argv, WASM_USERNAME);
    PDEBUG("# 498: initdb faking shutdown to complete WAL/OID states in single mode");
            async_restart = 1;
        }
    */
    async_restart = 1;
initdb_done:;
    PGL_LOG_INFO("[pgl_initdb] Reached initdb_done label");
    pgl_idb_status |= IDB_CALLED;

    if (optind > 0)
    {
        /* RESET getopt */
        optind = 1;
        /* we did not fail, clear the default failed state */
        pgl_idb_status &= IDB_OK;
    }
    else
    {
        PDEBUG("# 524: exiting on initdb-single error");
        // TODO raise js exception
    }
    PGL_LOG_INFO("[pgl_initdb] EXIT: returning %d", pgl_idb_status);
    PGL_LOG_ERROR("%s", "[pgl_initdb] *** FINAL: About to return from pgl_initdb function ***");
    PGL_LOG_ERROR("%s", "[pgl_initdb] *** If you see this message, pgl_initdb completed successfully ***");
    PGL_DEBUG_PRINT(stderr, "[pgl_initdb] *** RETURNING FROM pgl_initdb WITH STATUS %d ***\n", pgl_idb_status);
    PGL_LOG_ERROR("%s", "[pgl_initdb] *** ABOUT TO EXECUTE RETURN STATEMENT ***");
    PGL_PROFILE_TOTAL("pgl_initdb");
    return pgl_idb_status;
} // pgl_initdb

/*
    PGDATESTYLE
    TZ
    PG_SHMEM_ADDR

    PGCTLTIMEOUT
    PG_TEST_USE_UNIX_SOCKETS
    INITDB_TEMPLATE
    PSQL_HISTORY
    TMPDIR
    PGOPTIONS
*/

#if !defined(PGL_LIB_ONLY)
// __attribute__((export_name("main")))
int main(int argc, char **argv)
{
    int exit_code = 0;
    main_pre(argc, argv);
#if PGDEBUG
    printf("# 550: argv0 (%s) PGUSER=%s PGDATA=%s\n PGDATABASE=%s REPL=%s\n",
           argv[0], PGUSER, PGDATA, getenv("PGDATABASE"), getenv("REPL"));
#endif
    progname = get_progname(argv[0]);
    startup_hacks(progname);
    g_argv = argv;
    g_argc = argc;

    is_repl = strlen(getenv("REPL")) && getenv("REPL")[0] != 'N';
    is_embed = true;

    if (!is_repl)
    {
        PDEBUG("# 562: exit with live runtime (nodb)");
        return 0;
    }
#if defined(__wasi__)

#else
    /*
    main_post();

    PDEBUG("# 565: repl");
    // so it is repl
    main_repl();

    if (is_node) {
        PDEBUG("# 570: node repl");
        pg_repl_raf();
    }
    */
    emscripten_force_exit(exit_code);
#endif
    return exit_code;
}
#endif // !PGL_LIB_ONLY
