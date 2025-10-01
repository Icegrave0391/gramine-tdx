/*
 * Serverless checkpoint/restore for Gramine-VM.
 * 
 * In Gramine-VM: LibOS runs in ring-0, target program (e.g. Python) runs in ring-3.
 * When target program makes syscall, it traps to ring-0 LibOS.
 * 
 * This module checkpoints the ring-3 target program state:
 * - Ring-3 CPU registers (from syscall entry) 
 * - Ring-3 program memory (user space)
 * - Restore should return to ring-3 program after sysret
 */

#include <stdlib.h>
#include <string.h>

#include "libos_internal.h" 
#include "libos_utils.h"
#include "libos_vma.h"
#include "libos_tcb.h"
#include "libos_serverless.h"
#include "pal.h"

/* Global serverless checkpoint state for ring-3 target program */
static struct {
    bool checkpoint_created;
    bool enabled;
    
    /* Ring-3 target program CPU state (captured during syscall entry) */
    struct {
        /* General purpose registers */
        uint64_t rax, rbx, rcx, rdx;
        uint64_t rsi, rdi, rbp, rsp;
        uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
        
        /* Control registers */
        uint64_t rip;       /* Next instruction after the checkpoint-syscall returns */
        uint64_t rflags;    /* Flags register */
        
        /* Segment registers (ring-3 values) */
        uint64_t cs, ss, ds, es, fs, gs;
        
        /* Extended state (if needed) */
        // TODO: FPU/SSE/AVX state
    } guest_cpu_state;
    
    /* Ring-3 program memory snapshots */
    struct memory_region {
        void* start_addr;
        size_t size;
        void* snapshot_data;
        int protection;     /* PROT_* flags */
        char comment[32];   /* For debugging */
    } *memory_regions;
    size_t num_regions;
    
} g_serverless_checkpoint = {0};



/*
 * Capture ring-3 target program memory contents
 */
static int capture_memory_contents(void) {
    struct libos_vma_info* vmas;
    size_t vma_count;
    size_t target_regions = 0;
    
    /* Get all VMAs using the LibOS API */
    int ret = dump_all_vmas(/*include_unmapped=*/false, &vmas, &vma_count);
    if (ret < 0) {
        log_error("Failed to dump VMAs: %s", unix_strerror(ret));
        return ret;
    }
    
    /* Count ring-3 program memory regions (exclude LibOS internal memory and non-readable regions) */
    for (size_t i = 0; i < vma_count; i++) {
        struct libos_vma_info* vma = &vmas[i];
        
        /* Only checkpoint user program memory that is readable - exclude LibOS internal regions */
        if (!(vma->flags & VMA_INTERNAL) && !(vma->flags & VMA_UNMAPPED) && (vma->prot & PROT_READ)) {
            target_regions++;
        }
    }
    
    if (target_regions == 0) {
        log_debug("No ring-3 program memory to checkpoint");
        free_vma_info_array(vmas, vma_count);
        return 0;
    }
    
    /* Allocate memory region array */
    g_serverless_checkpoint.memory_regions = malloc(sizeof(struct memory_region) * target_regions);
    if (!g_serverless_checkpoint.memory_regions) {
        free_vma_info_array(vmas, vma_count);
        return -ENOMEM;
    }
    
    /* Capture each ring-3 program memory region */
    size_t region_idx = 0;
    for (size_t i = 0; i < vma_count; i++) {
        struct libos_vma_info* vma = &vmas[i];
        
        if (!(vma->flags & VMA_INTERNAL) && !(vma->flags & VMA_UNMAPPED) && (vma->prot & PROT_READ)) {
            
            struct memory_region* region = &g_serverless_checkpoint.memory_regions[region_idx];
            
            region->start_addr = vma->addr;
            region->size = vma->length;
            region->protection = vma->prot;
            size_t comment_len = strlen(vma->comment);
            size_t copy_len = (comment_len < sizeof(region->comment) - 1) ? comment_len : sizeof(region->comment) - 1;
            memcpy(region->comment, vma->comment, copy_len);
            region->comment[copy_len] = '\0';
            
            /* Allocate snapshot memory */
            region->snapshot_data = malloc(region->size);
            if (!region->snapshot_data) {
                /* Cleanup on failure */
                for (size_t j = 0; j < region_idx; j++) {
                    free(g_serverless_checkpoint.memory_regions[j].snapshot_data);
                }
                free(g_serverless_checkpoint.memory_regions);
                free_vma_info_array(vmas, vma_count);
                return -ENOMEM;
            }
            
            /* Copy ring-3 program memory contents */
            memcpy(region->snapshot_data, region->start_addr, region->size);
            
            log_debug("Captured ring-3 memory region %p-%p (%zu bytes) [%s]", 
                     region->start_addr, 
                     (char*)region->start_addr + region->size, 
                     region->size, 
                     region->comment);
            
            region_idx++;
        }
    }
    
    g_serverless_checkpoint.num_regions = target_regions;
    log_debug("Captured %zu ring-3 program memory regions (total: %zu VMAs)", 
              target_regions, vma_count);
    
    free_vma_info_array(vmas, vma_count);
    return 0;
}

