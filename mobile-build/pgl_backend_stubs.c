#include <stdbool.h>
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../pglite-wasm/pgl_os.h"

#include "postgres.h"
#include "miscadmin.h"           // BackendType, sig_atomic_t globals
#include "storage/latch.h"       // Latch, MyLatch
#include "postmaster/bgworker.h" // BackgroundWorker
#include "libpq/libpq-be.h"      // Port definition

#include "utils/timeout.h" // TimeoutId, TimestampTz
#include "storage/shmem.h" // Size, ShmemInitStruct

// Provide weak default definitions so real backend objects can override if linked
volatile sig_atomic_t InterruptPending __attribute__((weak)) = 0;
volatile sig_atomic_t QueryCancelPending __attribute__((weak)) = 0;
volatile sig_atomic_t ProcDiePending __attribute__((weak)) = 0;
volatile bool ClientAuthInProgress __attribute__((weak)) = false;
volatile bool notifyInterruptPending __attribute__((weak)) = false;
volatile bool catchupInterruptPending __attribute__((weak)) = 0;
volatile sig_atomic_t CheckClientConnectionPending __attribute__((weak)) = 0;
volatile sig_atomic_t ClientConnectionLost __attribute__((weak)) = 0;

// Holdoff counters used by ProcessInterrupts (match miscadmin.h types)
volatile uint32 InterruptHoldoffCount __attribute__((weak)) = 0;
volatile uint32 CritSectionCount __attribute__((weak)) = 0;
volatile uint32 QueryCancelHoldoffCount __attribute__((weak)) = 0;

// Timeout indicators used by ProcessInterrupts (match miscadmin.h types)
volatile sig_atomic_t IdleInTransactionSessionTimeoutPending __attribute__((weak)) = 0;
volatile sig_atomic_t TransactionTimeoutPending __attribute__((weak)) = 0;
volatile sig_atomic_t IdleSessionTimeoutPending __attribute__((weak)) = 0;
volatile sig_atomic_t IdleStatsUpdateTimeoutPending __attribute__((weak)) = 0;

BackendType MyBackendType __attribute__((weak)) = B_BACKEND;
BackgroundWorker *MyBgworkerEntry __attribute__((weak)) = NULL;

// Provide a basic latch instance so pointer is valid
static Latch s_MyLatch = {0};
Latch *MyLatch __attribute__((weak)) = &s_MyLatch;

// Parallel / barrier / logging related flags
volatile sig_atomic_t ProcSignalBarrierPending __attribute__((weak)) = 0;
bool ParallelMessagePending __attribute__((weak)) = false;
bool ParallelApplyMessagePending __attribute__((weak)) = false;
volatile sig_atomic_t LogMemoryContextPending __attribute__((weak)) = 0;

// Protocol state variables used by interactive_one.c
volatile bool ignore_till_sync __attribute__((weak)) = false;
volatile bool send_ready_for_query __attribute__((weak)) = true;

// Additional missing variables for mobile build
#ifndef EOF
#define EOF (-1)
#endif

// Parser stats GUC
bool log_parser_stats __attribute__((weak)) = false;

// Max backends (used by shared memory sizing helpers)
int MaxBackends __attribute__((weak)) = 64;

// Minimal implementations
void __attribute__((weak)) ProcessNotifyInterrupt(bool flush) { (void)flush; }
void __attribute__((weak)) ProcessCatchupInterrupt(void) {}
void __attribute__((weak)) ProcessLogMemoryContextInterrupt(void) {}
void __attribute__((weak)) HandleParallelMessages(void) {}
void __attribute__((weak)) HandleParallelApplyMessages(void) {}

bool __attribute__((weak)) pq_check_connection(void) { return true; }
bool __attribute__((weak)) IsTransactionOrTransactionBlock(void) { return false; }

void __attribute__((weak)) enable_timeout_after(TimeoutId id, int delay)
{
  (void)id;
  (void)delay;
}
bool __attribute__((weak)) get_timeout_indicator(TimeoutId id, bool reset)
{
  (void)id;
  (void)reset;
  return false;
}
TimestampTz __attribute__((weak)) get_timeout_finish_time(TimeoutId id)
{
  (void)id;
  return 0;
}

