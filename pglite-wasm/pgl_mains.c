#include <setjmp.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>

// ============================================================================
// Debug logging control - disabled by default for performance
// Build with -DPGL_VERBOSE_LOGGING to enable all debug output
// ============================================================================
#ifndef PGL_VERBOSE_LOGGING
#define PGL_DEBUG_PRINT(...) ((void)0)
#define PGL_DEBUG_FLUSH(s) ((void)0)
#define PGL_DEBUG_FPUTC(c, s) ((void)0)
#else
#define PGL_DEBUG_PRINT(...) fprintf(__VA_ARGS__)
#define PGL_DEBUG_FLUSH(s) fflush(s)
#define PGL_DEBUG_FPUTC(c, s) fputc(c, s)
#endif

volatile int sf_connected = 0;
FILE * single_mode_feed = NULL;
volatile bool inloop = false;
volatile sigjmp_buf local_sigjmp_buf;
bool repl = false;

// ============================================================================
// WAL/Recovery Debug Logging
// ============================================================================
#ifdef PGL_MOBILE
static void debug_log_wal_state(const char* phase) {
    const char* pgdata = getenv("PGDATA");
    if (!pgdata || !*pgdata) {
        PGL_LOG_INFO("[WAL_DEBUG] %s: PGDATA not set", phase);
        return;
    }
    
    PGL_LOG_INFO("[WAL_DEBUG] === %s WAL State ===", phase);
    
    // Check pg_wal directory
    char wal_dir[1024];
    snprintf(wal_dir, sizeof(wal_dir), "%s/pg_wal", pgdata);
    struct stat st;
    if (stat(wal_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        PGL_LOG_INFO("[WAL_DEBUG] pg_wal exists at: %s", wal_dir);
        
        // List WAL files
        DIR* dir = opendir(wal_dir);
        if (dir) {
            struct dirent* entry;
            int wal_count = 0;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                char file_path[1024];
                snprintf(file_path, sizeof(file_path), "%s/%s", wal_dir, entry->d_name);
                struct stat fst;
                if (stat(file_path, &fst) == 0) {
                    PGL_LOG_INFO("[WAL_DEBUG]   WAL: %s (%lld bytes)", entry->d_name, (long long)fst.st_size);
                    wal_count++;
                }
            }
            closedir(dir);
            PGL_LOG_INFO("[WAL_DEBUG] Total WAL files: %d", wal_count);
        }
    } else {
        PGL_LOG_INFO("[WAL_DEBUG] pg_wal does NOT exist (first run or clean state)");
    }
    
    // Check pg_control
    char ctrl_path[1024];
    snprintf(ctrl_path, sizeof(ctrl_path), "%s/global/pg_control", pgdata);
    if (stat(ctrl_path, &st) == 0) {
        PGL_LOG_INFO("[WAL_DEBUG] pg_control: %lld bytes", (long long)st.st_size);
    } else {
        PGL_LOG_INFO("[WAL_DEBUG] pg_control: MISSING (errno=%d)", errno);
    }
    
    // Check recovery signals
    char recovery_signal[1024];
    snprintf(recovery_signal, sizeof(recovery_signal), "%s/recovery.signal", pgdata);
    if (stat(recovery_signal, &st) == 0) {
        PGL_LOG_INFO("[WAL_DEBUG] WARNING: recovery.signal EXISTS - in recovery mode!");
    }
    
    char standby_signal[1024];
    snprintf(standby_signal, sizeof(standby_signal), "%s/standby.signal", pgdata);
    if (stat(standby_signal, &st) == 0) {
        PGL_LOG_INFO("[WAL_DEBUG] WARNING: standby.signal EXISTS - in standby mode!");
    }
    
    PGL_LOG_INFO("[WAL_DEBUG] === End %s WAL State ===", phase);
}

