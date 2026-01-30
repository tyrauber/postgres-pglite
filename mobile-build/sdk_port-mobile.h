#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Simple mobile CMA-like buffer semantics used by interactive_one.c
int get_buffer_size(int fd);       // capacity for channel fd
intptr_t get_buffer_addr(int fd);  // native pointer to buffer base + 1 for IO[]
// interactive_write is defined in interactive_one.c for both WASM and mobile
int interactive_read(void);        // return cma_wsize
void use_wire(int state);          // >0 wire mode, <=0 repl

// Skip startup auth for TCP servers that handle auth externally
// Must be called after pgl_backend() and use_wire(1) but before first interactive_one()
void pgl_skip_auth(void);

// Reset wire protocol session state for new client connections
// Call this when a client disconnects before accepting a new connection
// Resets: mobile_auth_started, CMA buffers, PQ send/recv buffers, channel
void pgl_reset_wire_session(void);

// I/O hook registration for custom storage layers (EFS, S3, etc.)
// See pgl_io_hooks.h for the full type definitions.
#include "pgl_io_hooks.h"

// SMgr-level storage hooks for relation-aware callbacks (write, sync, checkpoint).
// Fires after md.c operations succeed — provides semantic context (which table/index
// changed, which fork, which blocks) for S3 upload or dirty-relation tracking.
// See pgl_smgr_hooks.h for the full type definitions.
#include "pgl_smgr_hooks.h"

// WAL archive callback - called when a WAL segment is ready to archive.
// In embedded/library mode, PostgreSQL's archiver process can't run (no fork),
// so this callback replaces archive_command for pushing WAL segments to S3, etc.
//
// Parameters:
//   segment_path - full path to the WAL segment file (e.g. "/tmp/pglite/base/pg_wal/000000010000000000000001")
//   segment_name - basename only (e.g. "000000010000000000000001")
//
// After successfully archiving, the callback MUST call pgl_archive_done(segment_name)
// so PostgreSQL can recycle the WAL segment.
typedef void (*pgl_archive_callback_t)(const char *segment_path, const char *segment_name);

void pgl_set_archive_callback(pgl_archive_callback_t callback);
void pgl_archive_done(const char *segment_name);

// Enable WAL archiving mode. Must be called BEFORE pgl_backend().
// Sets archive_mode=on so PostgreSQL invokes XLogArchiveNotify() when
// WAL segments are ready, which triggers the registered archive callback.
// Without this, XLogArchivingActive() returns false and the callback never fires.
void pgl_enable_archiving(void);

// Check if archiving was enabled (used internally by pgl_backend startup)
extern volatile bool pgl_archiving_enabled;

// Expose variables similar to wasm build  
extern volatile int pgl_mobile_cma_wsize;  // External variable defined in pqcomm.c
extern volatile int cma_rsize;
extern volatile int channel;
extern volatile bool is_wire;
extern volatile bool is_repl;

#ifdef __cplusplus
}
#endif

