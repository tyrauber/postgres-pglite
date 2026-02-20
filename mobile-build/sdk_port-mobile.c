#include "sdk_port-mobile.h"
#include <stdlib.h>
#include <string.h>
#include "../pglite-wasm/pgl_os.h"

/* When included in PostgreSQL build, these variables are defined in postgres.c */
#ifdef POSTGRES_H
extern volatile int cma_rsize;  /* defined in postgres.c */
#endif

#ifndef POSTGRES_H
/* Standalone mobile build - define variables here */
volatile int cma_rsize = 0;
#endif

/* External variables defined in pqcomm.c, set by mobile SDK */
extern volatile int pgl_mobile_cma_wsize;
extern int original_request_size;  /* Defined in pgl_mobile_comm.c */

/* Mobile: Single definition point for these globals (used by interactive_one.c) */
volatile int channel = 0;
volatile bool is_wire = false;  /* Default to REPL mode for bootstrap */
volatile bool is_repl = true;   /* Start in REPL mode */

// Single channel buffer for now
#ifndef CMA_MB
#define CMA_MB 12
#endif
#ifndef CMA_FD
#define CMA_FD 1
#endif

static uint8_t* g_buf = NULL;
static int g_cap = 0;

/* External variables defined in pqcomm.c, set by mobile SDK */
extern void* pgl_mobile_cma_buffer_addr;
extern int pgl_mobile_cma_buffer_size;

static void ensure_buf() {
  if (!g_buf) {
    g_cap = (CMA_MB * 1024 * 1024) / CMA_FD;
    void* p = NULL;
    if (posix_memalign(&p, 16, (size_t)g_cap + 2) != 0) {
      p = NULL;
    }
    g_buf = (uint8_t*)p;
    if (g_buf) {
      memset(g_buf, 0, (size_t)g_cap + 2);
      /* Set external variables for PostgreSQL to access this buffer */
      pgl_mobile_cma_buffer_addr = g_buf + 1;  /* Match WASM offset */
      pgl_mobile_cma_buffer_size = g_cap;
      PGL_LOG_INFO("ensure_buf: set pgl_mobile_cma_buffer_addr=%p, size=%d, g_buf=%p", 
                         pgl_mobile_cma_buffer_addr, pgl_mobile_cma_buffer_size, (void*)g_buf);
    }
  }
}

int get_buffer_size(int fd) {
  (void)fd;
  ensure_buf();
  return g_cap;
}

intptr_t get_buffer_addr(int fd) {
  (void)fd;
  ensure_buf();
  // Return native pointer to (buf + 1) to match WASM IO semantics
  return (intptr_t)(g_buf + 1);
}

int interactive_read(void) {
  PGL_LOG_INFO("interactive_read: pgl_mobile_cma_wsize=%d, addr=%p", pgl_mobile_cma_wsize, (void*)&pgl_mobile_cma_wsize);
  return pgl_mobile_cma_wsize;
}

void use_wire(int state) {
  is_wire = (state > 0);
  extern volatile bool is_repl;
  is_repl = !is_wire;

  /* When switching to REPL mode, clear request size to avoid confusion */
  if (!is_wire) {
    original_request_size = 0;
    pgl_mobile_cma_wsize = 0;
    PGL_LOG_INFO("use_wire: switched to REPL mode, cleared request sizes");
  }
}

/* External variable defined in interactive_one.c (included via pg_main.c) */
extern bool mobile_auth_started;

/*
 * Skip startup auth for TCP servers that handle auth externally.
 * This sets mobile_auth_started=true so interactive_one() won't call startup_auth()
 * on the first query message. Must be called after pgl_backend() and use_wire(1).
 */
void pgl_skip_auth(void) {
  mobile_auth_started = true;
  PGL_LOG_INFO("pgl_skip_auth: mobile_auth_started set to true");
}