static void debug_log_file_state(const char* path, const char* description) {
    struct stat st;
    if (stat(path, &st) == 0) {
        PGL_LOG_INFO("[FILE_DEBUG] %s: %s (%lld bytes)", description, path, (long long)st.st_size);
        if (st.st_size == 0) {
            PGL_LOG_ERROR("[FILE_DEBUG] WARNING: %s is EMPTY!", description);
        }
    } else {
        PGL_LOG_ERROR("[FILE_DEBUG] %s: %s MISSING (errno=%d)", description, path, errno);
    }
}
#endif

__attribute__((export_name("pgl_shutdown")))
void
pg_shutdown() {
    PDEBUG("# 11:" __FILE__": pg_shutdown");
    proc_exit(66);
}

__attribute__((export_name("pgl_closed")))
int
pgl_closed() {
    if (sf_connected>0)
        return 1;
    return 0;
}

#if FIXME
extern bool startswith(const char *str, const char *prefix);
#endif

void
interactive_file() {
	int			firstchar = 0;
	int			c = 0;				/* character read from getc() */
	StringInfoData input_message;
	StringInfoData *inBuf;
    FILE *stream ;
    int sql_line=1;
    bool sql_skip = false;
	/*
	 * At top of loop, reset extended-query-message flag, so that any
	 * errors encountered in "idle" state don't provoke skip.
	 */
	doing_extended_query_message = false;

	/*
	 * Release storage left over from prior query cycle, and create a new
	 * query input buffer in the cleared MessageContext.
	 */
	MemoryContextSwitchTo(MessageContext);
	MemoryContextResetAndDeleteChildren(MessageContext);

	initStringInfo(&input_message);
    inBuf = &input_message;
	DoingCommandRead = true;

    stream = single_mode_feed;

    while (c!=EOF) {
    	resetStringInfo(inBuf);
	    while ((c = getc(stream)) != EOF) {
		    if (c == '\n')
		    {
                sql_line++;
			    if (UseSemiNewlineNewline)
			    {
				    /*
				        * In -j mode, semicolon followed by two newlines ends the
				        * command; otherwise treat newline as regular character.
				        */
				    if (inBuf->len > 1 &&
					    inBuf->data[inBuf->len - 1] == '\n' &&
					    inBuf->data[inBuf->len - 2] == ';')
				    {
					    /* might as well drop the second newline */
					    break;
				    }
			    }
			    else
			    {
				    /*
				        * In plain mode, newline ends the command unless preceded by
				        * backslash.
				        */
				    if (inBuf->len > 0 &&
					    inBuf->data[inBuf->len - 1] == '\\')
				    {
					    /* discard backslash from inBuf */
					    inBuf->data[--inBuf->len] = '\0';
					    /* discard newline too */
					    continue;
				    }
				    else
				    {
					    /* keep the newline character, but end the command */
					    appendStringInfoChar(inBuf, '\n');
					    break;
				    }
			    }
		    }

		    /* Not newline, or newline treated as regular character */
		    appendStringInfoChar(inBuf, (char) c);
        }


        if (c == EOF && inBuf->len == 0)
            return;

        /* Add '\0' to make it look the same as message case. */
        appendStringInfoChar(inBuf, (char) '\0');
        firstchar = 'Q';
#if FIXME
#warning "FIXME: REVOKE ALL ON pg_largeobject FROM PUBLIC;"
#warning "FIXME: REVOKE CREATE,TEMPORARY ON DATABASE template1 FROM public;"
    sql_skip |= startswith(inBuf->data , "REVOKE ALL ON pg_largeobject FROM PUBLIC;");
    sql_skip |= startswith(inBuf->data , "REVOKE CREATE,TEMPORARY ON DATABASE template1 FROM public;");
    if (sql_skip) {
        fprintf(stdout, "# 106: SKIPPED: %d: %s\n", sql_line, inBuf->data);
        sql_skip = false;
        continue;
    } else {
        // fprintf(stderr, "%d: %s\n", sql_line, inBuf->data);
    }
#endif
// ???
        if (ignore_till_sync && firstchar != EOF)
            continue;

        #include "pg_proto.c"
    }
    PDEBUG("# 115: interactive_file: end");
}

