#pragma once

#include <stdio.h> // FILE+fprintf
#include <stdlib.h>
#include <string.h>
#ifndef PGL_INITDB_MAIN
#define PGL_INITDB_MAIN
#endif
#include <setjmp.h>

/*
 * and now popen will return predefined slot from a file list
 * as file handle in initdb.c
 */



/*
 * popen is routed via pg_popen to stderr or a IDB_PIPE_* file
 * link a pclose replacement when we are in exec.c ( PG_EXEC defined )
 */

extern FILE * pgl_popen(const char *command, const char *type);
#define popen(command, mode) pgl_popen(command, mode)
// #define popen_check(command, mode) pgl_popen(command, mode)

extern int pgl_pclose(FILE *stream);
#define pclose(stream) pgl_pclose(stream)
#define pclose_check(stream) pgl_pclose(stream)


int
pg_chmod(const char * path, int mode_t) {
    return 0;
}

#ifdef FRONTEND
#undef FRONTEND
#endif

#define FRONTEND
#   include "common/logging.c"
#undef FRONTEND


#include "interfaces/libpq/pqexpbuffer.c"

// On Android/mobile glue, ensure initdb does not attempt POSIX shm APIs
#undef HAVE_SHM_OPEN
#undef HAVE_SHM_UNLINK


#define sync_pgdata(...)
#define icu_language_tag(loc_str) icu_language_tag_idb(loc_str)
#define icu_validate_locale(loc_str) icu_validate_locale_idb(loc_str)


#ifdef PGL_MOBILE
// Force initdb to use our mobile discovery functions instead of probing real binaries
#define find_other_exec find_other_exec_mobile
#define get_share_path  get_share_path_mobile

#include "pgl_config.h"

static int find_other_exec_mobile(const char *argv0, const char *target, const char *versionstr, char *retpath) {
    (void)argv0; (void)versionstr;

    /*
     * PRIORITY 1: Check if pgl_init() was called with explicit config.
     */
    if (pgl_is_initialized()) {
        if (pgl_get_argv0_internal(retpath, MAXPGPATH) == 0) {
            return 0; // success
        }
    }

    /*
     * PRIORITY 2: Legacy environment variable fallback.
     */
    const char* prefix = getenv("PREFIX");
    if (!prefix || !*prefix) prefix = getenv("ANDROID_DATA_DIR");
    if (!prefix || !*prefix) prefix = "/data/local/tmp/pglite";
    const char* tgt = target && *target ? target : "postgres";
    snprintf(retpath, MAXPGPATH, "%s/bin/%s", prefix, tgt);
    return 0; // success
}

static void get_share_path_mobile(const char *my_exec_path, char *ret_path) {
    (void)my_exec_path;

    /*
     * PRIORITY 1: Check if pgl_init() was called with explicit config.
     * This is the preferred path - explicit configuration over env vars.
     */
    if (pgl_is_initialized()) {
        if (pgl_get_share_path_internal(ret_path, MAXPGPATH) == 0) {
            return;
        }
    }

    /*
     * PRIORITY 2: Legacy environment variable cascade.
     * Priority order for initdb share path resolution:
     * 1. PREFIX - explicit app prefix (set by test harness or pglite-daemon)
     * 2. IOS_RUNTIME_DIR / ANDROID_RUNTIME_DIR - mobile runtime bundles
     * 3. PGSYSCONFDIR - legacy; BUT exec.c auto-sets this to PREFIX/etc via
     *    get_etc_path(), so we must check PREFIX first to avoid the wrong path.
     * 4. Fallback to /data/local/tmp/pglite
     *
     * NOTE: PGSYSCONFDIR is checked last because set_pglocale_pgservice()
     * (called early in initdb) auto-sets it to <exec_dir>/../etc which
     * does NOT contain share/postgresql.
     */
    const char* prefix = getenv("PREFIX");
    if (prefix && *prefix) {
        snprintf(ret_path, MAXPGPATH, "%s/share/postgresql", prefix);
        return;
    }
#ifdef __APPLE__
    const char* runtime = getenv("IOS_RUNTIME_DIR");
#else
    const char* runtime = getenv("ANDROID_RUNTIME_DIR");
#endif
    if (runtime && *runtime) {
        snprintf(ret_path, MAXPGPATH, "%s/share/postgresql", runtime);
        return;
    }
    const char* conf = getenv("PGSYSCONFDIR");
    if (conf && *conf) {
        snprintf(ret_path, MAXPGPATH, "%s/share/postgresql", conf);
        return;
    }
    // Last resort
    snprintf(ret_path, MAXPGPATH, "/data/local/tmp/pglite/share/postgresql");
}
#endif

#ifdef PGL_CATCH_EXIT
// Catch initdb's exit() and convert into a longjmp back to pgl_initdb_safe
static jmp_buf g_initdb_jmp;
static int g_initdb_status = 0;
static void pglite_initdb_exit(int code) {
    g_initdb_status = code;
    longjmp(g_initdb_jmp, 1);
}
#define exit(code) pglite_initdb_exit(code)
#endif

#include "bin/initdb/initdb.c"

#ifdef PGL_CATCH_EXIT
// Safe wrapper that prevents process termination on initdb failures
int pgl_initdb_safe(void) {
    g_initdb_status = 0;
    if (setjmp(g_initdb_jmp) == 0) {
        (void)pgl_initdb_main();
        return 0;
    }
    return g_initdb_status ? g_initdb_status : -1;
}
#endif

void use_socketfile(void) {
    is_repl = true;
    is_embed = false;
}