/*
 * Restore ring-3 program memory contents from checkpoint
 */
static int restore_memory_contents(void) {
    if (!g_serverless_checkpoint.memory_regions) {
        log_debug("No ring-3 memory snapshots to restore");
        return 0;
    }
    
    for (size_t i = 0; i < g_serverless_checkpoint.num_regions; i++) {
        struct memory_region* region = &g_serverless_checkpoint.memory_regions[i];
        
        log_debug("Restoring ring-3 memory region %p-%p (%zu bytes) [%s]",
                 region->start_addr, 
                 (char*)region->start_addr + region->size, 
                 region->size,
                 region->comment);
        
        /* Restore ring-3 program memory from snapshot */
        memcpy(region->start_addr, region->snapshot_data, region->size);
    }
    
    log_debug("Restored %zu ring-3 program memory regions", g_serverless_checkpoint.num_regions);
    return 0;
}

/*
 * Capture ring-3 target program CPU state from syscall context
 * This should be called when the syscall handler has the ring-3 state saved
 */
static int capture_cpu_state_from_syscall_context(PAL_CONTEXT* ring3_context) {
    if (!ring3_context) {
        log_error("No ring-3 context provided for checkpoint");
        return -EINVAL;
    }
    
    /* Extract ring-3 CPU state from syscall context */
    g_serverless_checkpoint.guest_cpu_state.rax = ring3_context->rax;
    g_serverless_checkpoint.guest_cpu_state.rbx = ring3_context->rbx;
    g_serverless_checkpoint.guest_cpu_state.rcx = ring3_context->rcx;
    g_serverless_checkpoint.guest_cpu_state.rdx = ring3_context->rdx;
    g_serverless_checkpoint.guest_cpu_state.rsi = ring3_context->rsi;
    g_serverless_checkpoint.guest_cpu_state.rdi = ring3_context->rdi;
    g_serverless_checkpoint.guest_cpu_state.rbp = ring3_context->rbp;
    g_serverless_checkpoint.guest_cpu_state.rsp = ring3_context->rsp;
    g_serverless_checkpoint.guest_cpu_state.r8  = ring3_context->r8;
    g_serverless_checkpoint.guest_cpu_state.r9  = ring3_context->r9;
    g_serverless_checkpoint.guest_cpu_state.r10 = ring3_context->r10;
    g_serverless_checkpoint.guest_cpu_state.r11 = ring3_context->r11;
    g_serverless_checkpoint.guest_cpu_state.r12 = ring3_context->r12;
    g_serverless_checkpoint.guest_cpu_state.r13 = ring3_context->r13;
    g_serverless_checkpoint.guest_cpu_state.r14 = ring3_context->r14;
    g_serverless_checkpoint.guest_cpu_state.r15 = ring3_context->r15;
    
    /* RIP (point to instruction after the syscall) */
    g_serverless_checkpoint.guest_cpu_state.rip = ring3_context->rip;
    g_serverless_checkpoint.guest_cpu_state.rflags = ring3_context->efl;
    
    /* Capture segment registers (ring-3 values) */
    g_serverless_checkpoint.guest_cpu_state.cs = ring3_context->csgsfsss;
    g_serverless_checkpoint.guest_cpu_state.ss = ring3_context->csgsfsss;
    g_serverless_checkpoint.guest_cpu_state.ds = ring3_context->csgsfsss;
    g_serverless_checkpoint.guest_cpu_state.es = ring3_context->csgsfsss;
    g_serverless_checkpoint.guest_cpu_state.fs = ring3_context->csgsfsss;
    g_serverless_checkpoint.guest_cpu_state.gs = ring3_context->csgsfsss;
    
    log_debug("Captured ring-3 CPU state (RIP: 0x%lx, RSP: 0x%lx, RFLAGS: 0x%lx)",
             g_serverless_checkpoint.guest_cpu_state.rip,
             g_serverless_checkpoint.guest_cpu_state.rsp,
             g_serverless_checkpoint.guest_cpu_state.rflags);
    
    return 0;
}