void
RePostgresSingleUserMain(int single_argc, char *single_argv[], const char *username)
{
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R001: ENTRY username=%s\n", username ? username : "NULL");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[RePostgresSingleUserMain] ENTRY - username=%s", username ? username : "NULL");
    debug_log_wal_state("PRE-SINGLE-USER");
#endif
#if PGDEBUG
printf("# 123: RePostgresSingleUserMain progname=%s for %s feed=%s\n", progname, single_argv[0], IDB_PIPE_SINGLE);
#endif
    // On mobile, the single-user script is emitted under PGDATA by pgl_popen (pgl_os.h)
    char idb_single_path[1024];
#ifdef PGL_MOBILE
    extern void pgl_get_pipe_path(int stage, char *out, size_t outsz);
    pgl_get_pipe_path(1, idb_single_path, sizeof(idb_single_path));
#else
    snprintf(idb_single_path, sizeof(idb_single_path), "%s/initdb.single.txt", PREFIX ? (const char*)PREFIX : WASM_PREFIX);
#endif
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R002: Looking for single script at: %s\n", idb_single_path);
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    debug_log_file_state(idb_single_path, "initdb.single.txt");
#endif
    single_mode_feed = fopen(idb_single_path, "r");
    if (!single_mode_feed) {
        PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R003: No single script found (errno=%d), returning early\n", errno);
        PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
        PGL_LOG_INFO("[RePostgresSingleUserMain] No single-user script found, skipping replay");
#endif
        return; // nothing to replay; continue to backend startup
    }
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R004: Opened single script successfully\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[RePostgresSingleUserMain] Opened single-user script: %s", idb_single_path);
#endif

    // should be template1.
    const char *dbname = NULL;

    // Reset getopt() state to ensure proper parsing after prior BootstrapModeMain
    optind = 1; opterr = 1; optopt = 0; optarg = NULL;

    // Log argv for diagnosis
#ifdef PGL_VERBOSE_LOGGING
    fprintf(stderr, "[pgl_single] argc=%d argv:", single_argc);
    for (int i = 0; i < single_argc; i++) {
        fprintf(stderr, " %s", single_argv[i]);
    }
    fputc('\n', stderr);
#endif

    /* Parse command-line options. */
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R005: Calling process_postgres_switches()\n");
    PGL_DEBUG_FLUSH(stderr);
    process_postgres_switches(single_argc, single_argv, PGC_POSTMASTER, &dbname);
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R006: process_postgres_switches() done, dbname=%s\n", dbname ? dbname : "NULL");
    PGL_DEBUG_FLUSH(stderr);
#if PGDEBUG
printf("# 134: dbname=%s\n", dbname);
#endif
    /* Log DataDir and control file presence before reading it */
    {
        char ctrl_path[1024];
        snprintf(ctrl_path, sizeof(ctrl_path), "%s/global/pg_control", DataDir);
        struct stat st; int rc = stat(ctrl_path, &st);
        PGL_DEBUG_PRINT(stderr, "[pgl_single] DataDir=%s PGDATA(env)=%s ctrl=%s rc=%d errno=%d size=%lld\n",
                DataDir, getenv("PGDATA"), ctrl_path, rc, errno, (long long)((rc==0)?st.st_size:0));
    }
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R007: Calling LocalProcessControlFile()\n");
    PGL_DEBUG_FLUSH(stderr);
    LocalProcessControlFile(false);
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R008: LocalProcessControlFile() done\n");
    PGL_DEBUG_FLUSH(stderr);

    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R009: Calling process_shared_preload_libraries()\n");
    PGL_DEBUG_FLUSH(stderr);
    process_shared_preload_libraries();
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R010: process_shared_preload_libraries() done\n");
    PGL_DEBUG_FLUSH(stderr);

    /* Initialize MaxBackends - required for shared memory sizing */
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R011: Calling InitializeMaxBackends()\n");
    PGL_DEBUG_FLUSH(stderr);
    InitializeMaxBackends();
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R012: InitializeMaxBackends() done\n");
    PGL_DEBUG_FLUSH(stderr);

