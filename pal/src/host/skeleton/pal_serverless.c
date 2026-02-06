/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2024 */

/*
 * This file contains the implementation of PAL serverless functions for the skeleton PAL.
 * These are dummy implementations since skeleton PAL doesn't have access to real system information.
 */

#include "pal.h"
#include "pal_internal.h"

/* Dummy implementation for skeleton PAL */
uint32_t PalServerlessGetVcpuCount(void) {
    /* Return a default value for skeleton PAL */
    return -1;
}

/* Dummy implementation for skeleton PAL */
int PalServerlessGetCpuTopology(size_t* threads_cnt, size_t* cores_cnt, size_t* sockets_cnt) {
    if (!threads_cnt || !cores_cnt || !sockets_cnt) {
        return -PAL_ERROR_INVAL;
    }
    
    /* Return default topology for skeleton PAL */
    *threads_cnt = 0;
    *cores_cnt = 0;
    *sockets_cnt = 0;
    
    return 0;
}

/* Dummy implementation for skeleton PAL */
size_t PalServerlessGetMemTotal(void) {
    /* Return a default memory size (1GB) for skeleton PAL */
    return 1024 * 1024 * 1024;
}

/* Dummy implementation for skeleton PAL */
const char* PalServerlessGetHostType(void) {
    /* Return skeleton as host type */
    return "skeleton";
}

/* Dummy implementation for skeleton PAL */
bool PalServerlessIsCpuOnline(size_t thread_id) {
    /* In skeleton PAL, only CPU 0 is "online" */
    return (thread_id == 0);
}

/* Dummy implementation for skeleton PAL */
size_t PalServerlessGetPalThreadCount(void) {
    /* Return a dummy thread count for skeleton PAL */
    return 1;
}

/* Dummy implementation for skeleton PAL */
void PalServerlessCheckpointBarrierAcquire(void) {
    /* No-op for skeleton PAL - no multi-core synchronization needed */
}

/* Dummy implementation for skeleton PAL */
void PalServerlessCheckpointBarrierRelease(void) {
    /* No-op for skeleton PAL - no multi-core synchronization needed */
}

void PalServerlessModuleInit(uint64_t libos_sm_data_base, uint64_t libos_sm_data_end,
                             uint64_t libos_sm_code_base, uint64_t libos_sm_code_end) {
    /* No-op for skeleton PAL - no special initialization needed */
}