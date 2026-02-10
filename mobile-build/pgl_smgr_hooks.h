/*
 * pgl_smgr_hooks.h - SMgr-level storage hooks for PGLite mobile/daemon builds
 *
 * Provides relation-aware callbacks for PostgreSQL's storage manager operations.
 * Supports both notification hooks (fire after operation) and intercept hooks
 * (can replace operation).
 *
 * Primary use case: HTTP-based storage where S3/CloudFront is the database.
 * - on_read: Intercept reads to fetch pages from HTTP instead of local disk
 * - on_write: Notification with buffer data for S3 upload
 * - on_prefetch: Async hint to prefetch pages from HTTP
 * - on_checkpoint: Sync point - block until all S3 uploads complete
 *
 * External code (e.g. Go via cgo) can use these hooks to implement:
 * - S3/CloudFront page storage
 * - Local disk caching with HTTP fallback
 * - Dirty page tracking
 * - Custom storage tiers
 *
 * Usage:
 *   pgl_smgr_hooks hooks = {
 *       .on_read = my_http_read_callback,
 *       .on_write = my_s3_upload_callback,
 *       .on_checkpoint = my_flush_uploads_callback,
 *       // NULL fields = no callback (use default behavior)
 *   };
 *   pgl_register_smgr_hooks(&hooks);
 *   // Pass NULL to clear all hooks
 *
 * GOTCHAS:
 * - Hooks must NOT call back into PostgreSQL (no palloc, ereport, SQL)
 * - Hooks called from single PostgreSQL backend thread
 * - on_read: nblocks can be >1 - must fill ALL buffers or return false
 * - on_read returning false triggers md.c fallback (local disk read)
 * - on_checkpoint MUST block until all uploads are durable on S3
 */
#ifndef PGL_SMGR_HOOKS_H
#define PGL_SMGR_HOOKS_H

#include <stdint.h>

/* Boolean return values for hooks (C89 compatible) */
#define PGL_TRUE  1
#define PGL_FALSE 0

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fork number constants for CGO consumers.
 * These match PostgreSQL's ForkNumber enum in common/relpath.h.
 */
#define PGL_MAIN_FORKNUM     0   /* Main data fork */
#define PGL_FSM_FORKNUM      1   /* Free space map (derived, skip for S3) */
#define PGL_VM_FORKNUM       2   /* Visibility map (derived, skip for S3) */
#define PGL_INIT_FORKNUM     3   /* Initialization fork (unlogged tables) */

/*
 * Block size constant - each page is exactly this many bytes.
 */
#define PGL_BLCKSZ           8192

/*
 * Simplified relation file locator using plain C types.
 * External consumers (Go/cgo) don't need PostgreSQL headers.
 * Maps to PostgreSQL's RelFileLocator: (spcOid, dbOid, relNumber).
 */
typedef struct pgl_relfilelocator {
	uint32_t spcOid;     /* tablespace OID (usually 1663 for default) */
	uint32_t dbOid;      /* database OID */
	uint32_t relNumber;  /* relation file number */
} pgl_relfilelocator;

/*
 * Hook function pointer types.
 *
 * forknum: Use PGL_*_FORKNUM constants above
 * blocknum/nblocks: In 8KB PostgreSQL blocks (PGL_BLCKSZ bytes each)
 */

/*
 * Read hook - INTERCEPT reads to fetch from HTTP/cache instead of local disk.
 *
 * Return true if hook filled ALL buffers (skip md.c local read).
 * Return false to fall through to md.c for local disk read.
 *
 * buffers: Array of nblocks pointers, each pointing to PGL_BLCKSZ bytes.
 *          Hook must fill ALL of them or return false.
 *
 * Called BEFORE md.c read. Safe for blocking HTTP calls (no locks held).
 */
typedef int (*pgl_smgr_read_hook_fn)(pgl_relfilelocator rlocator, int forknum,
                                     uint32_t blocknum, void **buffers,
                                     uint32_t nblocks);

/*
 * Prefetch hook - ASYNC hint to fetch pages in background.
 *
 * Called by PostgreSQL's sequential scan planner, bitmap heap scans, VACUUM,
 * and index builds. Return immediately - fetch asynchronously.
 *
 * Daemon can queue these for background HTTP fetch to warm local cache.
 */
typedef void (*pgl_smgr_prefetch_hook_fn)(pgl_relfilelocator rlocator, int forknum,
                                          uint32_t blocknum, uint32_t nblocks);

/*
 * Write hook - NOTIFICATION that pages were written to local disk.
 *
 * Called AFTER md.c write succeeds. Safe for async upload (no locks held).
 * Buffer data is valid during callback - copy if needed for async upload.
 *
 * buffers: Array of nblocks pointers to written page data (PGL_BLCKSZ each).
 *
 * Recommendation: Queue for background S3 upload, drain queue on checkpoint.
 */