// ? IgnoreSystemIndexes = true;
IgnoreSystemIndexes = false;
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R013: Calling process_shmem_requests()\n");
    PGL_DEBUG_FLUSH(stderr);
    process_shmem_requests();
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R014: process_shmem_requests() done\n");
    PGL_DEBUG_FLUSH(stderr);

    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R015: Calling InitializeShmemGUCs()\n");
    PGL_DEBUG_FLUSH(stderr);
    InitializeShmemGUCs();
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R016: InitializeShmemGUCs() done\n");
    PGL_DEBUG_FLUSH(stderr);

    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R017: Calling InitializeWalConsistencyChecking()\n");
    PGL_DEBUG_FLUSH(stderr);
    InitializeWalConsistencyChecking();
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R018: InitializeWalConsistencyChecking() done\n");
    PGL_DEBUG_FLUSH(stderr);

    /* CRITICAL: Initialize shared memory and semaphores.
     * This was missing and caused XLogCtl to be NULL, leading to crashes
     * in RecoveryInProgress() when accessing XLogCtl->SharedRecoveryState.
     * See: docs/issues/pglite-currentresourceowner-crash.md
     */
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R019: Calling CreateSharedMemoryAndSemaphores()\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[RePostgresSingleUserMain] About to CreateSharedMemoryAndSemaphores()");
#endif
    CreateSharedMemoryAndSemaphores();
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R020: CreateSharedMemoryAndSemaphores() done\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[RePostgresSingleUserMain] CreateSharedMemoryAndSemaphores() completed");
#endif

    PgStartTime = GetCurrentTimestamp();
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R021: PgStartTime set\n");
    PGL_DEBUG_FLUSH(stderr);

    /*
     * Create a per-backend PGPROC struct in shared memory. We must do this
     * before we can use LWLocks.
     */
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R022: Calling InitProcess()\n");
    PGL_DEBUG_FLUSH(stderr);
    InitProcess();
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R023: InitProcess() done\n");
    PGL_DEBUG_FLUSH(stderr);

    SetProcessingMode(InitProcessing);
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R024: SetProcessingMode done\n");
    PGL_DEBUG_FLUSH(stderr);

    /* Early initialization */
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R025: Calling BaseInit()\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[RePostgresSingleUserMain] About to call BaseInit()");
#endif
    BaseInit();
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R026: BaseInit() done\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[RePostgresSingleUserMain] BaseInit() completed - WAL recovery may have occurred");
    debug_log_wal_state("POST-BASEINIT");
#endif
PDEBUG("# 153: Re-InitPostgres");
if (am_walsender)
    PDEBUG("# 155: am_walsender == true");
//      BaseInit();

