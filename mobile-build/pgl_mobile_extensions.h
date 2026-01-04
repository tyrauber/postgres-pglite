/*
 * pgl_mobile_extensions.h - Static extension registry for mobile platforms
 *
 * On mobile platforms (iOS/Android), we cannot use dlopen/dlsym to dynamically
 * load extensions. Instead, extensions are statically linked and registered here.
 */

#ifndef PGL_MOBILE_EXTENSIONS_H
#define PGL_MOBILE_EXTENSIONS_H

#include <stdbool.h>

/*
 * Lookup a symbol in the static extension registry.
 * Returns the function pointer if found, NULL otherwise.
 */
extern void *pgl_mobile_lookup_symbol(const char *symbol);

/*
 * Check if a library is a built-in extension that we handle statically.
 * Returns true if the library is built-in and should use the static registry.
 */
extern bool pgl_mobile_is_builtin_library(const char *libname);

#endif /* PGL_MOBILE_EXTENSIONS_H */