typedef void (*pgl_smgr_write_hook_fn)(pgl_relfilelocator rlocator, int forknum,
                                       uint32_t blocknum, const void **buffers,
                                       uint32_t nblocks);

/*
 * Extend hook - NOTIFICATION that relation was extended with new pages.
 *
 * Similar to write hook but for newly allocated pages.
 * buffers: Array of nblocks pointers to new page data.
 */
typedef void (*pgl_smgr_extend_hook_fn)(pgl_relfilelocator rlocator, int forknum,
                                        uint32_t blocknum, const void **buffers,
                                        uint32_t nblocks);

/*
 * Sync hook - NOTIFICATION that relation fork was fsynced.
 */
typedef void (*pgl_smgr_sync_hook_fn)(pgl_relfilelocator rlocator, int forknum);

/*
 * Truncate hook - NOTIFICATION that relation was truncated.
 */
typedef void (*pgl_smgr_truncate_hook_fn)(pgl_relfilelocator rlocator, int forknum,
                                          uint32_t old_blocks, uint32_t new_blocks);

/*
 * Unlink hook - NOTIFICATION that relation file was deleted.
 */
typedef void (*pgl_smgr_unlink_hook_fn)(pgl_relfilelocator rlocator, int forknum);

/*
 * Checkpoint hook - SYNC point, MUST block until all uploads are durable.
 *
 * Called AFTER CreateCheckPoint() completes (all dirty buffers flushed,
 * pg_control updated). For S3 storage, this hook MUST block until all
 * queued page uploads are confirmed durable on S3.
 *
 * This is the consistency boundary - after checkpoint returns, all data
 * up to this point must be recoverable from S3.
 */
typedef void (*pgl_smgr_checkpoint_hook_fn)(void);

/*
 * Hook registration struct. NULL fields = no callback for that event.
 *
 * For HTTP storage, typical setup:
 *   .on_read = http_page_fetch,       // Intercept reads
 *   .on_prefetch = http_prefetch,     // Async prefetch hints
 *   .on_write = s3_upload_queue,      // Queue page uploads
 *   .on_extend = s3_upload_queue,     // Queue new page uploads
 *   .on_checkpoint = s3_flush_queue,  // Block until uploads complete
 */
typedef struct pgl_smgr_hooks {
	pgl_smgr_read_hook_fn       on_read;       /* intercept page reads */
	pgl_smgr_prefetch_hook_fn   on_prefetch;   /* async prefetch hint */
	pgl_smgr_write_hook_fn      on_write;      /* page write notification */
	pgl_smgr_extend_hook_fn     on_extend;     /* relation extend notification */
	pgl_smgr_sync_hook_fn       on_sync;       /* relation fsync notification */
	pgl_smgr_truncate_hook_fn   on_truncate;   /* relation truncate notification */
	pgl_smgr_unlink_hook_fn     on_unlink;     /* relation unlink notification */
	pgl_smgr_checkpoint_hook_fn on_checkpoint; /* checkpoint complete - SYNC */
} pgl_smgr_hooks;

/*
 * Register SMgr hooks. Pass NULL for any field to skip that event.
 * Pass NULL for the entire struct to clear all hooks.
 */
extern void pgl_register_smgr_hooks(const pgl_smgr_hooks *hooks);

/*
 * Internal dispatch functions - called from smgr.c, checkpointer.c.
 * These accept PostgreSQL types internally and convert to pgl_relfilelocator.
 * Do NOT call from external code.
 */

/* Returns PGL_TRUE if hook handled the read (filled buffers), PGL_FALSE for md.c fallback */
extern int pgl_smgr_try_read(const void *rlocator, int forknum,
                             uint32_t blocknum, void **buffers, uint32_t nblocks);

extern void pgl_smgr_notify_prefetch(const void *rlocator, int forknum,
                                     uint32_t blocknum, uint32_t nblocks);

extern void pgl_smgr_notify_write(const void *rlocator, int forknum,
                                  uint32_t blocknum, const void **buffers,
                                  uint32_t nblocks);

extern void pgl_smgr_notify_extend(const void *rlocator, int forknum,
                                   uint32_t blocknum, const void **buffers,
                                   uint32_t nblocks);

extern void pgl_smgr_notify_sync(const void *rlocator, int forknum);

extern void pgl_smgr_notify_truncate(const void *rlocator, int forknum,
                                     uint32_t old_blocks, uint32_t new_blocks);

extern void pgl_smgr_notify_unlink(const void *rlocator, int forknum);

extern void pgl_smgr_notify_checkpoint(void);

#ifdef __cplusplus
}
#endif

#endif /* PGL_SMGR_HOOKS_H */