#ifdef PGL_MOBILE
    /*
     * CRITICAL FIX: Set up PG_exception_stack BEFORE InitPostgres().
     * 
     * InitPostgres() calls StartupXLOG() which may perform WAL recovery.
     * During WAL recovery, if any error occurs (e.g., mdwritev fails due to
     * file I/O issues), PostgreSQL calls ereport(ERROR, ...) which uses
     * PG_exception_stack to longjmp to an error handler.
     * 
     * Without this handler, PG_exception_stack is NULL and the error
     * causes a crash (EXC_BREAKPOINT/SIGTRAP on iOS).
     * 
     * The pgl_sjlj.c include (which normally sets up PG_exception_stack)
     * happens AFTER InitPostgres(), which is too late for errors during
     * WAL recovery.
     * 
     * See: iOS crash in mdwritev.cold.1 during PerformWalRecovery
     */
    {
        sigjmp_buf init_exception_buf;
        
        PGL_LOG_INFO("[RePostgresSingleUserMain] Setting up PG_exception_stack for InitPostgres");
        
        if (sigsetjmp(init_exception_buf, 1) != 0)
        {
            /* Error occurred during InitPostgres - we longjmp'd here */
            PG_exception_stack = NULL;
            error_context_stack = NULL;
            
            PGL_LOG_ERROR("[RePostgresSingleUserMain] ERROR during InitPostgres/WAL recovery!");
            PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] InitPostgres failed - error caught by exception handler\n");
            
            /* Try to clean up and report the error */
            HOLD_INTERRUPTS();
            EmitErrorReport();
            FlushErrorState();
            RESUME_INTERRUPTS();
            
            /* Cannot continue - InitPostgres failed */
            PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] FATAL: Cannot continue after InitPostgres failure\n");
            return;
        }
        
        /* Set up exception stack before InitPostgres */
        PG_exception_stack = &init_exception_buf;
        
        InitPostgres(dbname, InvalidOid,	/* database to connect to */
                     username, InvalidOid,	/* role to connect as */
                     (!am_walsender) ? INIT_PG_LOAD_SESSION_LIBS : 0,
                     NULL);			/* no out_dbname */
        
        /* Clear exception stack - will be reset by pgl_sjlj.c later */
        PG_exception_stack = NULL;
        
        PGL_LOG_INFO("[RePostgresSingleUserMain] InitPostgres completed successfully");
    }
#else
    InitPostgres(dbname, InvalidOid,	/* database to connect to */
                 username, InvalidOid,	/* role to connect as */
                 (!am_walsender) ? INIT_PG_LOAD_SESSION_LIBS : 0,
                 NULL);			/* no out_dbname */
#endif

PDEBUG("# 164:" __FILE__);

    SetProcessingMode(NormalProcessing);

    BeginReportingGUCOptions();

    if (IsUnderPostmaster && Log_disconnections)
        on_proc_exit(log_disconnections, 0);

    pgstat_report_connect(MyDatabaseId);

    /* Perform initialization specific to a WAL sender process. */
    if (am_walsender)
        InitWalSender();

#if PGDEBUG
    whereToSendOutput = DestDebug;
#endif

    if (whereToSendOutput == DestDebug)
        printf("\nPostgreSQL stand-alone backend %s\n", PG_VERSION);

    /*
     * Create the memory context we will use in the main loop.
     *
     * MessageContext is reset once per iteration of the main loop, ie, upon
     * completion of processing of each command message from the client.
     */
    MessageContext = AllocSetContextCreate(TopMemoryContext,
						                   "MessageContext",
						                   ALLOCSET_DEFAULT_SIZES);

    /*
     * Create memory context and buffer used for RowDescription messages. As
     * SendRowDescriptionMessage(), via exec_describe_statement_message(), is
     * frequently executed for ever single statement, we don't want to
     * allocate a separate buffer every time.
     */
    row_description_context = AllocSetContextCreate(TopMemoryContext,
									                "RowDescriptionContext",
									                ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(row_description_context);
    initStringInfo(&row_description_buf);
    MemoryContextSwitchTo(TopMemoryContext);

#if defined(__wasi__)
    puts("# 210: sjlj exception handler off in initdb-wasi");
#else
#   define INITDB_SINGLE
#   include "pgl_sjlj.c"
#   undef INITDB_SINGLE
#endif // sjlj

    if (!ignore_till_sync)
        send_ready_for_query = true;	/* initially, or after error */
/*
    if (!inloop) {
        inloop = true;
        PDEBUG("# 335: REPL(initdb-single):Begin " __FILE__ );

        while (repl) { interactive_file(); }
    } else {
        // signal error
        optind = -1;
    }
*/

    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R027: Calling interactive_file()\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[RePostgresSingleUserMain] About to call interactive_file() for single-user replay");
#endif
  interactive_file();
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R028: interactive_file() done\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[RePostgresSingleUserMain] interactive_file() completed");
    debug_log_wal_state("POST-INTERACTIVE-FILE");
#endif
  fclose(single_mode_feed);
  single_mode_feed = NULL;
    PGL_DEBUG_PRINT(stderr, "[RePostgresSingleUserMain] R029: EXIT - single-user replay complete\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[RePostgresSingleUserMain] EXIT - single-user replay complete");
#endif

/*
    while (repl) { interactive_file(); }
    PDEBUG("# 240: REPL:End Raising a 'RuntimeError Exception' to halt program NOW");
    {
        void (*npe)() = NULL;
        npe();
    }
    // unreachable.
*/

    PDEBUG("# 248: no line-repl requested, exiting and keeping runtime alive");
}




