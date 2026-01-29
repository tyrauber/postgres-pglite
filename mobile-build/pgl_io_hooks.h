/*
 * pgl_io_hooks.h - I/O interception layer for PGLite mobile/daemon builds
 *
 * Allows external code (e.g. Go via cgo) to intercept all file I/O
 * by registering callback functions. When no hooks are registered,
 * all calls pass through to real POSIX functions.
 */
#ifndef PGL_IO_HOOKS_H
#define PGL_IO_HOOKS_H

#include <sys/types.h>
#include <stddef.h>

/* Hook function pointer types */
typedef ssize_t (*pgl_pread_hook_fn)(int fd, void *buf, size_t count, off_t offset);
typedef ssize_t (*pgl_pwrite_hook_fn)(int fd, const void *buf, size_t count, off_t offset);
typedef int (*pgl_fsync_hook_fn)(int fd);
typedef int (*pgl_open_hook_fn)(const char *path, int flags, int mode);
typedef int (*pgl_close_hook_fn)(int fd);

/* Hook registration struct */
typedef struct pgl_io_handlers {
	pgl_pread_hook_fn  on_pread;
	pgl_pwrite_hook_fn on_pwrite;
	pgl_fsync_hook_fn  on_fsync;
	pgl_open_hook_fn   on_open;
	pgl_close_hook_fn  on_close;
} pgl_io_handlers;

/*
 * Register I/O handlers. Pass NULL for any field to use default POSIX.
 * Pass NULL for the entire struct to clear all hooks.
 */
extern void pgl_register_io_handlers(const pgl_io_handlers *handlers);

/* Hooked replacement functions (called via macro redirect) */
extern ssize_t pgl_hooked_pread(int fd, void *buf, size_t count, off_t offset);
extern ssize_t pgl_hooked_pwrite(int fd, const void *buf, size_t count, off_t offset);
extern int pgl_hooked_fsync(int fd);
extern int pgl_hooked_open(const char *path, int flags, int mode);
extern int pgl_hooked_close(int fd);

#endif /* PGL_IO_HOOKS_H */
