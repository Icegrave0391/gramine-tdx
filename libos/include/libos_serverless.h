/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2024 Intel Corporation */

/*
 * This file contains definitions for serverless checkpoint/restore functionality.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pal.h"

/* Serverless checkpoint operations */
#define SERVERLESS_OP_CREATE_CHECKPOINT     1
#define SERVERLESS_OP_RESTORE_CHECKPOINT    2
#define SERVERLESS_OP_CHECK_STATUS          3
#define SERVERLESS_OP_ENABLE_CHECKPOINT     4
#define SERVERLESS_OP_DISABLE_CHECKPOINT    5

/* Checkpoint creation points */
#define CHECKPOINT_PYTHON_INITIALIZED       1
#define CHECKPOINT_MODULES_LOADED           2
#define CHECKPOINT_READY_FOR_EXECUTION      3

/* Function declarations */
int init_serverless_checkpoint(void);
int serverless_create_checkpoint(int checkpoint_point);
int serverless_create_checkpoint_from_syscall(int checkpoint_point, PAL_CONTEXT* ring3_context);
int serverless_restore_checkpoint(void);
bool serverless_has_checkpoint(void);
int serverless_clear_checkpoint(void);
long libos_syscall_serverless_checkpoint(long operation, long arg1, long arg2);

/* Utility functions */
static inline uint64_t get_timestamp(void) {
    uint64_t timestamp;
    __asm__ volatile("rdtsc" : "=A"(timestamp));
    return timestamp;
}

/* Environment variable helpers */
const char* pal_get_host_env(const char* key);
