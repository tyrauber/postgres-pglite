#include "pgl_mobile_compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../pglite-wasm/pgl_os.h"

// Provide a stable mobile symbol name expected by the RN bridge.
// Map pgl_shutdown() to the implementation in pgl_mains.c (pg_shutdown).
extern void pg_shutdown(void);
void pgl_shutdown(void) { pg_shutdown(); }

// NOTE: find_my_exec and find_other_exec are now handled in PostgreSQL's
// src/common/exec.c with PGL_MOBILE support. The mobile implementations
// use environment variables (PREFIX, ANDROID_DATA_DIR) to construct paths
// without probing the filesystem.
// See: vendor/postgres-pglite/src/common/exec.c

// NOTE: get_share_path is now handled directly in PostgreSQL's src/port/path.c
// with PGL_MOBILE support. It uses environment variables (PGSYSCONFDIR,
// IOS_RUNTIME_DIR, ANDROID_RUNTIME_DIR) to find bundled PostgreSQL data files.
// See: vendor/postgres-pglite/src/port/path.c

// NOTE: optreset is now provided in src/port/getopt.c for Linux/PGL_MOBILE builds.
// See the #if defined(PGL_MOBILE) && defined(__linux__) block there.

