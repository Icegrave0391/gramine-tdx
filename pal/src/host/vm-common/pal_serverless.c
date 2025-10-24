/*
 * PAL serverless support for Gramine-VM.
 * 
 * This module provides PAL-level APIs for serverless checkpoint/restore functionality.
 * Allows LibOS to query system information like vCPU count needed for checkpoint operations.
 */

#include "api.h"
#include "kernel_multicore.h"
#include "kernel_sched.h"
#include "kernel_thread.h"
#include "pal_common.h"
#include "pal_serverless.h"
#include "pal_internal.h"
#include "pal_topology.h"

/* Helper function to get thread state name */
static const char* get_thread_state_name(enum thread_state state) {
    switch (state) {
        case THREAD_STOPPED:  return "STOPPED";
        case THREAD_RUNNABLE: return "RUNNABLE";
        case THREAD_BLOCKED:  return "BLOCKED";
        case THREAD_RUNNING:  return "RUNNING";
        default:              return "UNKNOWN";
    }
}

/* Helper function to get PAL handle from thread */
static struct pal_handle* get_pal_handle_from_thread(struct thread* thread) {
    if (!thread)
        return NULL;
    
    /* Calculate TCB address from thread address */
    uintptr_t tcb_addr = (uintptr_t)thread - offsetof(struct pal_tcb_vm, kernel_thread);
    struct pal_tcb_vm* tcb = (struct pal_tcb_vm*)tcb_addr;
    
    return tcb->thread_handle;
}

/* Callback function to print information about a single PAL thread */
static void print_pal_thread_info(struct thread* thread, size_t index) {
    if (!thread) {
        log_always("Thread[%zu]: NULL thread pointer", index);
        return;
    }
    
    log_always("=== PAL Thread[%zu] ===", index);
    log_always("  Thread ID: %u", thread->thread_id);
    log_always("  State: %s", get_thread_state_name(thread->state));
    log_always("  Is Helper: %s", thread->is_helper ? "YES" : "NO");
    log_always("  Blocked On: %p", thread->blocked_on);
    
    /* Print CPU affinity mask */
    log_always("  CPU Affinity Mask:");
    for (size_t i = 0; i < MAX_NUM_CPU_LONGS; i++) {
        if (thread->cpu_mask[i] != 0) {
            log_always("    cpu_mask[%zu]: 0x%lx", i, thread->cpu_mask[i]);
        }
    }
    
    /* Print thread context (CPU registers) */
    log_always("  Thread Context (CPU Registers):");
    log_always("    General Purpose Registers:");
    log_always("      RAX: 0x%016lx  RBX: 0x%016lx  RCX: 0x%016lx  RDX: 0x%016lx", 
              thread->context.rax, thread->context.rbx, thread->context.rcx, thread->context.rdx);
    log_always("      RSI: 0x%016lx  RDI: 0x%016lx  RBP: 0x%016lx  RSP: 0x%016lx",
              thread->context.rsi, thread->context.rdi, thread->context.rbp, thread->context.rsp);
    log_always("      R8:  0x%016lx  R9:  0x%016lx  R10: 0x%016lx  R11: 0x%016lx",
              thread->context.r8, thread->context.r9, thread->context.r10, thread->context.r11);
    log_always("      R12: 0x%016lx  R13: 0x%016lx  R14: 0x%016lx  R15: 0x%016lx",
              thread->context.r12, thread->context.r13, thread->context.r14, thread->context.r15);
    
    log_always("    Control Registers:");
    log_always("      RIP: 0x%016lx  RFLAGS: 0x%016lx", thread->context.rip, thread->context.rflags);
    log_always("      User RIP: 0x%016lx  User FSBASE: 0x%016lx", 
              thread->context.user_rip, thread->context.user_fsbase);
    
    log_always("    Segment Registers:");
    log_always("      CSGSFSSS: 0x%016lx", thread->context.csgsfsss);
    
    log_always("    Exception Context:");
    log_always("      ERR: 0x%016lx  TRAPNO: 0x%016lx  CR2: 0x%016lx", 
              thread->context.err, thread->context.trapno, thread->context.cr2);
    log_always("      OLDMASK: 0x%016lx", thread->context.oldmask);
    
    log_always("    FPU Context:");
    log_always("      FPREGS: %p", thread->context.fpregs);
    
    /* Print IRQ pseudo stack */
    log_always("  IRQ Pseudo Stack:");
    log_always("    RIP: 0x%016lx  CS: 0x%016lx  RFLAGS: 0x%016lx",
              thread->irq_pseudo_stack.rip, thread->irq_pseudo_stack.cs, thread->irq_pseudo_stack.rflags);
    log_always("    RSP: 0x%016lx  SS: 0x%016lx", 
              thread->irq_pseudo_stack.rsp, thread->irq_pseudo_stack.ss);
    
    /* Print sigreturn context */
    log_always("  Sigreturn Context:");
    log_always("    User RSP: 0x%016lx  User RAX: 0x%016lx  Pseudo RSP: 0x%016lx",
              thread->sigreturn_user_rsp, thread->sigreturn_user_rax, thread->sigreturn_pseudo_rsp);
    
    /* Print associated PAL handle information */
    struct pal_handle* pal_handle = get_pal_handle_from_thread(thread);
    if (pal_handle) {
        log_always("  Associated PAL Handle: %p", pal_handle);
        log_always("    Type: %d", pal_handle->hdr.type);
        if (pal_handle->hdr.type == PAL_TYPE_THREAD) {
            log_always("    Thread TID: %d", pal_handle->thread.tid);
            log_always("    Thread Stack: %p", pal_handle->thread.stack);
            log_always("    Kernel Thread: %p", pal_handle->thread.kernel_thread);
        }
    } else {
        log_always("  Associated PAL Handle: NULL");
    }
    
    log_always("=== End PAL Thread[%zu] ===", index);
}