// Timeouts: The real implementations in timeout.c are used.
// These weak stubs are only here as fallbacks if timeout.c isn't linked.
void __attribute__((weak)) disable_timeout(TimeoutId id, bool keep_indicator)
{
  (void)id;
  (void)keep_indicator;
}
void __attribute__((weak)) disable_all_timeouts(bool keep_indicator) { (void)keep_indicator; }
void __attribute__((weak)) reschedule_timeouts(void) {}

// NOTE: ShmemInitStruct is provided by shmem.o (not stubbed here).
// The real implementation uses ShmemSegHdr/ShmemBase which are initialized
// by InitShmemAccess() called from CreateSharedMemoryAndSemaphores().
// Our PGSharedMemoryCreate() provides the fake shared memory segment.

// NOTE: add_size and mul_size are provided by shmem.o (not stubbed here).

// Stats reporting stub
void __attribute__((weak)) pgstat_report_stat(bool force) { (void)force; }

// NOTE: proc_exit is now handled directly in PostgreSQL's ipc.c with PGL_MOBILE support.
// The implementation uses pgl_boot_jmp (defined in pg_main.c) to longjmp back to the
// caller instead of calling exit(), which would terminate the host app.
// See: vendor/postgres-pglite/src/backend/storage/ipc/ipc.c

// Removed stub pq_init - the real pq_init in pqcomm.c handles PGL_MOBILE properly
// and initializes critical variables like PqRecvLength/PqRecvPointer that error
// recovery code depends on. Socket operations in pq_init are now properly guarded
// to skip on mobile platforms while preserving essential buffer initialization.

// pg_proc_exit is a convenience wrapper used by some code
extern void proc_exit(int code);
void pg_proc_exit(int code) { proc_exit(code); }

// WASM pipe emulation symbols expected by interactive_one.c
FILE *__attribute__((weak)) SOCKET_FILE = NULL;
int __attribute__((weak)) SOCKET_DATA = 0;
void __attribute__((weak)) pq_recvbuf_fill(FILE *fp, int packetlen)
{
  (void)fp;
  (void)packetlen;
}

// Removed pq_getbyte and pq_getbytes overrides - these should NOT be overridden!
// PostgreSQL's pq_getbyte/pq_getbytes work through PQcommMethods which we've
// already set up in pgl_mobile_comm.c. Overriding them directly breaks bootstrap
// mode where regular file I/O is needed.

// Logical replication worker queries
bool __attribute__((weak)) IsLogicalWorker(void) { return false; }
bool __attribute__((weak)) IsLogicalLauncher(void) { return false; }

// =============================================================================
// Semaphore stubs for mobile (single-user mode, no IPC needed)
// =============================================================================
// iOS doesn't support sem_open() in sandboxed apps, and System V semaphores
// require kernel support not available on iOS. Since PGLite runs in single-user
// mode without multiple backend processes, we can stub these out safely.
//
// NOTE: These are STRONG symbols (no weak attribute) because we exclude
// pg_sema.o and pg_shmem.o from the build. See build-native.sh.

#include "storage/pg_sema.h"

// Simple in-memory semaphore implementation for single-user mode
typedef struct PGSemaphoreData
{
  int count;
} PGSemaphoreData;

// Pool of semaphores (allocated in regular heap since we don't need shared memory)
static PGSemaphoreData *semaphorePool = NULL;
static int numSemaphores = 0;
static int maxSemaphores = 0;

Size PGSemaphoreShmemSize(int maxSemas)
{
  // No shared memory needed for single-user mode
  (void)maxSemas;
  return 0;
}

void PGReserveSemaphores(int maxSemas)
{
  // Allocate semaphore pool in regular heap
  if (semaphorePool == NULL && maxSemas > 0)
  {
    semaphorePool = (PGSemaphoreData *)calloc(maxSemas, sizeof(PGSemaphoreData));
    maxSemaphores = maxSemas;
    numSemaphores = 0;
  }
}

PGSemaphore PGSemaphoreCreate(void)
{
  if (semaphorePool == NULL || numSemaphores >= maxSemaphores)
  {
    // If pool is full or not initialized, allocate new one
    PGSemaphore sema = (PGSemaphore)calloc(1, sizeof(PGSemaphoreData));
    if (sema)
      sema->count = 1; // Initial count of 1
    return sema;
  }
  PGSemaphore sema = &semaphorePool[numSemaphores++];
  sema->count = 1; // Initial count of 1
  return sema;
}