/*
 * Create checkpoint of ring-3 target program state
 * This should be called from the syscall handler with the ring-3 context
 */
int serverless_create_checkpoint_from_syscall(int checkpoint_point, PAL_CONTEXT* ring3_context) {
    if (g_serverless_checkpoint.checkpoint_created) {
        log_debug("Ring-3 program checkpoint already exists, skipping creation");
        return 0;
    }
    
    if (!g_serverless_checkpoint.enabled) {
        log_debug("Serverless checkpointing disabled");
        return 0;
    }
    
    log_debug("Creating ring-3 program checkpoint at point %d", checkpoint_point);
    
    /* Capture ring-3 CPU state from syscall context */
    int ret = capture_cpu_state_from_syscall_context(ring3_context);
    if (ret < 0) {
        log_error("Failed to capture ring-3 CPU state: %s", unix_strerror(ret));
        return ret;
    }
    
    /* Capture ring-3 program memory contents */
    ret = capture_memory_contents();
    if (ret < 0) {
        log_error("Failed to capture ring-3 memory: %s", unix_strerror(ret));
        return ret;
    }
    
    /* Mark checkpoint as created */
    g_serverless_checkpoint.checkpoint_created = true;
    
    log_debug("Ring-3 program checkpoint created successfully (%zu memory regions)", 
              g_serverless_checkpoint.num_regions);
    return 0;
}

/*
 * Wrapper for syscall handler - gets ring-3 context from TCB
 */
int serverless_create_checkpoint(int checkpoint_point) {
    /* Get ring-3 context from the current TCB */
    PAL_CONTEXT* ring3_context = LIBOS_TCB_GET(context.regs);
    if (!ring3_context) {
        log_error("Cannot access ring-3 CPU context from syscall");
        return -EFAULT;
    }
    
    return serverless_create_checkpoint_from_syscall(checkpoint_point, ring3_context);
}


/*
 * Wrapper for syscall handler - modifies ring-3 context in TCB to restore state
 */
