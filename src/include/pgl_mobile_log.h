/*-------------------------------------------------------------------------
 *
 * pgl_mobile_log.h
 *    Cross-platform logging for PGLite mobile builds.
 *
 * VERBOSE LOGGING IS DISABLED BY DEFAULT for performance.
 * To enable, build with -DPGL_VERBOSE_LOGGING
 *
 * On iOS devices, fprintf(stderr, ...) can crash or go nowhere.
 * This header provides safe logging that works on:
 *   - iOS Simulator (uses os_log)
 *   - iOS Device (uses os_log)
 *   - Android (uses __android_log_print)
 *   - Other platforms (uses fprintf)
 *
 * Usage:
 *   PGL_LOG("message");
 *   PGL_LOG_FMT("value=%d", value);
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGL_MOBILE_LOG_H
#define PGL_MOBILE_LOG_H

/*
 * By default, PGL_LOG and PGL_LOG_FMT are no-ops for performance.
 * Define PGL_VERBOSE_LOGGING to enable verbose logging for debugging.
 */
#ifdef PGL_VERBOSE_LOGGING

#ifdef PGL_MOBILE

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include <os/log.h>

/* Use os_log on iOS - it's the only reliable way to log on device */
#define PGL_LOG(msg) os_log(OS_LOG_DEFAULT, "[PGLite] %{public}s", msg)
#define PGL_LOG_FMT(fmt, ...) os_log(OS_LOG_DEFAULT, "[PGLite] " fmt, ##__VA_ARGS__)

#else
/* macOS or other Apple platforms */
#include <stdio.h>
#define PGL_LOG(msg) do { fprintf(stderr, "[PGLite] %s\n", msg); fflush(stderr); } while(0)
#define PGL_LOG_FMT(fmt, ...) do { fprintf(stderr, "[PGLite] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#endif /* TARGET_OS_IOS */

#elif defined(__ANDROID__)
#include <android/log.h>
#define PGL_LOG(msg) __android_log_print(ANDROID_LOG_INFO, "PGLite", "%s", msg)
#define PGL_LOG_FMT(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "PGLite", fmt, ##__VA_ARGS__)

#else
/* Other mobile platforms - fallback to fprintf */
#include <stdio.h>
#define PGL_LOG(msg) do { fprintf(stderr, "[PGLite] %s\n", msg); fflush(stderr); } while(0)
#define PGL_LOG_FMT(fmt, ...) do { fprintf(stderr, "[PGLite] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#endif /* __APPLE__ */

#else /* !PGL_MOBILE */

/* Non-mobile builds - use standard fprintf */
#include <stdio.h>
#define PGL_LOG(msg) do { fprintf(stderr, "[PGLite] %s\n", msg); fflush(stderr); } while(0)
#define PGL_LOG_FMT(fmt, ...) do { fprintf(stderr, "[PGLite] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

#endif /* PGL_MOBILE */

#else /* !PGL_VERBOSE_LOGGING */

/* Default: logging disabled for performance */
#define PGL_LOG(msg) ((void)0)
#define PGL_LOG_FMT(fmt, ...) ((void)0)

#endif /* PGL_VERBOSE_LOGGING */

#endif /* PGL_MOBILE_LOG_H */