/* Internal function to walk through all PAL threads and print detailed information */
static void walk_pal_thread_list(void) {
    log_always("=== PAL Thread List Walkthrough ===");
    
    size_t total_threads = sched_get_thread_count();
    log_always("Total PAL threads: %zu", total_threads);
    
    if (total_threads == 0) {
        log_always("No PAL threads found in scheduler");
        log_always("=== End PAL Thread List Walkthrough ===");
        return;
    }
    
    log_always("Walking through all PAL threads...");
    sched_walk_thread_list_for_debug(print_pal_thread_info);
    
    log_always("=== End PAL Thread List Walkthrough ===");
}

/*
 * Get the number of virtual CPUs available in the system.
 * 
 * This function provides LibOS access to the vCPU count that was detected
 * during PAL initialization via TDX TDCALL or other platform-specific mechanisms.
 * 
 * Returns:
 *   Number of vCPUs (1 to MAX_NUM_CPUS)
 */
uint32_t PalServerlessGetVcpuCount(void) {
    /* Return the global vCPU count that was set during PAL initialization */
    return g_num_cpus;
}

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
int PalServerlessGetCpuTopology(size_t* threads_cnt, size_t* cores_cnt, size_t* sockets_cnt) {
    if (!threads_cnt || !cores_cnt || !sockets_cnt) {
        return -PAL_ERROR_INVAL;
    }

    /* Get the PAL public state which contains topology information */
    struct pal_public_state* pal_state = PalGetPalPublicState();
    if (!pal_state) {
        return -PAL_ERROR_NOTIMPLEMENTED;
    }

    /* Extract topology information */
    *threads_cnt = pal_state->topo_info.threads_cnt;
    *cores_cnt = pal_state->topo_info.cores_cnt;
    *sockets_cnt = pal_state->topo_info.sockets_cnt;

    return 0;
}

/*
 * Get total system memory size.
 * 
 * Returns:
 *   Total memory size in bytes, or 0 if not available
 */
size_t PalServerlessGetMemTotal(void) {
    struct pal_public_state* pal_state = PalGetPalPublicState();
    if (!pal_state) {
        return 0;
    }

    return pal_state->mem_total;
}

/*
 * Get host type information.
 * 
 * Returns:
 *   String describing the host type (e.g., "TDX", "VM"), or NULL if not available
 */
const char* PalServerlessGetHostType(void) {
    struct pal_public_state* pal_state = PalGetPalPublicState();
    if (!pal_state) {
        return NULL;
    }

    return pal_state->host_type;
}

/*
 * Check if a specific CPU thread is online.
 * 
 * Parameters:
 *   thread_id - Hardware thread ID to check
 * 
 * Returns:
 *   true if the thread is online, false otherwise
 */
bool PalServerlessIsCpuOnline(size_t thread_id) {
    struct pal_public_state* pal_state = PalGetPalPublicState();
    if (!pal_state || thread_id >= pal_state->topo_info.threads_cnt) {
        return false;
    }

    return pal_state->topo_info.threads[thread_id].is_online;
}

/*
 * Get the number of PAL internal threads.
 * 
 * This function provides LibOS access to the number of threads currently
 * managed by PAL's internal scheduler.
 * 
 * Returns:
 *   Number of PAL internal threads
 */
size_t PalServerlessGetPalThreadCount(void) {
    size_t count = sched_get_thread_count();
    
    /* Also perform detailed thread walkthrough for debugging */
    walk_pal_thread_list();
    
    return count;
}