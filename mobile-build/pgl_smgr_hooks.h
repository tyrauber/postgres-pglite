/*
 * pgl_smgr_hooks.h - SMgr-level storage hooks for PGLite mobile/daemon builds
 *
 * Provides relation-aware callbacks that fire when PostgreSQL's storage
 * manager writes, extends, syncs, truncates, or unlinks relation files.
 * Also fires on checkpoint completion.
 *
 * This complements pgl_io_hooks.h (POSIX-level fd-based interception) by
 * providing semantic context: which relation (table/index) changed, which
 * fork, which blocks. External code (e.g. Go via cgo) can use these hooks
 * to implement S3 upload, dirty-relation tracking, or custom storage tiers.
 *
 * Design: md.c continues doing all real I/O. These hooks are notification-only
 * callbacks that fire AFTER the md.c operation succeeds. They do not replace
 * or intercept the I/O path.
 *
 * Usage:
 *   pgl_smgr_hooks hooks = {
 *       .on_write  = my_write_callback,
 *       .on_checkpoint = my_checkpoint_callback,
 *       // NULL fields = no callback for that event
 *   };
 *   pgl_register_smgr_hooks(&hooks);
 *   // Pass NULL to clear all hooks
 */
#ifndef PGL_SMGR_HOOKS_H
#define PGL_SMGR_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Simplified relation file locator using plain C types.
 * External consumers (Go/cgo) don't need PostgreSQL headers.
 * Maps to PostgreSQL's RelFileLocator: (spcOid, dbOid, relNumber).
 */
typedef struct pgl_relfilelocator {
	uint32_t spcOid;     /* tablespace OID */
	uint32_t dbOid;      /* database OID */
	uint32_t relNumber;  /* relation file number */
} pgl_relfilelocator;

/*
 * Hook function pointer types.
 *
 * forknum values: 0=main, 1=fsm, 2=vm, 3=init
 * blocknum/nblocks are in 8KB PostgreSQL blocks.
 */
typedef void (*pgl_smgr_write_hook_fn)(pgl_relfilelocator rlocator, int forknum,
                                       uint32_t blocknum, uint32_t nblocks);
typedef void (*pgl_smgr_extend_hook_fn)(pgl_relfilelocator rlocator, int forknum,
                                        uint32_t blocknum, uint32_t nblocks);
typedef void (*pgl_smgr_sync_hook_fn)(pgl_relfilelocator rlocator, int forknum);
typedef void (*pgl_smgr_truncate_hook_fn)(pgl_relfilelocator rlocator, int forknum,
                                          uint32_t old_blocks, uint32_t new_blocks);
typedef void (*pgl_smgr_unlink_hook_fn)(pgl_relfilelocator rlocator, int forknum);
typedef void (*pgl_smgr_checkpoint_hook_fn)(void);

/*
 * Hook registration struct. NULL fields = no callback for that event.
 */
typedef struct pgl_smgr_hooks {
	pgl_smgr_write_hook_fn      on_write;      /* relation blocks written */
	pgl_smgr_extend_hook_fn     on_extend;     /* relation extended */
	pgl_smgr_sync_hook_fn       on_sync;       /* relation fsynced */
	pgl_smgr_truncate_hook_fn   on_truncate;   /* relation truncated */
	pgl_smgr_unlink_hook_fn     on_unlink;     /* relation file deleted */
	pgl_smgr_checkpoint_hook_fn on_checkpoint;  /* checkpoint completed */
} pgl_smgr_hooks;

/*
 * Register SMgr hooks. Pass NULL for any field to skip that event.
 * Pass NULL for the entire struct to clear all hooks.
 */
extern void pgl_register_smgr_hooks(const pgl_smgr_hooks *hooks);

/*
 * Internal dispatch functions — called from smgr.c and checkpointer.c.
 * These accept PostgreSQL types internally and convert to pgl_relfilelocator.
 * Do NOT call from external code.
 */
extern void pgl_smgr_notify_write(const void *rlocator, int forknum,
                                   uint32_t blocknum, uint32_t nblocks);
extern void pgl_smgr_notify_extend(const void *rlocator, int forknum,
                                    uint32_t blocknum, uint32_t nblocks);
extern void pgl_smgr_notify_sync(const void *rlocator, int forknum);
extern void pgl_smgr_notify_truncate(const void *rlocator, int forknum,
                                      uint32_t old_blocks, uint32_t new_blocks);
extern void pgl_smgr_notify_unlink(const void *rlocator, int forknum);
extern void pgl_smgr_notify_checkpoint(void);

#ifdef __cplusplus
}
#endif

#endif /* PGL_SMGR_HOOKS_H */