void
AsyncPostgresSingleUserMain(int argc, char *argv[], const char *username, int async_restart)
{
	const char *dbname = NULL;
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A001: ENTRY username=%s async_restart=%d\n",
            username ? username : "NULL", async_restart);
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[AsyncPostgresSingleUserMain] ENTRY - username=%s async_restart=%d",
                 username ? username : "NULL", async_restart);
    debug_log_wal_state("PRE-ASYNC-SINGLE-USER");
#endif
PDEBUG("# 254:"__FILE__);

// if (!async_restart)	/* Initialize startup process environment. */
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A002: Calling InitStandaloneProcess()\n");
    PGL_DEBUG_FLUSH(stderr);
	InitStandaloneProcess(argv[0]);
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A003: InitStandaloneProcess() done\n");
    PGL_DEBUG_FLUSH(stderr);
PDEBUG("# 254:"__FILE__);
// if (!async_restart) /* Set default values for command-line options.	 */
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A004: Calling InitializeGUCOptions()\n");
    PGL_DEBUG_FLUSH(stderr);
	InitializeGUCOptions();
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A005: InitializeGUCOptions() done\n");
    PGL_DEBUG_FLUSH(stderr);
PDEBUG("# 257:"__FILE__);
// if (!async_restart)	/* Parse command-line options. */
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A006: Calling process_postgres_switches()\n");
    PGL_DEBUG_FLUSH(stderr);
	process_postgres_switches(argc, argv, PGC_POSTMASTER, &dbname);
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A007: process_postgres_switches() done, dbname=%s\n", dbname ? dbname : "NULL");
    PGL_DEBUG_FLUSH(stderr);


PDEBUG("# 260:"__FILE__);
	/* Must have gotten a database name, or have a default (the username) */
	if (dbname == NULL)
	{
		dbname = username;
		if (dbname == NULL)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s: no database nor user name specified",
							progname)));
	}

PDEBUG("# 291:SelectConfigFiles "__FILE__);
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A008: async_restart=%d, checking if we should skip to async_db_change\n", async_restart);
    PGL_DEBUG_FLUSH(stderr);
if (async_restart) {
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A009: async_restart=1, jumping to async_db_change\n");
    PGL_DEBUG_FLUSH(stderr);
    goto async_db_change;
}
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A010: async_restart=0, doing full init\n");
    PGL_DEBUG_FLUSH(stderr);
	/* Acquire configuration parameters */
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A011: Calling SelectConfigFiles()\n");
    PGL_DEBUG_FLUSH(stderr);
	if (!SelectConfigFiles(userDoption, progname)) {
        PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A012: SelectConfigFiles FAILED, calling proc_exit(1)\n");
        PGL_DEBUG_FLUSH(stderr);
        proc_exit(1);
    }
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A013: SelectConfigFiles() done\n");
    PGL_DEBUG_FLUSH(stderr);
