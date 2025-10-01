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

int create_checkpoint(int checkpoint_point) {
    long ret = syscall(SERVERLESS_SYSCALL_NUM, OP_CREATE_CHECKPOINT, checkpoint_point, 0);
    if (ret < 0) {
        printf("Failed to create checkpoint: %s\n", strerror(-ret));
        return -1;
    }
    return 0;
}

int restore_checkpoint(int val) {
    long ret = syscall(SERVERLESS_SYSCALL_NUM, OP_RESTORE_CHECKPOINT, 0, val);
    
    // Unreachable if successful
    /* If we reach here, restore failed because it should have jumped to checkpointed RIP */
    printf("Restore syscall failed: %s\n", strerror(-ret));
    return -1;
}

int main(void) {
    static int counter = 0;
    printf("=== Simple Checkpoint/Restore Test ===\n");
    fflush(stdout);
    /* Checkpoint (C) */
    create_checkpoint(CHECKPOINT_PROGRAM_INITIALIZED);
    /*
     * Checkpoint created here. Program state should be saved.
     * On restore, execution should jump back to this point.
     */

    counter++; // should always be 1
    printf("Counter value: %d\n", counter);
    fflush(stdout);  /* Force the printf to appear immediately */

    /* Restore (R) */
    restore_checkpoint(counter); // just pass the counter for debugging

    /* WARN: unreachable */
    printf("ERROR: Should not reach here after restore!\n");
    return 0;
}