/*
 * Reset wire protocol session state for new client connections.
 *
 * When a client disconnects and a new one connects, call this function to
 * reset all wire protocol state so the new client can start fresh with
 * proper authentication flow.
 *
 * This resets:
 * - mobile_auth_started flag (so auth happens for new client)
 * - CMA buffer sizes (cma_rsize, pgl_mobile_cma_wsize)
 * - PostgreSQL receive/send buffer pointers
 * - Channel state
 * - mobile send buffer (original_request_size)
 *
 * Usage in pglite-daemon:
 *   // When client disconnects:
 *   pgl_reset_wire_session();
 *   // Now ready for new client connection
 */
/* --- Init Profiling --- */

/* Runtime flag for init profiling - default OFF */
volatile bool pgl_profile_enabled = false;

void pgl_enable_profiling(bool enable) {
  pgl_profile_enabled = enable;
  if (enable) {
    fprintf(stderr, "[PGL_PROFILE] profiling enabled\n");
    fflush(stderr);
  }
}

/* External from pg_main.c - controls initdb replay vs existing-db path */
extern volatile int async_restart;
extern volatile int pgl_idb_status;

/*
 * Cold-start mode: tell pgl_backend() that a database already exists on disk.
 *
 * In serverless scenarios (Lambda cold-start), each invocation is a fresh process
 * but the database files exist (from S3/EFS). Without this call, pgl_backend()
 * would try to replay initdb which fails on an existing database.
 *
 * Must be called BEFORE pgl_backend().
 */
void pgl_set_existing_db(void) {
  /* Skip initdb replay - go directly to existing-db initialization path */
  async_restart = 0;
  /* Mark initdb as "called" so the warning check passes */
  pgl_idb_status = 0b11111110;  /* IDB_OK value from pg_main.c */
  fprintf(stderr, "[PGL] pgl_set_existing_db: cold-start mode enabled\n");
  fflush(stderr);
}

/* --- WAL Archive Callback --- */

/* Global callback pointer defined in xlogarchive.c, NULL means no archive callback registered */
extern pgl_archive_callback_t pgl_archive_callback_fn;

/* Flag: if true, pgl_backend() will set archive_mode=on in its argv */
volatile bool pgl_archiving_enabled = false;

void pgl_enable_archiving(void) {
  pgl_archiving_enabled = true;
  PGL_LOG_INFO("pgl_enable_archiving: archive_mode will be enabled on next pgl_backend()");
}

void pgl_set_archive_callback(pgl_archive_callback_t callback) {
  pgl_archive_callback_fn = callback;
  PGL_LOG_INFO("pgl_set_archive_callback: %s", callback ? "registered" : "cleared");
}

/*
 * Mark a WAL segment as successfully archived.
 * Must be called by the archive callback after uploading to S3/etc.
 * This renames {segment}.ready -> {segment}.done so PostgreSQL can recycle it.
 */
void pgl_archive_done(const char *segment_name) {
  /* Implemented via XLogArchiveForceDone in xlogarchive.c.
   * We call it through a function pointer set during PostgreSQL init,
   * since we can't include PostgreSQL headers here. */
  extern void XLogArchiveForceDone(const char *);
  XLogArchiveForceDone(segment_name);
  PGL_LOG_INFO("pgl_archive_done: marked %s as done", segment_name);
}

/* --- Wire Session Reset --- */

void pgl_reset_wire_session(void) {
  PGL_LOG_INFO("pgl_reset_wire_session: resetting wire protocol state for new connection");

  /* Reset auth flag so new client goes through auth flow */
  mobile_auth_started = false;

  /* Reset CMA buffer state */
  cma_rsize = 0;
  pgl_mobile_cma_wsize = 0;
  channel = 0;

  /* Reset PostgreSQL buffer state (receive + send buffers) */
  extern void pq_reset_session_state(void);
  pq_reset_session_state();

  /* Reset mobile comm original_request_size */
  extern int original_request_size;
  original_request_size = 0;

  PGL_LOG_INFO("pgl_reset_wire_session: reset complete - mobile_auth_started=%d cma_rsize=%d cma_wsize=%d",
               mobile_auth_started, cma_rsize, pgl_mobile_cma_wsize);
}