PDEBUG("# 278:SelectConfigFiles "__FILE__);

    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A014: Calling checkDataDir()\n");
    PGL_DEBUG_FLUSH(stderr);
	checkDataDir();
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A015: checkDataDir() done\n");
    PGL_DEBUG_FLUSH(stderr);
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A016: Calling ChangeToDataDir()\n");
    PGL_DEBUG_FLUSH(stderr);
	ChangeToDataDir();
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A017: ChangeToDataDir() done\n");
    PGL_DEBUG_FLUSH(stderr);

	/*
	 * Create lockfile for data directory.
	 */
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A018: Calling CreateDataDirLockFile()\n");
    PGL_DEBUG_FLUSH(stderr);
	CreateDataDirLockFile(false);
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A019: CreateDataDirLockFile() done\n");
    PGL_DEBUG_FLUSH(stderr);

	/* read control file (error checking and contains config ) */
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A020: Calling LocalProcessControlFile()\n");
    PGL_DEBUG_FLUSH(stderr);
	LocalProcessControlFile(false);
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A021: LocalProcessControlFile() done\n");
    PGL_DEBUG_FLUSH(stderr);

	/*
	 * process any libraries that should be preloaded at postmaster start
	 */
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A022: Calling process_shared_preload_libraries()\n");
    PGL_DEBUG_FLUSH(stderr);
	process_shared_preload_libraries();
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A023: process_shared_preload_libraries() done\n");
    PGL_DEBUG_FLUSH(stderr);

	/* Initialize MaxBackends */
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A024: Calling InitializeMaxBackends()\n");
    PGL_DEBUG_FLUSH(stderr);
	InitializeMaxBackends();
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A025: InitializeMaxBackends() done\n");
    PGL_DEBUG_FLUSH(stderr);
PDEBUG("# 127"); /* on_shmem_exit stubs call start here */
	/*
	 * Give preloaded libraries a chance to request additional shared memory.
	 */
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A026: Calling process_shmem_requests()\n");
    PGL_DEBUG_FLUSH(stderr);
	process_shmem_requests();
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A027: process_shmem_requests() done\n");
    PGL_DEBUG_FLUSH(stderr);

	/*
	 * Now that loadable modules have had their chance to request additional
	 * shared memory, determine the value of any runtime-computed GUCs that
	 * depend on the amount of shared memory required.
	 */
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A028: Calling InitializeShmemGUCs()\n");
    PGL_DEBUG_FLUSH(stderr);
	InitializeShmemGUCs();
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A029: InitializeShmemGUCs() done\n");
    PGL_DEBUG_FLUSH(stderr);

	/*
	 * Now that modules have been loaded, we can process any custom resource
	 * managers specified in the wal_consistency_checking GUC.
	 */
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A030: Calling InitializeWalConsistencyChecking()\n");
    PGL_DEBUG_FLUSH(stderr);
	InitializeWalConsistencyChecking();
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A031: InitializeWalConsistencyChecking() done\n");
    PGL_DEBUG_FLUSH(stderr);

    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A032: Calling CreateSharedMemoryAndSemaphores()\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[AsyncPostgresSingleUserMain] About to CreateSharedMemoryAndSemaphores()");
#endif
	CreateSharedMemoryAndSemaphores();
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A033: CreateSharedMemoryAndSemaphores() done\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[AsyncPostgresSingleUserMain] CreateSharedMemoryAndSemaphores() completed");
#endif

	/*
	 * Remember stand-alone backend startup time,roughly at the same point
	 * during startup that postmaster does so.
	 */
	PgStartTime = GetCurrentTimestamp();
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A034: PgStartTime set\n");
    PGL_DEBUG_FLUSH(stderr);

	/*
	 * Create a per-backend PGPROC struct in shared memory. We must do this
	 * before we can use LWLocks.
	 */
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A035: Calling InitProcess()\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[AsyncPostgresSingleUserMain] About to InitProcess()");
#endif
	InitProcess();
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A036: InitProcess() done\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[AsyncPostgresSingleUserMain] InitProcess() completed");
#endif

