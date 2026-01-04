/*
 * pgl_mobile_stubs.c - Weak stub implementations for mobile extension support
 *
 * These stubs allow the PostgreSQL build to succeed. They will be overridden
 * by the actual implementations in pgl_mobile_extensions.c when the final
 * library is linked.
 *
 * Only compiled when PGL_MOBILE is defined.
 */

#ifdef PGL_MOBILE

#include <stdbool.h>
#include <stddef.h>

/*
 * Weak symbol: pgl_mobile_lookup_symbol
 * This stub returns NULL (symbol not found).
 * The real implementation is in mobile-build/pgl_mobile_extensions.c
 */
__attribute__((weak)) void *
pgl_mobile_lookup_symbol(const char *symbol)
{
  return NULL;
}

/*
 * Weak symbol: pgl_mobile_is_builtin_library
 * This stub returns false (not a builtin library).
 * The real implementation is in mobile-build/pgl_mobile_extensions.c
 */
__attribute__((weak)) bool
pgl_mobile_is_builtin_library(const char *libname)
{
  return false;
}

#endif /* PGL_MOBILE */
