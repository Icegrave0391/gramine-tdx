/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2024 Intel Corporation */

/*
 * Implementation of custom syscall 999 for serverless checkpoint/restore functionality.
 * This addresses the cold-start problem by allowing Python runtime state to be preserved
 * and reused across function invocations.
 */

#include <linux_abi/errors.h>

#include "libos_internal.h" 
#include "libos_lock.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "libos_utils.h"

/* Operations for serverless checkpoint syscall */
#define OP_CREATE_CHECKPOINT  1
#define OP_RESTORE_CHECKPOINT 2
#define OP_CHECK_STATUS       3

/* Checkpoint points */
// #define CHECKPOINT_PYTHON_INITIALIZED  1
// #define CHECKPOINT_MODULES_LOADED      2
// #define CHECKPOINT_READY_FOR_EXECUTION 3

/* External functions from libos_serverless.c */
extern int serverless_create_checkpoint(int checkpoint_point);
extern int serverless_restore_checkpoint(void);
extern int init_serverless_checkpoint(void);
extern void serverless_on_python_ready(void);
extern void serverless_on_function_complete(void);

/*
 * Custom syscall handler for serverless checkpoint operations.
 * Syscall number: 999 (__NR_serverless_checkpoint)
 *
 * Arguments:
 *   operation: OP_CREATE_CHECKPOINT, OP_RESTORE_CHECKPOINT, or OP_CHECK_STATUS
 *   arg1: checkpoint point (for CREATE_CHECKPOINT), unused otherwise
 *   arg2: unused, reserved for future extensions
 *
 * Returns:
 *   0 on success, negative error code on failure
 *   For CHECK_STATUS: 1 if checkpoint available, 0 if not available
 */
long libos_syscall_serverless_checkpoint(long operation, long arg1, long arg2) {
    __UNUSED(arg2); /* Reserved for future use */

    log_debug("Serverless checkpoint syscall: op=%ld, arg1=%ld, arg2=%ld", 
              operation, arg1, arg2);

    switch (operation) {
        case OP_CREATE_CHECKPOINT: {
            int checkpoint_point = 0;

            log_always("Creating serverless checkpoint at point %d", checkpoint_point);
            
            int ret = serverless_create_checkpoint(checkpoint_point);
            if (ret < 0) {
                log_error("Failed to create checkpoint: %s", unix_strerror(ret));
                return ret;
            }
            
            log_always("Serverless checkpoint created successfully");
            return 0;
        }
        
        case OP_RESTORE_CHECKPOINT: {
            log_debug("Restoring from serverless checkpoint");
            
            int ret = serverless_restore_checkpoint();
            if (ret < 0) {
                log_error("Failed to restore checkpoint: %s", unix_strerror(ret));
                return ret;
            }
            
            /* Note: If restore is successful, execution should not reach here
             * as the process state should be reset to the checkpoint */
            log_debug("Serverless checkpoint restored successfully");
            return 0;
        }
        
        case OP_CHECK_STATUS: {
            /* Check if checkpoint system has a checkpoint */
            extern bool serverless_has_checkpoint(void);
            
            int status = serverless_has_checkpoint() ? 1 : 0;
            
            log_debug("Checkpoint status check: %s", status ? "available" : "not available");
            return status;
        }
        
        default:
            log_error("Invalid serverless checkpoint operation: %ld", operation);
            return -ENOSYS;
    }
}

/*
 * Initialize the serverless checkpoint system during LibOS startup.
 * This should be called from libos_init.c
 */
int libos_init_serverless_checkpoint(void) {
    log_debug("Initializing serverless checkpoint system");
    
    int ret = init_serverless_checkpoint();
    if (ret < 0) {
        log_error("Failed to initialize serverless checkpoint system: %s", unix_strerror(ret));
        return ret;
    }
    
    log_always("Serverless checkpoint system initialized");
    return 0;
}

/*
 * Hook for Python runtime initialization complete.
 * This can be called when Python modules are loaded and the runtime is ready.
 */
void libos_serverless_python_ready_hook(void) {
    log_debug("Python runtime ready - triggering checkpoint creation");
    serverless_on_python_ready();
}

/*
 * Hook for serverless function execution complete.
 * This can be called after each function execution to trigger restore.
 */
void libos_serverless_function_complete_hook(void) {
    log_debug("Function execution complete - checking for auto-restore");
    serverless_on_function_complete();
}