// main
	SetProcessingMode(InitProcessing);
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A037: SetProcessingMode done\n");
    PGL_DEBUG_FLUSH(stderr);

	/* Early initialization */
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A038: Calling BaseInit()\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[AsyncPostgresSingleUserMain] About to BaseInit() - WAL recovery happens here");
#endif
	BaseInit();
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A039: BaseInit() done\n");
    PGL_DEBUG_FLUSH(stderr);
#ifdef PGL_MOBILE
    PGL_LOG_INFO("[AsyncPostgresSingleUserMain] BaseInit() completed - WAL recovery finished");
    debug_log_wal_state("POST-ASYNC-BASEINIT");
#endif
async_db_change:;
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A040: At async_db_change label\n");
    PGL_DEBUG_FLUSH(stderr);

PDEBUG("# 167");
	/*
	 * General initialization.
	 *
	 * NOTE: if you are tempted to add code in this vicinity, consider putting
	 * it inside InitPostgres() instead.  In particular, anything that
	 * involves database access should be there, not here.
	 */
	InitPostgres(dbname, InvalidOid,	/* database to connect to */
				 username, InvalidOid,	/* role to connect as */
                 (!am_walsender) ? INIT_PG_LOAD_SESSION_LIBS : 0,
				 NULL);			/* no out_dbname */

	/*
	 * If the PostmasterContext is still around, recycle the space; we don't
	 * need it anymore after InitPostgres completes.  Note this does not trash
	 * *MyProcPort, because ConnCreate() allocated that space with malloc()
	 * ... else we'd need to copy the Port data first.  Also, subsidiary data
	 * such as the username isn't lost either; see ProcessStartupPacket().
	 */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	SetProcessingMode(NormalProcessing);

	/*
	 * Now all GUC states are fully set up.  Report them to client if
	 * appropriate.
	 */
	BeginReportingGUCOptions();

	/*
	 * Also set up handler to log session end; we have to wait till now to be
	 * sure Log_disconnections has its final value.
	 */
	if (IsUnderPostmaster && Log_disconnections)
		on_proc_exit(log_disconnections, 0);

	pgstat_report_connect(MyDatabaseId);

	/* Perform initialization specific to a WAL sender process. */
	if (am_walsender)
		InitWalSender();

	/*
	 * Send this backend's cancellation info to the frontend.
	 */
	if (whereToSendOutput == DestRemote)
	{
		StringInfoData buf;

		pq_beginmessage(&buf, 'K');
		pq_sendint32(&buf, (int32) MyProcPid);
		pq_sendint32(&buf, (int32) MyCancelKey);
		pq_endmessage(&buf);
		/* Need not flush since ReadyForQuery will do it. */
	}

	/* Welcome banner for standalone case */
	if (whereToSendOutput == DestDebug)
		printf("\nPostgreSQL stand-alone backend %s\n", PG_VERSION);

	/*
	 * Create the memory context we will use in the main loop.
	 *
	 * MessageContext is reset once per iteration of the main loop, ie, upon
	 * completion of processing of each command message from the client.
	 */
	MessageContext = AllocSetContextCreate(TopMemoryContext, "MessageContext", ALLOCSET_DEFAULT_SIZES);

	/*
	 * Create memory context and buffer used for RowDescription messages. As
	 * SendRowDescriptionMessage(), via exec_describe_statement_message(), is
	 * frequently executed for ever single statement, we don't want to
	 * allocate a separate buffer every time.
	 */
	row_description_context = AllocSetContextCreate(TopMemoryContext, "RowDescriptionContext", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(row_description_context);
	initStringInfo(&row_description_buf);
	MemoryContextSwitchTo(TopMemoryContext);
    PGL_DEBUG_PRINT(stderr, "[AsyncPostgresSingleUserMain] A041: EXIT - function complete\n");
    PGL_DEBUG_FLUSH(stderr);
} // AsyncPostgresSingleUserMain


