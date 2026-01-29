/*
 * pgl_io_hooks.c - I/O interception layer for PGLite mobile/daemon builds
 *
 * Default behavior: passthrough to real POSIX calls.
 * Call pgl_register_io_handlers() to intercept.
 */
#include "pgl_io_hooks.h"

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* Global hook function pointers (NULL = use real POSIX) */
static pgl_pread_hook_fn  g_pread_hook  = NULL;
static pgl_pwrite_hook_fn g_pwrite_hook = NULL;
static pgl_fsync_hook_fn  g_fsync_hook  = NULL;
static pgl_open_hook_fn   g_open_hook   = NULL;
static pgl_close_hook_fn  g_close_hook  = NULL;

void
pgl_register_io_handlers(const pgl_io_handlers *handlers)
{
	if (handlers == NULL)
	{
		g_pread_hook  = NULL;
		g_pwrite_hook = NULL;
		g_fsync_hook  = NULL;
		g_open_hook   = NULL;
		g_close_hook  = NULL;
		return;
	}

	g_pread_hook  = handlers->on_pread;
	g_pwrite_hook = handlers->on_pwrite;
	g_fsync_hook  = handlers->on_fsync;
	g_open_hook   = handlers->on_open;
	g_close_hook  = handlers->on_close;
}

ssize_t
pgl_hooked_pread(int fd, void *buf, size_t count, off_t offset)
{
	if (g_pread_hook)
		return g_pread_hook(fd, buf, count, offset);
	return pread(fd, buf, count, offset);
}

ssize_t
pgl_hooked_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
	if (g_pwrite_hook)
		return g_pwrite_hook(fd, buf, count, offset);
	return pwrite(fd, buf, count, offset);
}

int
pgl_hooked_fsync(int fd)
{
	if (g_fsync_hook)
		return g_fsync_hook(fd);
	return fsync(fd);
}

int
pgl_hooked_open(const char *path, int flags, int mode)
{
	if (g_open_hook)
		return g_open_hook(path, flags, mode);
	return open(path, flags, mode);
}

int
pgl_hooked_close(int fd)
{
	if (g_close_hook)
		return g_close_hook(fd);
	return close(fd);
}
