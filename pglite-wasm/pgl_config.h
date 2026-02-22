#pragma once
#ifndef PGL_CONFIG_H
#define PGL_CONFIG_H

#include <stddef.h>  /* size_t */

/**
 * PGLite Unified Path Configuration
 *
 * This module provides explicit path configuration for PGLite, replacing
 * the fragmented environment variable cascades and argv0-based path resolution.
 *
 * Usage:
 *   PgliteConfig config = {
 *       .data_dir = "/storage/data",
 *       .share_dir = "/tmp/myapp/share",
 *       .validate_paths = true,
 *   };
 *
 *   PgliteValidation v = pgl_validate_config(&config);
 *   if (!v.valid) {
 *       fprintf(stderr, "Config error: %s\n", v.error);
 *       exit(1);
 *   }
 *
 *   pgl_init(&config);
 *   // Now pgl_initdb() and pgl_backend() use these paths
 */

#include <stdbool.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/**
 * Configuration structure for PGLite paths.
 * Set paths explicitly to avoid argv0/env var resolution issues.
 */
typedef struct PgliteConfig {
    /* ========== Required Paths ========== */

    /**
     * PGDATA - where the database cluster lives.
     * This directory will contain base/, global/, pg_wal/, etc.
     * Example: "/storage/data" or "/var/lib/pglite/data"
     */
    const char *data_dir;

    /**
     * Share directory - contains PostgreSQL runtime files.
     * Expected structure:
     *   {share_dir}/
     *   ├── postgresql/     <- postgres.bki, information_schema.sql, etc.
     *   ├── timezone/       <- timezone data files
     *   ├── timezonesets/   <- timezone abbreviation sets
     *   ├── extension/      <- .control and .sql files
     *   └── tsearch_data/   <- text search dictionaries
     *
     * Example: "/tmp/everydb/share" or "/opt/pglite/share"
     */
    const char *share_dir;

    /* ========== Optional Path Overrides ========== */
    /* If NULL, derived from share_dir */

    /**
     * Override for timezone data directory.
     * Default: {share_dir}/timezone
     */
    const char *timezone_dir;

    /**
     * Override for timezone abbreviation sets.
     * Default: {share_dir}/timezonesets
     */
    const char *timezonesets_dir;

    /**
     * Override for extension files (.control, .sql).
     * Default: {share_dir}/extension
     */
    const char *extension_dir;

    /**
     * Override for text search dictionaries.
     * Default: {share_dir}/tsearch_data
     */
    const char *tsearch_dir;

    /**
     * Override for locale data.
     * Default: {share_dir}/locale
     */
    const char *locale_dir;

    /**
     * PROJ data directory for PostGIS.
     * Should contain proj.db.
     * If NULL, PostGIS spatial ref lookups may fail.
     * Example: "/tmp/everydb/proj"
     */
    const char *proj_dir;

    /**
     * Password file for initdb.
     * If set, initdb will read the superuser password from this file.
     * Example: "/tmp/everydb/password"
     */
    const char *password_file;

    /**
     * Username for the database superuser.
     * Default: "postgres"
     */
    const char *username;

    /* ========== Behavior Flags ========== */

    /**
     * If true, pgl_init() will verify all paths exist before initialization.
     * Recommended for debugging and development.
     */
    bool validate_paths;

    /**
     * If true, error messages include detailed directory structure hints.
     * Recommended for debugging path issues.
     */
    bool verbose_errors;

} PgliteConfig;

/**
 * Validation result structure.
 * Contains detailed information about which paths were checked and any errors.
 */
typedef struct PgliteValidation {
    /** Overall validation result */
    bool valid;

    /** Human-readable error message if validation failed */
    char error[2048];

    /** Individual path check results */
    struct {
        bool data_dir_exists;
        bool data_dir_writable;
        bool share_dir_exists;
        bool postgres_bki_exists;
        bool timezone_exists;
        bool timezonesets_exists;
        bool extension_dir_exists;
        bool plpgsql_control_exists;  /* Required for PostGIS and many extensions */
        bool tsearch_data_exists;
        bool proj_db_exists;          /* Only checked if proj_dir is set */
        bool password_file_exists;    /* Only checked if password_file is set */
    } checks;

} PgliteValidation;

/* ========== API Functions ========== */

/**
 * Initialize PGLite with explicit path configuration.
 *
 * Call this ONCE at startup, BEFORE pgl_initdb() or pgl_backend().
 * After calling pgl_init(), all path resolution uses the provided config
 * instead of environment variables or argv0.
 *
 * @param config Configuration structure with paths and options.
 *               The structure is copied, so it can be stack-allocated.
 * @return 0 on success, -1 if already initialized or validation failed.
 */
int pgl_init(const PgliteConfig *config);

/**
 * Validate a configuration before initialization.
 *
 * Checks that all required paths exist and are accessible.
 * Call this before pgl_init() to get detailed error messages.
 *
 * @param config Configuration to validate.
 * @return Validation result with detailed check information.
 */
PgliteValidation pgl_validate_config(const PgliteConfig *config);

/**
 * Get the current configuration.
 *
 * Returns NULL if pgl_init() has not been called.
 * The returned pointer is valid for the lifetime of the process.
 *
 * @return Pointer to current config, or NULL if not initialized.
 */
const PgliteConfig* pgl_get_config(void);

/**
 * Check if PGLite has been initialized with pgl_init().
 *
 * @return true if pgl_init() has been called successfully.
 */
bool pgl_is_initialized(void);

/**
 * Get a resolved path from the configuration.
 *
 * This function handles derivation of optional paths from share_dir.
 * Used internally by path.c and exec.c.
 *
 * @param path_type One of: "share", "data", "timezone", "timezonesets",
 *                  "extension", "tsearch", "locale", "proj", "password"
 * @param out_path Buffer to write the resolved path.
 * @param out_size Size of the output buffer.
 * @return 0 on success, -1 if path_type is unknown or config not initialized.
 */
int pgl_get_path(const char *path_type, char *out_path, size_t out_size);

/* ========== Internal Functions (for path.c/exec.c integration) ========== */

/**
 * Get the share directory path, including /postgresql suffix for core files.
 * Used by initdb and backend for finding postgres.bki, etc.
 *
 * @param out_path Buffer to write the path.
 * @param out_size Size of the output buffer.
 * @return 0 on success, -1 if not configured.
 */
int pgl_get_share_path_internal(char *out_path, size_t out_size);

/**
 * Get the data directory path (PGDATA).
 *
 * @param out_path Buffer to write the path.
 * @param out_size Size of the output buffer.
 * @return 0 on success, -1 if not configured.
 */
int pgl_get_data_path_internal(char *out_path, size_t out_size);

/**
 * Get a synthetic argv0 for PostgreSQL's path resolution.
 * Returns {share_dir}/../bin/postgres or a reasonable equivalent.
 *
 * @param out_path Buffer to write the path.
 * @param out_size Size of the output buffer.
 * @return 0 on success, -1 if not configured.
 */
int pgl_get_argv0_internal(char *out_path, size_t out_size);

#endif /* PGL_CONFIG_H */
