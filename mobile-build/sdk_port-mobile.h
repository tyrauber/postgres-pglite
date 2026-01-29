#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Simple mobile CMA-like buffer semantics used by interactive_one.c
int get_buffer_size(int fd);       // capacity for channel fd
intptr_t get_buffer_addr(int fd);  // native pointer to buffer base + 1 for IO[]
// interactive_write is defined in interactive_one.c for both WASM and mobile
int interactive_read(void);        // return cma_wsize
void use_wire(int state);          // >0 wire mode, <=0 repl

// Skip startup auth for TCP servers that handle auth externally
// Must be called after pgl_backend() and use_wire(1) but before first interactive_one()
void pgl_skip_auth(void);

// Reset wire protocol session state for new client connections
// Call this when a client disconnects before accepting a new connection
// Resets: mobile_auth_started, CMA buffers, PQ send/recv buffers, channel
void pgl_reset_wire_session(void);

// I/O hook registration for custom storage layers (EFS, S3, etc.)
// See pgl_io_hooks.h for the full type definitions.
#include "pgl_io_hooks.h"

// Expose variables similar to wasm build  
extern volatile int pgl_mobile_cma_wsize;  // External variable defined in pqcomm.c
extern volatile int cma_rsize;
extern volatile int channel;
extern volatile bool is_wire;
extern volatile bool is_repl;

#ifdef __cplusplus
}
#endif

