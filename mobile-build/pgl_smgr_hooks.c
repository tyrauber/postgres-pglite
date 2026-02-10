/*
 * pgl_smgr_hooks.c - SMgr-level storage hooks for PGLite mobile/daemon builds
 *
 * Provides hooks for both interception (read) and notification (write, etc.)
 * of PostgreSQL storage manager operations.
 *
 * Primary use case: HTTP-based storage where S3/CloudFront is the database.
 * - on_read: Intercept reads to fetch from HTTP instead of local disk
 * - on_write: Notification with buffer data for S3 upload
 * - on_prefetch: Async hint for background HTTP prefetch
 * - on_checkpoint: Sync point - block until S3 uploads complete
 *
 * Called from smgr.c (read/write/extend/sync/truncate/unlink) and checkpointer.c
 * (checkpoint complete). All calls are guarded by PGL_MOBILE ifdefs at the
 * call sites.
 *
 * The dispatch functions accept void* for the RelFileLocator to avoid requiring
 * PostgreSQL headers. The struct layout is identical to pgl_relfilelocator
 * (3 x uint32_t: spcOid, dbOid, relNumber).
 */
#include "pgl_smgr_hooks.h"

#include <string.h>

/* Global hook function pointers (NULL = no callback) */
static pgl_smgr_read_hook_fn       g_read_hook       = NULL;
static pgl_smgr_prefetch_hook_fn   g_prefetch_hook   = NULL;
static pgl_smgr_write_hook_fn      g_write_hook      = NULL;
static pgl_smgr_extend_hook_fn     g_extend_hook     = NULL;
static pgl_smgr_sync_hook_fn       g_sync_hook       = NULL;
static pgl_smgr_truncate_hook_fn   g_truncate_hook   = NULL;
static pgl_smgr_unlink_hook_fn     g_unlink_hook     = NULL;
static pgl_smgr_checkpoint_hook_fn g_checkpoint_hook = NULL;

void
pgl_register_smgr_hooks(const pgl_smgr_hooks *hooks)
{
	if (hooks == NULL)
	{
		g_read_hook       = NULL;
		g_prefetch_hook   = NULL;
		g_write_hook      = NULL;
		g_extend_hook     = NULL;
		g_sync_hook       = NULL;
		g_truncate_hook   = NULL;
		g_unlink_hook     = NULL;
		g_checkpoint_hook = NULL;
		return;
	}

	g_read_hook       = hooks->on_read;
	g_prefetch_hook   = hooks->on_prefetch;
	g_write_hook      = hooks->on_write;
	g_extend_hook     = hooks->on_extend;
	g_sync_hook       = hooks->on_sync;
	g_truncate_hook   = hooks->on_truncate;
	g_unlink_hook     = hooks->on_unlink;
	g_checkpoint_hook = hooks->on_checkpoint;
}

/*
 * Convert a PostgreSQL RelFileLocator (void* to avoid PG header dependency)
 * to our plain-C pgl_relfilelocator. Layout is identical: 3 x uint32_t.
 */
static inline pgl_relfilelocator
convert_rlocator(const void *pg_rlocator)
{
	pgl_relfilelocator loc;
	memcpy(&loc, pg_rlocator, sizeof(pgl_relfilelocator));
	return loc;
}

/*
 * Internal dispatch functions — called from smgr.c under #ifdef PGL_MOBILE.
 * Each checks its hook pointer and returns immediately if NULL (zero overhead).
 */

/*
 * Try to read pages via hook (for HTTP storage).
 * Returns PGL_TRUE if hook filled all buffers, PGL_FALSE to fall through to md.c.
 */
int
pgl_smgr_try_read(const void *rlocator, int forknum,
                  uint32_t blocknum, void **buffers, uint32_t nblocks)
{
	if (g_read_hook)
		return g_read_hook(convert_rlocator(rlocator), forknum, blocknum, buffers, nblocks);
	return PGL_FALSE;
}

/*
 * Notify prefetch hook (async hint for background HTTP fetch).
 */
void
pgl_smgr_notify_prefetch(const void *rlocator, int forknum,
                         uint32_t blocknum, uint32_t nblocks)
{
	if (g_prefetch_hook)
		g_prefetch_hook(convert_rlocator(rlocator), forknum, blocknum, nblocks);
}

/*
 * Notify write hook with buffer data (for S3 upload).
 */
void
pgl_smgr_notify_write(const void *rlocator, int forknum,
                      uint32_t blocknum, const void **buffers, uint32_t nblocks)
{
	if (g_write_hook)
		g_write_hook(convert_rlocator(rlocator), forknum, blocknum, buffers, nblocks);
}

/*
 * Notify extend hook with buffer data (for S3 upload of new pages).
 */
void
pgl_smgr_notify_extend(const void *rlocator, int forknum,
                       uint32_t blocknum, const void **buffers, uint32_t nblocks)
{
	if (g_extend_hook)
		g_extend_hook(convert_rlocator(rlocator), forknum, blocknum, buffers, nblocks);
}

void
pgl_smgr_notify_sync(const void *rlocator, int forknum)
{
	if (g_sync_hook)
		g_sync_hook(convert_rlocator(rlocator), forknum);
}

void
pgl_smgr_notify_truncate(const void *rlocator, int forknum,
                         uint32_t old_blocks, uint32_t new_blocks)
{
	if (g_truncate_hook)
		g_truncate_hook(convert_rlocator(rlocator), forknum, old_blocks, new_blocks);
}

void
pgl_smgr_notify_unlink(const void *rlocator, int forknum)
{
	if (g_unlink_hook)
		g_unlink_hook(convert_rlocator(rlocator), forknum);
}

void
pgl_smgr_notify_checkpoint(void)
{
	if (g_checkpoint_hook)
		g_checkpoint_hook();
}
