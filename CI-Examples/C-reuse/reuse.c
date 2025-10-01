/* Copyright (C) 2023 Gramine contributors
 * SPDX-License-Identifier: BSD-3-Clause */

#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <string.h>

/* Serverless checkpoint syscall operations */
#define SERVERLESS_SYSCALL_NUM 999
#define OP_CREATE_CHECKPOINT   1
#define OP_RESTORE_CHECKPOINT  2
#define OP_CHECK_STATUS        3
#define OP_CLEAR_CHECKPOINT    4

/* Checkpoint points */
#define CHECKPOINT_PROGRAM_INITIALIZED  1

/* Helper functions for checkpoint/restore operations */
long do_checkpoint_syscall(long op, long arg1, long arg2) {
    return syscall(SERVERLESS_SYSCALL_NUM, op, arg1, arg2);
}

int create_checkpoint(int checkpoint_point) {
    printf("Creating checkpoint at point %d...\n", checkpoint_point);
    long ret = do_checkpoint_syscall(OP_CREATE_CHECKPOINT, checkpoint_point, 0);
    if (ret < 0) {
        printf("Failed to create checkpoint: %s\n", strerror(-ret));
        return -1;
    }
    printf("Checkpoint created successfully (result: %ld)\n", ret);
    return 0;
}

int restore_checkpoint(void) {
    printf("Restoring from checkpoint...\n");
    long ret = do_checkpoint_syscall(OP_RESTORE_CHECKPOINT, 0, 0);
    
    // Unreachable if successful
    /* If we reach here, restore failed because it should have jumped to checkpointed RIP */
    printf("Restore syscall failed: %s\n", strerror(-ret));
    return -1;
}

int main(void) {
    static int counter = 0;
    printf("=== Simple Checkpoint/Restore Test ===\n");

    /* Checkpoint (C) */
    create_checkpoint(CHECKPOINT_PROGRAM_INITIALIZED);
    /*
     * Checkpoint created here. Program state should be saved.
     * On restore, execution should jump back to this point.
     */

    counter++; // should always be 1
    printf("Counter value: %d\n", counter);

    /* Restore (R) */
    restore_checkpoint();

    /* WARN: unreachable */
    printf("ERROR: Should not reach here after restore!\n");
    return 0;
}