int serverless_restore_checkpoint(void) {
    if (!g_serverless_checkpoint.checkpoint_created) {
        log_error("No ring-3 program checkpoint available");
        return -ENOENT;
    }
    
    log_debug("Restoring ring-3 program from checkpoint");
    
    /* Restore ring-3 program memory contents first */
    int ret = restore_memory_contents();
    if (ret < 0) {
        log_error("Failed to restore ring-3 memory: %s", unix_strerror(ret));
        return ret;
    }
    
    /* Get ring-3 context from the current TCB and modify it directly */
    /* This will change where the syscall returns to when it completes */
    PAL_CONTEXT* ring3_context = LIBOS_TCB_GET(context.regs);
    if (!ring3_context) {
        log_error("Cannot access ring-3 CPU context for restore");
        return -EFAULT;
    }
    
    /* Restore ring-3 CPU state by directly modifying the context */
    ring3_context->rax = g_serverless_checkpoint.guest_cpu_state.rax;
    ring3_context->rbx = g_serverless_checkpoint.guest_cpu_state.rbx;
    ring3_context->rcx = g_serverless_checkpoint.guest_cpu_state.rcx;
    ring3_context->rdx = g_serverless_checkpoint.guest_cpu_state.rdx;
    ring3_context->rsi = g_serverless_checkpoint.guest_cpu_state.rsi;
    ring3_context->rdi = g_serverless_checkpoint.guest_cpu_state.rdi;
    ring3_context->rbp = g_serverless_checkpoint.guest_cpu_state.rbp;
    ring3_context->rsp = g_serverless_checkpoint.guest_cpu_state.rsp;
    ring3_context->r8  = g_serverless_checkpoint.guest_cpu_state.r8;
    ring3_context->r9  = g_serverless_checkpoint.guest_cpu_state.r9;
    ring3_context->r10 = g_serverless_checkpoint.guest_cpu_state.r10;
    ring3_context->r11 = g_serverless_checkpoint.guest_cpu_state.r11;
    ring3_context->r12 = g_serverless_checkpoint.guest_cpu_state.r12;
    ring3_context->r13 = g_serverless_checkpoint.guest_cpu_state.r13;
    ring3_context->r14 = g_serverless_checkpoint.guest_cpu_state.r14;
    ring3_context->r15 = g_serverless_checkpoint.guest_cpu_state.r15;
    
    /* Most importantly, set RIP to the checkpointed instruction */
    /* This is where the syscall will return to */
    ring3_context->rip = g_serverless_checkpoint.guest_cpu_state.rip;
    ring3_context->efl = g_serverless_checkpoint.guest_cpu_state.rflags;
    
    /* Restore segment registers */
    ring3_context->csgsfsss = g_serverless_checkpoint.guest_cpu_state.cs;
    
    log_debug("Ring-3 program checkpoint restored - syscall will return to checkpointed state");
    log_debug("Restored RIP: 0x%lx, RSP: 0x%lx", 
             g_serverless_checkpoint.guest_cpu_state.rip,
             g_serverless_checkpoint.guest_cpu_state.rsp);
    
    /* Return success - the syscall return mechanism will now jump to checkpointed state */
    return 0;
}

/*
 * Check if checkpoint exists
 */
bool serverless_has_checkpoint(void) {
    return g_serverless_checkpoint.checkpoint_created;
}

/*
 * Clear checkpoint
 */
int serverless_clear_checkpoint(void) {
    /* Free memory region snapshots */
    if (g_serverless_checkpoint.memory_regions) {
        for (size_t i = 0; i < g_serverless_checkpoint.num_regions; i++) {
            free(g_serverless_checkpoint.memory_regions[i].snapshot_data);
        }
        free(g_serverless_checkpoint.memory_regions);
        g_serverless_checkpoint.memory_regions = NULL;
        g_serverless_checkpoint.num_regions = 0;
    }
    
    /* Reset state */
    bool enabled = g_serverless_checkpoint.enabled; /* Preserve enabled flag */
    memset(&g_serverless_checkpoint, 0, sizeof(g_serverless_checkpoint));
    g_serverless_checkpoint.enabled = enabled;
    
    log_debug("Ring-3 program checkpoint cleared");
    return 0;
}

/*
 * Initialize serverless checkpoint system.
 * Called during LibOS initialization.
 */
int init_serverless_checkpoint(void) {
    g_serverless_checkpoint.enabled = true;
    return 0;
}

/*
 * Hook into Python interpreter initialization.
 * This is called when Python modules are loaded and ready.
 */
void serverless_on_python_ready(void) {
    /* Trigger checkpoint creation after Python initialization */
    if (g_serverless_checkpoint.enabled) {
        serverless_create_checkpoint(CHECKPOINT_MODULES_LOADED);
    }
}

/*
 * Hook into function execution completion.
 * This is called after each serverless function completes.
 */
void serverless_on_function_complete(void) {
    /* Check if we should restore to checkpoint */
    /* Auto-restore functionality can be implemented later if needed */
    if (g_serverless_checkpoint.enabled) {
        log_debug("Function complete hook called");
    }
}
