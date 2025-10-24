/*
 * PAL serverless support header for Gramine-VM.
 * 
 * This header declares PAL-level APIs for serverless checkpoint/restore functionality.
 * These functions allow LibOS to query system information needed for checkpoint operations.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Get the number of virtual CPUs available in the system.
 * 
 * This function provides LibOS access to the vCPU count that was detected
 * during PAL initialization via TDX TDCALL or other platform-specific mechanisms.
 * 
 * Returns:
 *   Number of vCPUs (1 to MAX_NUM_CPUS)
 */
uint32_t PalServerlessGetVcpuCount(void);

/*
 * Get detailed CPU topology information.
 * 
 * This function provides LibOS access to the CPU topology information
 * that is available through the PAL public state.
 * 
 * Parameters:
 *   threads_cnt - Pointer to store the number of hardware threads
 *   cores_cnt   - Pointer to store the number of CPU cores  
 *   sockets_cnt - Pointer to store the number of CPU sockets
 * 
 * Returns:
 *   0 on success, negative error code on failure
 */
int PalServerlessGetCpuTopology(size_t* threads_cnt, size_t* cores_cnt, size_t* sockets_cnt);

/*
 * Get total system memory size.
 * 
 * Returns:
 *   Total memory size in bytes, or 0 if not available
 */
size_t PalServerlessGetMemTotal(void);

/*
 * Get host type information.
 * 
 * Returns:
 *   String describing the host type (e.g., "TDX", "VM"), or NULL if not available
 */
const char* PalServerlessGetHostType(void);

/*
 * Check if a specific CPU thread is online.
 * 
 * Parameters:
 *   thread_id - Hardware thread ID to check
 * 
 * Returns:
 *   true if the thread is online, false otherwise
 */
bool PalServerlessIsCpuOnline(size_t thread_id);