/**
 * PGLite Unified Path Configuration - Implementation
 *
 * This module provides explicit path configuration for PGLite, replacing
 * the fragmented environment variable cascades and argv0-based path resolution.
 */

#include "pgl_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* ========== Global State ========== */

static PgliteConfig g_config = {0};
static bool g_config_initialized = false;

/* Derived paths (computed from share_dir if not explicitly set) */
static char g_share_postgresql_path[PATH_MAX];
static char g_timezone_path[PATH_MAX];
static char g_timezonesets_path[PATH_MAX];
static char g_extension_path[PATH_MAX];
static char g_tsearch_path[PATH_MAX];
static char g_locale_path[PATH_MAX];
static char g_argv0_path[PATH_MAX];

/* ========== Helper Functions ========== */

static bool dir_exists(const char *path) {
    if (!path || !*path) return false;
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static bool file_exists(const char *path) {
    if (!path || !*path) return false;
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

static bool dir_writable(const char *path) {
    if (!path || !*path) return false;
    return (access(path, W_OK) == 0);
}

static void safe_strcpy(char *dst, const char *src, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t src_len = strlen(src);
    if (src_len >= dst_size) {
        src_len = dst_size - 1;
    }
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
}

static void path_join(char *dst, size_t dst_size, const char *base, const char *suffix) {
    if (!dst || dst_size == 0) return;

    size_t base_len = base ? strlen(base) : 0;
    size_t suffix_len = suffix ? strlen(suffix) : 0;

    /* Remove trailing slash from base if present */
    while (base_len > 0 && base[base_len - 1] == '/') {
        base_len--;
    }

    /* Remove leading slash from suffix if present */
    while (suffix_len > 0 && suffix[0] == '/') {
        suffix++;
        suffix_len--;
    }

    /* Check if it fits */
    if (base_len + 1 + suffix_len >= dst_size) {
        /* Truncate - this shouldn't happen with PATH_MAX */
        dst[0] = '\0';
        return;
    }

    memcpy(dst, base, base_len);
    dst[base_len] = '/';
    memcpy(dst + base_len + 1, suffix, suffix_len);
    dst[base_len + 1 + suffix_len] = '\0';
}

/* ========== Validation ========== */

PgliteValidation pgl_validate_config(const PgliteConfig *config) {
    PgliteValidation v = {0};
    v.valid = true;

    if (!config) {
        v.valid = false;
        safe_strcpy(v.error, "Configuration is NULL", sizeof(v.error));
        return v;
    }

    /* Check required: data_dir */
    if (!config->data_dir || !*config->data_dir) {
        v.valid = false;
        safe_strcpy(v.error, "data_dir is required (where the database cluster lives)", sizeof(v.error));
        return v;
    }

    /* Check required: share_dir */
    if (!config->share_dir || !*config->share_dir) {
        v.valid = false;
        safe_strcpy(v.error, "share_dir is required (where PostgreSQL runtime files live)", sizeof(v.error));
        return v;
    }

    /* Check data_dir parent is writable (data_dir itself may not exist yet) */
    {
        char parent[PATH_MAX];
        safe_strcpy(parent, config->data_dir, sizeof(parent));
        char *last_slash = strrchr(parent, '/');
        if (last_slash && last_slash != parent) {
            *last_slash = '\0';
            v.checks.data_dir_writable = dir_writable(parent);
        } else {
            /* Root or current dir */
            v.checks.data_dir_writable = true;
        }
    }

    v.checks.data_dir_exists = dir_exists(config->data_dir);
    /* data_dir not existing is OK - initdb will create it */

    /* Check share_dir exists */
    v.checks.share_dir_exists = dir_exists(config->share_dir);
    if (!v.checks.share_dir_exists) {
        v.valid = false;
        if (config->verbose_errors) {
            snprintf(v.error, sizeof(v.error),
                "Share directory not found: %s\n\n"
                "This directory should contain PostgreSQL runtime files.\n"
                "Expected structure:\n"
                "  %s/\n"
                "  ├── postgresql/     <- Core PostgreSQL files\n"
                "  ├── timezone/       <- Timezone data\n"
                "  ├── timezonesets/   <- Timezone abbreviations\n"
                "  └── extension/      <- Extension .control files\n",
                config->share_dir, config->share_dir);
        } else {
            snprintf(v.error, sizeof(v.error),
                "Share directory not found: %s", config->share_dir);
        }
        return v;
    }

    /* Check postgres.bki exists (critical for initdb) */
    {
        char bki_path[PATH_MAX];
        char bki_gz_path[PATH_MAX];
        path_join(bki_path, sizeof(bki_path), config->share_dir, "postgresql/postgres.bki");
        path_join(bki_gz_path, sizeof(bki_gz_path), config->share_dir, "postgresql/postgres.bki.gz");

        v.checks.postgres_bki_exists = file_exists(bki_path) || file_exists(bki_gz_path);
        if (!v.checks.postgres_bki_exists) {
            v.valid = false;
            if (config->verbose_errors) {
                snprintf(v.error, sizeof(v.error),
                    "postgres.bki not found at: %s\n\n"
                    "This file is required for database initialization (initdb).\n"
                    "Expected directory structure:\n"
                    "  %s/\n"
                    "  ├── postgresql/\n"
                    "  │   └── postgres.bki  <- MISSING\n"
                    "  ├── timezone/\n"
                    "  └── extension/\n\n"
                    "Note: Both uncompressed (.bki) and gzipped (.bki.gz) are supported.",
                    bki_path, config->share_dir);
            } else {
                snprintf(v.error, sizeof(v.error),
                    "postgres.bki not found at: %s/postgresql/postgres.bki", config->share_dir);
            }
            return v;
        }
    }

    /* Check timezone directory */
    {
        char tz_path[PATH_MAX];
        const char *tz_dir = config->timezone_dir;
        if (!tz_dir || !*tz_dir) {
            path_join(tz_path, sizeof(tz_path), config->share_dir, "timezone");
            tz_dir = tz_path;
        }
        v.checks.timezone_exists = dir_exists(tz_dir);
        /* Not fatal if missing - some minimal installs may omit it */
    }

    /* Check timezonesets directory */
    {
        char tzsets_path[PATH_MAX];
        const char *tzsets_dir = config->timezonesets_dir;
        if (!tzsets_dir || !*tzsets_dir) {
            path_join(tzsets_path, sizeof(tzsets_path), config->share_dir, "timezonesets");
            tzsets_dir = tzsets_path;
        }
        v.checks.timezonesets_exists = dir_exists(tzsets_dir);
    }

    /* Check extension directory */
    {
        char ext_path[PATH_MAX];
        const char *ext_dir = config->extension_dir;
        if (!ext_dir || !*ext_dir) {
            path_join(ext_path, sizeof(ext_path), config->share_dir, "extension");
            ext_dir = ext_path;
        }
        v.checks.extension_dir_exists = dir_exists(ext_dir);

        /* Check plpgsql.control (required for PostGIS and many extensions) */
        if (v.checks.extension_dir_exists) {
            char plpgsql_path[PATH_MAX];
            path_join(plpgsql_path, sizeof(plpgsql_path), ext_dir, "plpgsql.control");
            v.checks.plpgsql_control_exists = file_exists(plpgsql_path);
        }
    }

    /* Check tsearch_data directory */
    {
        char ts_path[PATH_MAX];
        const char *ts_dir = config->tsearch_dir;
        if (!ts_dir || !*ts_dir) {
            path_join(ts_path, sizeof(ts_path), config->share_dir, "tsearch_data");
            ts_dir = ts_path;
        }
        v.checks.tsearch_data_exists = dir_exists(ts_dir);
    }

    /* Check proj.db if proj_dir is set (PostGIS) */
    if (config->proj_dir && *config->proj_dir) {
        char proj_db_path[PATH_MAX];
        path_join(proj_db_path, sizeof(proj_db_path), config->proj_dir, "proj.db");
        v.checks.proj_db_exists = file_exists(proj_db_path);
        if (!v.checks.proj_db_exists) {
            v.valid = false;
            if (config->verbose_errors) {
                snprintf(v.error, sizeof(v.error),
                    "proj.db not found at: %s\n\n"
                    "This file is required for PostGIS spatial reference lookups.\n"
                    "Expected: %s/proj.db\n\n"
                    "If you're not using PostGIS, set proj_dir to NULL.",
                    proj_db_path, config->proj_dir);
            } else {
                snprintf(v.error, sizeof(v.error),
                    "proj.db not found at: %s/proj.db", config->proj_dir);
            }
            return v;
        }
    }

    /* Check password file if set */
    if (config->password_file && *config->password_file) {
        v.checks.password_file_exists = file_exists(config->password_file);
        if (!v.checks.password_file_exists) {
            v.valid = false;
            snprintf(v.error, sizeof(v.error),
                "Password file not found: %s", config->password_file);
            return v;
        }
    }

    return v;
}

/* ========== Initialization ========== */

int pgl_init(const PgliteConfig *config) {
    if (g_config_initialized) {
        fprintf(stderr, "[pgl_init] Error: Already initialized. Call pgl_init() only once.\n");
        return -1;
    }

    if (!config) {
        fprintf(stderr, "[pgl_init] Error: Configuration is NULL.\n");
        return -1;
    }

    /* Validate if requested */
    if (config->validate_paths) {
        PgliteValidation v = pgl_validate_config(config);
        if (!v.valid) {
            fprintf(stderr, "[pgl_init] Validation failed: %s\n", v.error);
            return -1;
        }
    }

    /* Copy config */
    memset(&g_config, 0, sizeof(g_config));

    /* Copy string pointers - we assume they're static or long-lived */
    g_config.data_dir = config->data_dir;
    g_config.share_dir = config->share_dir;
    g_config.timezone_dir = config->timezone_dir;
    g_config.timezonesets_dir = config->timezonesets_dir;
    g_config.extension_dir = config->extension_dir;
    g_config.tsearch_dir = config->tsearch_dir;
    g_config.locale_dir = config->locale_dir;
    g_config.proj_dir = config->proj_dir;
    g_config.password_file = config->password_file;
    g_config.username = config->username;
    g_config.validate_paths = config->validate_paths;
    g_config.verbose_errors = config->verbose_errors;

    /* Derive paths that weren't explicitly set */

    /* share/postgresql - core PostgreSQL files */
    path_join(g_share_postgresql_path, sizeof(g_share_postgresql_path),
              config->share_dir, "postgresql");

    /* timezone */
    if (config->timezone_dir && *config->timezone_dir) {
        safe_strcpy(g_timezone_path, config->timezone_dir, sizeof(g_timezone_path));
    } else {
        path_join(g_timezone_path, sizeof(g_timezone_path), config->share_dir, "timezone");
    }

    /* timezonesets */
    if (config->timezonesets_dir && *config->timezonesets_dir) {
        safe_strcpy(g_timezonesets_path, config->timezonesets_dir, sizeof(g_timezonesets_path));
    } else {
        path_join(g_timezonesets_path, sizeof(g_timezonesets_path), config->share_dir, "timezonesets");
    }

    /* extension */
    if (config->extension_dir && *config->extension_dir) {
        safe_strcpy(g_extension_path, config->extension_dir, sizeof(g_extension_path));
    } else {
        path_join(g_extension_path, sizeof(g_extension_path), config->share_dir, "extension");
    }

    /* tsearch_data */
    if (config->tsearch_dir && *config->tsearch_dir) {
        safe_strcpy(g_tsearch_path, config->tsearch_dir, sizeof(g_tsearch_path));
    } else {
        path_join(g_tsearch_path, sizeof(g_tsearch_path), config->share_dir, "tsearch_data");
    }

    /* locale */
    if (config->locale_dir && *config->locale_dir) {
        safe_strcpy(g_locale_path, config->locale_dir, sizeof(g_locale_path));
    } else {
        path_join(g_locale_path, sizeof(g_locale_path), config->share_dir, "locale");
    }

    /* Synthetic argv0 for PostgreSQL's internal path resolution.
     * We create a path like {share_dir}/../bin/postgres so that
     * PostgreSQL's relative path resolution finds our share files.
     *
     * Given share_dir = /tmp/everydb/share
     * argv0 = /tmp/everydb/bin/postgres
     * PostgreSQL resolves: {argv0}/../share = /tmp/everydb/share ✓
     */
    {
        /* Find parent of share_dir */
        char parent[PATH_MAX];
        safe_strcpy(parent, config->share_dir, sizeof(parent));
        char *last_slash = strrchr(parent, '/');
        if (last_slash && last_slash != parent) {
            *last_slash = '\0';
            path_join(g_argv0_path, sizeof(g_argv0_path), parent, "bin/postgres");
        } else {
            /* Fallback */
            safe_strcpy(g_argv0_path, "/tmp/pglite/bin/postgres", sizeof(g_argv0_path));
        }
    }

    /* Set environment variables for backwards compatibility */
    if (config->data_dir) {
        setenv("PGDATA", config->data_dir, 1);
    }
    if (config->username) {
        setenv("PGUSER", config->username, 1);
    }

    /* Set PROJ_DATA for PostGIS if configured */
    if (config->proj_dir && *config->proj_dir) {
        setenv("PROJ_DATA", config->proj_dir, 1);
        setenv("PROJ_LIB", config->proj_dir, 1);  /* Some versions use PROJ_LIB */
    }

    g_config_initialized = true;

    fprintf(stderr, "[pgl_init] Initialized with:\n");
    fprintf(stderr, "  data_dir:  %s\n", config->data_dir ? config->data_dir : "(null)");
    fprintf(stderr, "  share_dir: %s\n", config->share_dir ? config->share_dir : "(null)");
    fprintf(stderr, "  proj_dir:  %s\n", config->proj_dir ? config->proj_dir : "(null)");
    fprintf(stderr, "  argv0:     %s\n", g_argv0_path);

    return 0;
}

/* ========== Accessors ========== */

const PgliteConfig* pgl_get_config(void) {
    if (!g_config_initialized) {
        return NULL;
    }
    return &g_config;
}

bool pgl_is_initialized(void) {
    return g_config_initialized;
}

int pgl_get_path(const char *path_type, char *out_path, size_t out_size) {
    if (!g_config_initialized || !out_path || out_size == 0) {
        return -1;
    }

    if (strcmp(path_type, "share") == 0) {
        safe_strcpy(out_path, g_share_postgresql_path, out_size);
    } else if (strcmp(path_type, "data") == 0) {
        safe_strcpy(out_path, g_config.data_dir, out_size);
    } else if (strcmp(path_type, "timezone") == 0) {
        safe_strcpy(out_path, g_timezone_path, out_size);
    } else if (strcmp(path_type, "timezonesets") == 0) {
        safe_strcpy(out_path, g_timezonesets_path, out_size);
    } else if (strcmp(path_type, "extension") == 0) {
        safe_strcpy(out_path, g_extension_path, out_size);
    } else if (strcmp(path_type, "tsearch") == 0) {
        safe_strcpy(out_path, g_tsearch_path, out_size);
    } else if (strcmp(path_type, "locale") == 0) {
        safe_strcpy(out_path, g_locale_path, out_size);
    } else if (strcmp(path_type, "proj") == 0) {
        if (g_config.proj_dir) {
            safe_strcpy(out_path, g_config.proj_dir, out_size);
        } else {
            out_path[0] = '\0';
        }
    } else if (strcmp(path_type, "password") == 0) {
        if (g_config.password_file) {
            safe_strcpy(out_path, g_config.password_file, out_size);
        } else {
            out_path[0] = '\0';
        }
    } else if (strcmp(path_type, "argv0") == 0) {
        safe_strcpy(out_path, g_argv0_path, out_size);
    } else {
        return -1;  /* Unknown path type */
    }

    return 0;
}

/* ========== Internal Functions for path.c/exec.c ========== */

int pgl_get_share_path_internal(char *out_path, size_t out_size) {
    if (!g_config_initialized) {
        return -1;
    }
    safe_strcpy(out_path, g_share_postgresql_path, out_size);
    return 0;
}

int pgl_get_data_path_internal(char *out_path, size_t out_size) {
    if (!g_config_initialized || !g_config.data_dir) {
        return -1;
    }
    safe_strcpy(out_path, g_config.data_dir, out_size);
    return 0;
}

int pgl_get_argv0_internal(char *out_path, size_t out_size) {
    if (!g_config_initialized) {
        return -1;
    }
    safe_strcpy(out_path, g_argv0_path, out_size);
    return 0;
}