void PGSemaphoreReset(PGSemaphore sema)
{
  if (sema)
    sema->count = 0;
}

void PGSemaphoreLock(PGSemaphore sema)
{
  // In single-user mode, just decrement count (no actual blocking needed)
  if (sema)
    sema->count--;
}

void PGSemaphoreUnlock(PGSemaphore sema)
{
  if (sema)
    sema->count++;
}

bool PGSemaphoreTryLock(PGSemaphore sema)
{
  if (sema && sema->count > 0)
  {
    sema->count--;
    return true;
  }
  return false;
}

// =============================================================================
// Shared memory stubs - CreateSharedMemoryAndSemaphores needs these
// =============================================================================
// NOTE: These are STRONG symbols (no weak attribute) because we exclude
// pg_shmem.o from the build. See build-native.sh.

#include "storage/shmem.h"
#include "storage/pg_shmem.h"

// PGShm* functions are called by CreateSharedMemoryAndSemaphores
// In single-user mode, we can use regular heap memory instead of actual shared memory

static void *fakeSharedMemory = NULL;
static Size fakeSharedMemorySize = 0;

// Global variables expected by PostgreSQL
unsigned long UsedShmemSegID = 0;
void *UsedShmemSegAddr = NULL;

PGShmemHeader *PGSharedMemoryCreate(Size size, PGShmemHeader **shim)
{
  // Allocate fake shared memory using calloc (heap memory for single-user mode)
  fakeSharedMemorySize = size;
  fakeSharedMemory = calloc(1, size);
  if (!fakeSharedMemory)
  {
    // Emergency fallback - should never happen
    return NULL;
  }

  UsedShmemSegAddr = fakeSharedMemory;

  // Set up the header
  PGShmemHeader *header = (PGShmemHeader *)fakeSharedMemory;
  header->magic = PGShmemMagic;
  header->creatorPID = getpid();
  header->totalsize = size;
  header->freeoffset = sizeof(PGShmemHeader);
  header->dsm_control = 0;
  header->index = NULL;

  if (shim)
    *shim = header;
  return header;
}

bool PGSharedMemoryIsInUse(unsigned long id1, unsigned long id2)
{
  (void)id1;
  (void)id2;
  return false; // No shared memory conflicts in single-user mode
}

void PGSharedMemoryDetach(void)
{
  if (fakeSharedMemory)
  {
    free(fakeSharedMemory);
    fakeSharedMemory = NULL;
    fakeSharedMemorySize = 0;
  }
  UsedShmemSegAddr = NULL;
}

void GetHugePageSize(Size *hugepagesize, int *mmap_flags)
{
  // iOS doesn't support huge pages
  if (hugepagesize)
    *hugepagesize = 0;
  if (mmap_flags)
    *mmap_flags = 0;
}

// GUC check hook for huge_page_size setting
// This is referenced by guc_tables.c and was originally in pg_shmem.c
#include "utils/guc.h"
bool check_huge_page_size(int *newval, void **extra, GucSource source)
{
  // iOS doesn't support huge pages, so only 0 is allowed
  (void)extra;
  (void)source;
  if (*newval != 0)
  {
    GUC_check_errdetail("\"huge_page_size\" must be 0 on this platform.");
    return false;
  }
  return true;
}

// ============================================================================
// Encoding function wrappers for libpq compatibility
// ============================================================================
// PostgreSQL builds libpgcommon_srv.a with _private suffix on encoding functions,
// but libpq expects the non-private names. These wrappers bridge the gap.
// This allows us to use only _srv libraries without needing _shlib duplicates.
//
// IMPORTANT: pg_wchar.h defines macros that rename these functions to _private.
// We must undefine them to create the actual public symbols.

#undef pg_char_to_encoding
#undef pg_encoding_to_char

extern int pg_char_to_encoding_private(const char *name);
extern const char *pg_encoding_to_char_private(int encoding);

int pg_char_to_encoding(const char *name)
{
  return pg_char_to_encoding_private(name);
}

const char *pg_encoding_to_char(int encoding)
{
  return pg_encoding_to_char_private(encoding);
}