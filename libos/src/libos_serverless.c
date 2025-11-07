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
#include "libos_checkpoint.h"
#include "libos_thread.h"
#include "libos_process.h"
#include "libos_handle.h"
#include "libos_ipc.h"
#include "libos_lock.h"
#include "libos_refcount.h"
#include "libos_table.h"
#include "pal.h"
#include "cpu.h"  /* for cli() and sti() functions */

// #define DEBUG_PRINT

/* Forward declarations for checkpoint structures */
struct memory_region_snapshot;

/* CPU state captured from ring-3 target program during syscall entry */
struct guest_cpu_context {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip;
    uint64_t rflags;
    
    /* Segment registers (ring-3 values) */
    uint64_t cs, ss, ds, es, fs, gs;
    
    /* TODO: reserved for FPU/SSE/AVX state */
    uint64_t reserved[8];   
};

/* Memory region snapshot metadata - used for both user and kernel memory */
struct memory_region_snapshot {
    void*    start_addr;        /* Virtual address of the region */
    size_t   size;              /* Size in bytes */
    void*    snapshot_data;     /* Pointer to snapshot storage in data area */
    int      protection;        /* PROT_* flags */
    uint32_t flags;            /* Additional flags (VMA flags for kernel regions) */
    char     comment[32];       /* Region description for debugging */
};

/* Ring-3 user program state */
struct ring3_state {
    struct guest_cpu_context cpu_context;              /* CPU registers and state */
    
    struct {
        struct memory_region_snapshot*  regions;         /* Array of memory region snapshots */
        size_t                          num_regions;     /* Number of memory regions */
        uint64_t                        total_size;      /* Total size of captured memory */
    } memory_mgmt;
    
    struct {
        uint64_t reserved[8];   /* Reserved for future ring-3 state */
    } reserved_state;
};

/* Ring-0 LibOS kernel state */
struct ring0_state {
    struct {
        struct memory_region_snapshot*       regions;      /* Array of kernel memory snapshots */
        size_t                               num_regions;  /* Number of kernel regions */
        uint64_t                             total_size;   /* Total kernel memory size */
    } memory_mgmt;
    
    /* Placeholder for future kernel subsystems */
    struct {
        uint64_t reserved[16];  /* Reserved for thread state */
    } thread_mgmt;
    
    struct {
        uint64_t reserved[16];  /* Reserved for filesystem state */
    } fs_mgmt;
    
    struct {
        uint64_t reserved[16];  /* Reserved for signal state */
    } signal_mgmt;
};

/* Main serverless checkpoint state structure */
struct serverless_checkpoint_state {
    struct {
        bool        checkpoint_created;     /* Whether checkpoint exists */
        bool        enabled;               /* Whether checkpointing is enabled */
        uint32_t    version;               /* Checkpoint format version */
        uint64_t    timestamp;             /* Creation timestamp */
        uint64_t    checksum;              /* Data integrity checksum */
        uint64_t    reserved[8];           /* Reserved for future metadata */
    } metadata;
    
    struct ring3_state   user_state;

    struct ring0_state   kernel_state;
};

/* Page-aligned checkpoint structure that occupies exactly one page */
#define SIZE_PAGE               (1 << 12)
#define SIZE_MB                 (1024 * 1024)
#define SERVERLESS_CR_SIZE      (30 * SIZE_MB) /* 15 MB for checkpoint data */

struct serverless_checkpoint_section {
    /* the first page contains metadata */
    struct serverless_checkpoint_state checkpoint_metadata;
    char __padding[SIZE_PAGE - sizeof(struct serverless_checkpoint_state)];
    /* the remain region contains data */
    char data[SERVERLESS_CR_SIZE];
} __attribute__((aligned(SIZE_PAGE), packed));

/* Compile-time check to ensure the section structure is exactly aligned */
_Static_assert(sizeof(struct serverless_checkpoint_section) == (SERVERLESS_CR_SIZE + SIZE_PAGE),
               "serverless_checkpoint_section must be exactly aligned to the size");

/* Static global checkpoint section - guaranteed to be page-aligned and occupy one full page */
static struct serverless_checkpoint_section g_serverless_checkpoint_section = {0};

/* Global serverless checkpoint state pointer - allocated in CR-aware memory */
/* IMPORTANT: This pointer itself is in normal memory, but points to CR-aware memory
 * to avoid self-modification issues during restore */
static struct serverless_checkpoint_state* g_serverless_checkpoint = &g_serverless_checkpoint_section.checkpoint_metadata;
static uint64_t g_checkpoint_base;
static uint64_t g_checkpoint_end;


/* Global variable to control which system_malloc to use */
bool g_use_cr_malloc = false;

/* Static allocator state - tracks current allocation offset within data region */
static size_t g_static_alloc_offset = 0;

/* Simple static allocator that allocates from g_serverless_checkpoint_section.data region */
static void* CR_malloc(size_t size) {
    /* Align size to 8-byte boundary for better alignment */
    size_t aligned_size = (size + 7) & ~7;
    
    /* Check if we have enough space in the data region */
    if (g_static_alloc_offset + aligned_size > SERVERLESS_CR_SIZE) {
        log_error("CR_malloc: Not enough space in static data region. Requested: %zu bytes, Available: %zu bytes", 
                  aligned_size, (size_t)SERVERLESS_CR_SIZE - g_static_alloc_offset);
        return NULL;
    }
    
    /* Get pointer to the allocated region */
    void* allocated_ptr = &g_serverless_checkpoint_section.data[g_static_alloc_offset];
    
    /* Advance the allocation offset */
    g_static_alloc_offset += aligned_size;
    return allocated_ptr;
}

/* Reset the static allocator to initial state */
static void CR_malloc_reset(void) {
    g_static_alloc_offset = 0;
    log_debug("CR_malloc_reset: Static allocator reset, available space: %zu bytes", (size_t)SERVERLESS_CR_SIZE);
}

/* Get current allocator statistics */
static void CR_malloc_stats(void) {
    log_always("CR_malloc Stats:");
    log_always("  Data region base: %p", g_serverless_checkpoint_section.data);
    log_always("  Total capacity: %zu bytes (%zu MB)", (size_t)SERVERLESS_CR_SIZE, (size_t)SERVERLESS_CR_SIZE / (1024 * 1024));
    log_always("  Current offset: %zu bytes", g_static_alloc_offset);
    log_always("  Used space: %zu bytes (%zu KB)", g_static_alloc_offset, g_static_alloc_offset / 1024);
    log_always("  Available space: %zu bytes (%zu KB)", 
              (size_t)SERVERLESS_CR_SIZE - g_static_alloc_offset, 
              ((size_t)SERVERLESS_CR_SIZE - g_static_alloc_offset) / 1024);
    log_always("  Usage percentage: %.2f%%", 
              (double)g_static_alloc_offset / (double)SERVERLESS_CR_SIZE * 100.0);
}

/* Print detailed PAL handle information */
static void print_pal_handle_info(const char* prefix, PAL_HANDLE handle) {
    if (!handle) {
        log_always("%s: handle=NULL", prefix);
        return;
    }

    log_always("%s: handle=%p. (detailed info cannot be accessed by LibOS)", prefix, handle);
}

/*
 * Calculate overlap between a VMA region and checkpoint section
 * Returns true if there's overlap and fills overlap information
 */
static bool calculate_overlap_with_checkpoint(void* vma_start, size_t vma_size,
                                            void** overlap_start, size_t* overlap_size) {
    uintptr_t vma_start_addr = (uintptr_t)vma_start;
    uintptr_t vma_end_addr = vma_start_addr + vma_size;
    uintptr_t checkpoint_start = g_checkpoint_base;
    uintptr_t checkpoint_end = g_checkpoint_end;
    
    /* Check if there's any overlap */
    if (vma_end_addr <= checkpoint_start || vma_start_addr >= checkpoint_end) {
        return false; /* No overlap */
    }
    
    /* Calculate overlap boundaries */
    uintptr_t overlap_start_addr = (vma_start_addr > checkpoint_start) ? vma_start_addr : checkpoint_start;
    uintptr_t overlap_end_addr = (vma_end_addr < checkpoint_end) ? vma_end_addr : checkpoint_end;
    
    *overlap_start = (void*)overlap_start_addr;
    *overlap_size = overlap_end_addr - overlap_start_addr;
    
    /* todo: debug log to print the overlap region */
    log_debug("VMA [%p - %p] overlaps with checkpoint section [%p - %p] at [%p - %p]",
              vma_start, (void*)vma_end_addr,
                (void*)checkpoint_start, (void*)checkpoint_end,
                (void*)overlap_start_addr, (void*)overlap_end_addr);
    return true;
}

/*
 * Capture both ring-3 user program memory and ring-0 kernel memory contents
 * Uses a unified approach to process all VMAs in one pass
 */
static int capture_all_memory_contents(void) {
    struct libos_vma_info* vmas;
    size_t vma_count;
    
    /* Get all VMAs including internal ones */
    int ret = dump_all_vmas_with_internal(&vmas, &vma_count);
    if (ret < 0) {
        log_error("Failed to dump VMAs with internal: %s", unix_strerror(ret));
        return ret;
    }
    
    /* Note: checkpoint region range calculation moved to kernel memory processing where needed */
    
    /* Count qualifying regions for both user and kernel memory */
    size_t user_regions = 0;
    size_t kernel_regions = 0;
    
    for (size_t i = 0; i < vma_count; i++) {
        struct libos_vma_info* vma = &vmas[i];
        
        /* Check for user memory regions */
        if (!(vma->flags & VMA_INTERNAL) && !(vma->flags & VMA_UNMAPPED) && (vma->prot & (PROT_READ | PROT_WRITE))) {
            user_regions++;
        }
        
        /* Check for kernel memory regions */
        if ((vma->flags & VMA_INTERNAL) && 
            (vma->prot & PROT_READ) && 
            (vma->prot & PROT_WRITE) && 
            (strcmp(vma->comment, "slab-CR") != 0) &&
            (strstr(vma->comment, "libos_stack") == NULL)) {
            
            /* Check if this VMA overlaps with checkpoint section */
            void* overlap_start;
            size_t overlap_size;
            bool has_overlap = calculate_overlap_with_checkpoint(vma->addr, vma->length, &overlap_start, &overlap_size);
            
            if (has_overlap) {
                /* Split region into non-overlapping parts */
                void* vma_start = vma->addr;
                void* vma_end = (char*)vma->addr + vma->length;
                void* overlap_end = (char*)overlap_start + overlap_size;
                
                /* Count regions before overlap */
                if (overlap_start > vma_start) {
                    kernel_regions++; /* Before overlap */
                }
                /* Count regions after overlap */
                if (overlap_end < vma_end) {
                    kernel_regions++; /* After overlap */
                }
                /* Note: we skip the overlapping part completely */
            } else {
                kernel_regions++; /* No overlap, count as single region */
            }
        }
    }
    
    log_debug("Found %zu user regions and %zu kernel regions to capture", user_regions, kernel_regions);
    
    /* Allocate arrays for both user and kernel regions */
    if (user_regions > 0) {
        g_serverless_checkpoint->user_state.memory_mgmt.regions = 
            CR_malloc(sizeof(struct memory_region_snapshot) * user_regions);
        if (!g_serverless_checkpoint->user_state.memory_mgmt.regions) {
            free_vma_info_array(vmas, vma_count);
            return -ENOMEM;
        }
    }
    
    if (kernel_regions > 0) {
        g_serverless_checkpoint->kernel_state.memory_mgmt.regions = 
            CR_malloc(sizeof(struct memory_region_snapshot) * kernel_regions);
        if (!g_serverless_checkpoint->kernel_state.memory_mgmt.regions) {
            if (user_regions > 0) {
                free(g_serverless_checkpoint->user_state.memory_mgmt.regions);
            }
            free_vma_info_array(vmas, vma_count);
            return -ENOMEM;
        }
    }
    
    /* Capture memory contents in one pass through all VMAs */
    size_t user_idx = 0;
    size_t kernel_idx = 0;
    uint64_t total_user_memory = 0;
    uint64_t total_kernel_memory = 0;
    
    for (size_t i = 0; i < vma_count; i++) {
        struct libos_vma_info* vma = &vmas[i];
        
        /* Process user memory regions */
        if (!(vma->flags & VMA_INTERNAL) && !(vma->flags & VMA_UNMAPPED) && (vma->prot & (PROT_READ | PROT_WRITE))) {
            struct memory_region_snapshot* region = &g_serverless_checkpoint->user_state.memory_mgmt.regions[user_idx];
            
            region->start_addr = vma->addr;
            region->size = vma->length;
            region->protection = vma->prot;
            region->flags = vma->flags;
            size_t comment_len = strlen(vma->comment);
            size_t copy_len = (comment_len < sizeof(region->comment) - 1) ? comment_len : sizeof(region->comment) - 1;
            memcpy(region->comment, vma->comment, copy_len);
            region->comment[copy_len] = '\0';
            
            /* Allocate snapshot memory */
            region->snapshot_data = CR_malloc(region->size);
            if (!region->snapshot_data) {
                /* Cleanup on failure */
                for (size_t j = 0; j < user_idx; j++) {
                    free(g_serverless_checkpoint->user_state.memory_mgmt.regions[j].snapshot_data);
                }
                for (size_t j = 0; j < kernel_idx; j++) {
                    free(g_serverless_checkpoint->kernel_state.memory_mgmt.regions[j].snapshot_data);
                }
                free_vma_info_array(vmas, vma_count);
                return -ENOMEM;
            }
            
            /* Copy memory content directly (no overlap to skip) */
            memcpy(region->snapshot_data, region->start_addr, region->size);
            total_user_memory += region->size;
            
            log_debug("Captured ring-3 memory region %p-%p (%zu bytes) [%s]", 
                     region->start_addr, 
                     (char*)region->start_addr + region->size, 
                     region->size, 
                     region->comment);
            
            user_idx++;
        }
        
        /* Process kernel memory regions */
        else if ((vma->flags & VMA_INTERNAL) && 
                 (vma->prot & PROT_READ) && 
                 (vma->prot & PROT_WRITE) && 
                 (strcmp(vma->comment, "slab-CR") != 0) &&
                 (strstr(vma->comment, "libos_stack") == NULL)) {
            
            /* Check for overlap with checkpoint section */
            void* overlap_start;
            size_t overlap_size;
            bool has_overlap = calculate_overlap_with_checkpoint(vma->addr, vma->length, &overlap_start, &overlap_size);
            
            if (has_overlap) {
                /* Split VMA into non-overlapping regions, skipping checkpoint section completely */
                void* vma_start = vma->addr;
                void* vma_end = (char*)vma->addr + vma->length;
                void* overlap_end = (char*)overlap_start + overlap_size;
                
                /* Process region before overlap */
                if (overlap_start > vma_start) {
                    struct memory_region_snapshot* region = &g_serverless_checkpoint->kernel_state.memory_mgmt.regions[kernel_idx];
                    
                    region->start_addr = vma_start;
                    region->size = (char*)overlap_start - (char*)vma_start;
                    region->protection = vma->prot;
                    region->flags = vma->flags;
                    size_t comment_len = strlen(vma->comment);
                    size_t copy_len = (comment_len < sizeof(region->comment) - 1) ? comment_len : sizeof(region->comment) - 1;
                    memcpy(region->comment, vma->comment, copy_len);
                    region->comment[copy_len] = '\0';
                    
                    /* Allocate and copy memory */
                    region->snapshot_data = CR_malloc(region->size);
                    if (!region->snapshot_data) {
                        /* Cleanup on failure */
                        for (size_t j = 0; j < user_idx; j++) {
                            free(g_serverless_checkpoint->user_state.memory_mgmt.regions[j].snapshot_data);
                        }
                        for (size_t j = 0; j < kernel_idx; j++) {
                            free(g_serverless_checkpoint->kernel_state.memory_mgmt.regions[j].snapshot_data);
                        }
                        free_vma_info_array(vmas, vma_count);
                        return -ENOMEM;
                    }
                    
                    memcpy(region->snapshot_data, region->start_addr, region->size);
                    total_kernel_memory += region->size;
                    
                    log_debug("Captured ring-0 kernel memory region %p-%p (%zu bytes) [%s] - BEFORE overlap", 
                             region->start_addr, 
                             (char*)region->start_addr + region->size, 
                             region->size, 
                             region->comment);
                    
                    kernel_idx++;
                }
                
                /* Skip the overlapping checkpoint section completely */
                log_debug("Skipping overlapping checkpoint section %p-%p (%zu bytes) in VMA [%s]", 
                         overlap_start, overlap_end, overlap_size, vma->comment);
                
                /* Process region after overlap */
                if (overlap_end < vma_end) {
                    struct memory_region_snapshot* region = &g_serverless_checkpoint->kernel_state.memory_mgmt.regions[kernel_idx];
                    
                    region->start_addr = overlap_end;
                    region->size = (char*)vma_end - (char*)overlap_end;
                    region->protection = vma->prot;
                    region->flags = vma->flags;
                    size_t comment_len = strlen(vma->comment);
                    size_t copy_len = (comment_len < sizeof(region->comment) - 1) ? comment_len : sizeof(region->comment) - 1;
                    memcpy(region->comment, vma->comment, copy_len);
                    region->comment[copy_len] = '\0';
                    
                    /* Allocate and copy memory */
                    region->snapshot_data = CR_malloc(region->size);
                    if (!region->snapshot_data) {
                        /* Cleanup on failure */
                        for (size_t j = 0; j < user_idx; j++) {
                            free(g_serverless_checkpoint->user_state.memory_mgmt.regions[j].snapshot_data);
                        }
                        for (size_t j = 0; j < kernel_idx; j++) {
                            free(g_serverless_checkpoint->kernel_state.memory_mgmt.regions[j].snapshot_data);
                        }
                        free_vma_info_array(vmas, vma_count);
                        return -ENOMEM;
                    }
                    
                    memcpy(region->snapshot_data, region->start_addr, region->size);
                    total_kernel_memory += region->size;
                    
                    log_debug("Captured ring-0 kernel memory region %p-%p (%zu bytes) [%s] - AFTER overlap", 
                             region->start_addr, 
                             (char*)region->start_addr + region->size, 
                             region->size, 
                             region->comment);
                    
                    kernel_idx++;
                }
            } else {
                /* No overlap, process as single region */
                struct memory_region_snapshot* region = &g_serverless_checkpoint->kernel_state.memory_mgmt.regions[kernel_idx];
                
                region->start_addr = vma->addr;
                region->size = vma->length;
                region->protection = vma->prot;
                region->flags = vma->flags;
                size_t comment_len = strlen(vma->comment);
                size_t copy_len = (comment_len < sizeof(region->comment) - 1) ? comment_len : sizeof(region->comment) - 1;
                memcpy(region->comment, vma->comment, copy_len);
                region->comment[copy_len] = '\0';
                
                /* Allocate and copy memory */
                region->snapshot_data = CR_malloc(region->size);
                if (!region->snapshot_data) {
                    /* Cleanup on failure */
                    for (size_t j = 0; j < user_idx; j++) {
                        free(g_serverless_checkpoint->user_state.memory_mgmt.regions[j].snapshot_data);
                    }
                    for (size_t j = 0; j < kernel_idx; j++) {
                        free(g_serverless_checkpoint->kernel_state.memory_mgmt.regions[j].snapshot_data);
                    }
                    free_vma_info_array(vmas, vma_count);
                    return -ENOMEM;
                }
                
                memcpy(region->snapshot_data, region->start_addr, region->size);
                total_kernel_memory += region->size;
                
                log_debug("Captured ring-0 kernel memory region %p-%p (%zu bytes) [%s]", 
                         region->start_addr, 
                         (char*)region->start_addr + region->size, 
                         region->size, 
                         region->comment);
                
                kernel_idx++;
            }
        }
    }
    
    /* Update counters and totals */
    g_serverless_checkpoint->user_state.memory_mgmt.num_regions = user_regions;
    g_serverless_checkpoint->user_state.memory_mgmt.total_size = total_user_memory;
    g_serverless_checkpoint->kernel_state.memory_mgmt.num_regions = kernel_regions;
    g_serverless_checkpoint->kernel_state.memory_mgmt.total_size = total_kernel_memory;
    
    log_always("Captured %zu ring-3 program memory regions (total: %zu KB / %zu MB)", 
              user_regions, total_user_memory / 1024, total_user_memory / (1024 * 1024));
    log_always("Captured %zu ring-0 kernel memory regions (total: %zu KB / %zu MB)", 
              kernel_regions, total_kernel_memory / 1024, total_kernel_memory / (1024 * 1024));
    
    free_vma_info_array(vmas, vma_count);
    return 0;
}

/*
 * Restore both ring-3 user program memory and ring-0 kernel memory contents from checkpoint
 * Uses unified approach: first restore ring-3 memory, then ring-0 memory
 */
static int restore_all_memory_contents(void) {
    /* First restore ring-3 program memory */
    if (g_serverless_checkpoint->user_state.memory_mgmt.regions) {
        for (size_t i = 0; i < g_serverless_checkpoint->user_state.memory_mgmt.num_regions; i++) {
            struct memory_region_snapshot* region = &g_serverless_checkpoint->user_state.memory_mgmt.regions[i];
            
            log_debug("Restoring ring-3 memory region %p-%p (%zu bytes) [%s]",
                     region->start_addr, 
                     (char*)region->start_addr + region->size, 
                     region->size,
                     region->comment);
            
            /* Restore user memory directly (no overlap possible) */
            memcpy(region->start_addr, region->snapshot_data, region->size);
        }
        log_debug("Restored %zu ring-3 program memory regions", g_serverless_checkpoint->user_state.memory_mgmt.num_regions);
    } else {
        log_debug("No ring-3 memory snapshots to restore");
    }
    
#if 1
    /* Then restore ring-0 kernel memory */
    if (g_serverless_checkpoint->kernel_state.memory_mgmt.regions) {
        size_t region_count = g_serverless_checkpoint->kernel_state.memory_mgmt.num_regions;
        size_t total_restored_memory = 0;
        
        for (size_t i = 0; i < region_count; i++) {
            struct memory_region_snapshot* region = &g_serverless_checkpoint->kernel_state.memory_mgmt.regions[i];
            
            log_debug("Restoring ring-0 kernel memory region %p-%p (%zu bytes) [%s]",
                     region->start_addr, 
                     (char*)region->start_addr + region->size, 
                     region->size,
                     region->comment);
            
            /* Restore memory directly (no overlap since regions were split during capture) */
            memcpy(region->start_addr, region->snapshot_data, region->size);
            total_restored_memory += region->size;
        }
        
        log_always("Restored %zu ring-0 kernel memory regions (total: %zu KB / %zu MB)", 
                  region_count, total_restored_memory / 1024, total_restored_memory / (1024 * 1024));
    } else {
        log_debug("No ring-0 kernel memory snapshots to restore");
    }
#endif
    return 0;
}

/*
 * Capture ring-3 target program CPU state from syscall context
 * This should be called when the syscall handler has the ring-3 state saved
 */

/* Callback function to print detailed information about a single thread */
__attribute__((unused)) static int print_single_thread_info(struct libos_thread* thread, void* arg) {
    __UNUSED(arg);
    struct libos_thread* cur = get_cur_thread();
    bool is_current = (thread == cur);
    
    log_always("Thread %p (TID=%d)%s Complete State:", 
              thread, thread->tid, is_current ? " [CURRENT]" : "");
    log_always("  Identity: UID/GID=%d/%d, EUID/EGID=%d/%d, SUID/SGID=%d/%d", 
              thread->uid, thread->gid, thread->euid, thread->egid, thread->suid, thread->sgid);
    
    /* Print groups */
    log_always("  Groups: count=%zu", thread->groups_info.count);
    for (size_t i = 0; i < thread->groups_info.count && i < 8; i++) {
        log_always("    Group[%zu]: %d", i, thread->groups_info.groups[i]);
    }
    
    log_always("  Thread State: time_to_die=%s", thread->time_to_die ? "YES" : "NO");
    log_always("  Signal mask: 0x%016lx", *(unsigned long*)&thread->signal_mask);
    log_always("  Memory: stack=%p-%p, libos_stack_bottom=%p", 
              thread->stack, thread->stack_top, thread->libos_stack_bottom);
    log_always("  RefCount: %ld", refcount_get(&thread->ref_count));
    
    /* Print child TID pointers */
    log_always("  Child TID pointers: set_child_tid=%p, clear_child_tid=%p, clear_child_tid_pal=%d",
              thread->set_child_tid, thread->clear_child_tid, thread->clear_child_tid_pal);
    
    /* Print thread's PAL handle */
    print_pal_handle_info("  PAL Thread Handle", thread->pal_handle);
    
    /* Print thread's LibOS TCB */
    if (thread->libos_tcb) {
        log_always("  LibOS TCB: %p", thread->libos_tcb);
        log_always("    TCB self pointer: %p", thread->libos_tcb->self);
        log_always("    Syscall entry: %p", thread->libos_tcb->libos_syscall_entry);
        log_always("    Current syscall: %ld", thread->libos_tcb->context.syscall_nr);
        log_always("    TLS: 0x%lx", thread->libos_tcb->context.tls);
        if (thread->libos_tcb->context.regs) {
            log_always("    PAL Context: present (%p)", thread->libos_tcb->context.regs);
        } else {
            log_always("    PAL Context: NULL");
        }
    } else {
        log_always("  LibOS TCB: NULL");
    }
    
    /* Print thread's complete handle map and ALL file descriptors */
    if (thread->handle_map) {
        log_always("  Handle Map: fd_size=%u, fd_top=%u, refcount=%ld", 
                  thread->handle_map->fd_size, thread->handle_map->fd_top,
                  refcount_get(&thread->handle_map->ref_count));
        
        uint32_t active_fds = 0;
        log_always("  All File Descriptors:");
        for (uint32_t fd = 0; fd < thread->handle_map->fd_top; fd++) {
            if (thread->handle_map->map[fd] && HANDLE_ALLOCATED(thread->handle_map->map[fd])) {
                struct libos_handle* hdl = thread->handle_map->map[fd]->handle;
                if (hdl) {
                    active_fds++;
                    log_always("    FD %u: type=%d, flags=0x%x, refcount=%ld", 
                              fd, hdl->type, hdl->flags, refcount_get(&hdl->ref_count));
                    log_always("      URI: %s", hdl->uri ?: "<no-uri>");
                    
                    /* Print corresponding PAL handle with detailed info */
                    print_pal_handle_info("      PAL Handle", hdl->pal_handle);
                }
            }
        }
        log_always("  Total Active FDs: %u", active_fds);
    } else {
        log_always("  Handle Map: NULL");
    }
    
    /* Print signal-related state */
    if (thread->signal_dispositions) {
        log_always("  Signal Dispositions: present (refcount=%ld)",
                  refcount_get(&thread->signal_dispositions->ref_count));
    } else {
        log_always("  Signal Dispositions: NULL");
    }
    
    log_always("  Signal Queue: pending_signals=0x%016lx", thread->pending_signals);
    log_always("  Signal Altstack: present=%s", thread->signal_altstack.ss_sp ? "YES" : "NO");
    
    /* Print scheduler and sync objects */
    if (thread->scheduler_event) {
        log_always("  Scheduler Event: present (%p)", thread->scheduler_event);
    } else {
        log_always("  Scheduler Event: NULL");
    }
    
    /* Print other thread properties */
    log_always("  Robust List: %p", thread->robust_list);
    log_always("  Frameptr: %p", thread->frameptr);
    
    /* Print CPU affinity */
    if (thread->cpu_affinity_mask) {
        log_always("  CPU Affinity: present (mask=%p)", thread->cpu_affinity_mask);
    } else {
        log_always("  CPU Affinity: NULL");
    }
    
    log_always("--- End Thread %p ---", thread);
    return 1; /* Continue walking */
}

/* Structure to count threads */
struct thread_count_arg {
    size_t count;
};

/* Callback function to count threads */
__attribute__((unused)) static int count_threads_callback(struct libos_thread* thread, void* arg) {
    __UNUSED(thread);
    struct thread_count_arg* count_arg = (struct thread_count_arg*)arg;
    count_arg->count++;
    return 1; /* Continue walking */
}

/* Print all threads and their complete state including PAL handles */
__attribute__((unused)) static void print_all_threads_state(void) {
    log_always("=== Complete LibOS System Threads State ===");
    
    /* First, count total threads */
    struct thread_count_arg count_arg = {0};
    int ret = walk_thread_list(count_threads_callback, &count_arg, /*one_shot=*/false);
    if (ret < 0) {
        log_always("Failed to count threads: %s", unix_strerror(ret));
        return;
    }
    
    /* Print current thread info */
    struct libos_thread* cur = get_cur_thread();
    if (cur) {
        log_always("Current Active Thread: %p (TID=%d)", cur, cur->tid);
    } else {
        log_always("ERROR: No current thread!");
        return;
    }
    
    log_always("Total System Threads: %zu", count_arg.count);
    log_always("---");
    
    /* Print detailed information for all threads */
    ret = walk_thread_list(print_single_thread_info, NULL, /*one_shot=*/false);
    if (ret < 0) {
        log_always("Failed to walk thread list: %s", unix_strerror(ret));
        return;
    }
    
    log_always("=== End Complete System Threads State ===");
}

/* Print LibOS memory management state */
__attribute__((unused)) static void print_memory_management_state(void) {
    log_always("=== LibOS Memory Management State ===");
    
    struct libos_vma_info* vmas = NULL;
    size_t vma_count = 0;
    
    int ret = dump_all_vmas_with_internal(&vmas, &vma_count);
    if (ret < 0) {
        log_always("Failed to dump VMAs: %s", unix_strerror(ret));
        return;
    }
    
    size_t internal_count = 0;
    size_t user_count = 0;
    size_t unmapped_count = 0;
    size_t total_memory = 0;
    
    log_always("All VMAs:");
    for (size_t i = 0; i < vma_count; i++) {
        struct libos_vma_info* vma = &vmas[i];
        const char* type_str = "USER";
        
        if (vma->flags & VMA_INTERNAL) {
            type_str = "INTERNAL";
            internal_count++;
        } else if (vma->flags & VMA_UNMAPPED) {
            type_str = "UNMAPPED";
            unmapped_count++;
        } else {
            user_count++;
        }
        
        total_memory += vma->length;
        
        log_always("  VMA %zu: [0x%lx-0x%lx] len=0x%lx prot=0x%x flags=0x%x type=%s comment=%s",
                  i + 1, (uintptr_t)vma->addr, (uintptr_t)vma->addr + vma->length,
                  vma->length, vma->prot, vma->flags, type_str, vma->comment);
        
        /* Print associated file handle and PAL handle */
        if (vma->file) {
            log_always("    File: %s, offset=0x%lx, refcount=%ld", 
                      vma->file->uri ?: "<unknown>", vma->file_offset,
                      refcount_get(&vma->file->ref_count));
            print_pal_handle_info("    File PAL Handle", vma->file->pal_handle);
        }
    }
    
    /* Calculate additional statistics */
    size_t user_size = 0, internal_size = 0, unmapped_size = 0;
    size_t writable_user_count = 0, writable_internal_count = 0;
    size_t writable_user_size = 0, writable_internal_size = 0;
    
    /* Detailed kernel memory breakdown */
    size_t libos_kernel_count = 0, libos_kernel_size = 0;
    size_t pal_kernel_count = 0, pal_kernel_size = 0;
    size_t writable_libos_kernel_count = 0, writable_libos_kernel_size = 0;
    size_t writable_pal_kernel_count = 0, writable_pal_kernel_size = 0;
    
    for (size_t i = 0; i < vma_count; i++) {
        struct libos_vma_info* vma = &vmas[i];
        
        if (vma->flags & VMA_INTERNAL) {
            internal_size += vma->length;
            if (vma->prot & PROT_WRITE) {
                writable_internal_count++;
                writable_internal_size += vma->length;
            }
            
            /* Classify kernel memory by comment */
            if (strstr(vma->comment, "pal internal") || strstr(vma->comment, "PAL internal")) {
                pal_kernel_count++;
                pal_kernel_size += vma->length;
                if (vma->prot & PROT_WRITE) {
                    writable_pal_kernel_count++;
                    writable_pal_kernel_size += vma->length;
                }
            } else {
                /* LibOS kernel memory (slab, vma, etc.) */
                if (strstr(vma->comment, "slab-CR")) {
                    /* Skip slab-CR memory from counting */
                    continue;
                }
                libos_kernel_count++;
                libos_kernel_size += vma->length;
                if (vma->prot & PROT_WRITE) {
                    writable_libos_kernel_count++;
                    writable_libos_kernel_size += vma->length;
                }
            }
        } else if (vma->flags & VMA_UNMAPPED) {
            unmapped_size += vma->length;
        } else {
            user_size += vma->length;
            if (vma->prot & PROT_WRITE) {
                writable_user_count++;
                writable_user_size += vma->length;
            }
        }
    }
    
    log_always("VMA Summary:");
    log_always("  Total VMAs: %zu", vma_count);
    log_always("  Total User VMAs: %zu (size: %zu KB / %zu MB)", user_count, user_size / 1024, user_size / (1024 * 1024));
    log_always("  Total Kernel VMAs: %zu (size: %zu KB / %zu MB)", internal_count, internal_size / 1024, internal_size / (1024 * 1024));
    log_always("  Total Unmapped VMAs: %zu (size: %zu KB / %zu MB)", unmapped_count, unmapped_size / 1024, unmapped_size / (1024 * 1024));
    log_always("  Total Writable User VMAs: %zu (size: %zu KB / %zu MB)", writable_user_count, writable_user_size / 1024, writable_user_size / (1024 * 1024));
    log_always("  Total Writable Kernel VMAs: %zu (size: %zu KB / %zu MB)", writable_internal_count, writable_internal_size / 1024, writable_internal_size / (1024 * 1024));
    log_always("Detailed Kernel Memory Breakdown:");
    log_always("  Total Kernel (LibOS) VMAs: %zu (size: %zu KB / %zu MB)", libos_kernel_count, libos_kernel_size / 1024, libos_kernel_size / (1024 * 1024));
    log_always("  Total Kernel (PAL) VMAs: %zu (size: %zu KB / %zu MB)", pal_kernel_count, pal_kernel_size / 1024, pal_kernel_size / (1024 * 1024));
    log_always("  Total Writable Kernel (LibOS) VMAs: %zu (size: %zu KB / %zu MB)", writable_libos_kernel_count, writable_libos_kernel_size / 1024, writable_libos_kernel_size / (1024 * 1024));
    log_always("  Total Writable Kernel (PAL) VMAs: %zu (size: %zu KB / %zu MB)", writable_pal_kernel_count, writable_pal_kernel_size / 1024, writable_pal_kernel_size / (1024 * 1024));
    log_always("Total Memory Summary:");
    log_always("  Total Memory: 0x%lx (%zu KB / %zu MB)", total_memory, total_memory / 1024, total_memory / (1024 * 1024));

    if (vmas) {
        free_vma_info_array(vmas, vma_count);
    }
}

/* Print LibOS process state */
__attribute__((unused)) static void print_process_state(void) {
    log_always("=== LibOS Process State ===");
    log_always("Process Descriptor:");
    log_always("  PID: %d", g_process.pid);
    log_always("  PPID: %d", g_process.ppid);
    log_always("  PGID: %d", g_process.pgid);
    log_always("  SID: %d", g_process.sid);
    log_always("  Process group leader: %s", g_process.pid == g_process.pgid ? "YES" : "NO");
    log_always("  Session leader: %s", g_process.pid == g_process.sid ? "YES" : "NO");
    
    /* Print working directory and root */
    if (g_process.cwd) {
        log_always("  Current Working Directory: [dentry present]");
    }
    if (g_process.root) {
        log_always("  Root Directory: [dentry present]");
    }
    
    /* Print executable information */
    if (g_process.exec) {
        log_always("  Executable: %s", g_process.exec->uri ?: "<unknown>");
        print_pal_handle_info("  Executable PAL Handle", g_process.exec->pal_handle);
    }
}

/* Print LibOS filesystem state */
__attribute__((unused)) static void print_filesystem_state(void) {
    log_always("=== LibOS Filesystem State ===");
    
    /* Print mount information */
    log_always("Mount Information:");
    log_always("  Root dentry configured");
    
    /* Print dentry cache statistics if available */
    log_always("  Dentry cache active");
    
    /* TODO: Add more detailed filesystem state printing */
    log_always("  Filesystem state tracking active");
}

/* Print LibOS signal state */
__attribute__((unused)) static void print_signal_state(void) {
    log_always("=== LibOS Signal State ===");
    
    struct libos_thread* cur = get_cur_thread();
    if (cur && cur->signal_dispositions) {
        log_always("Signal Dispositions:");
        log_always("  Process signal dispositions configured");
        log_always("  Per-thread signal state active");
        
        /* Print current thread's signal state */
        log_always("Current Thread Signal State:");
        log_always("  Signal mask: [present]");
        log_always("  Saved sigmask: [present] (has_saved: %s)", 
                  cur->has_saved_sigmask ? "YES" : "NO");
        log_always("  Pending signals count: %lu", cur->pending_signals);
        
        /* Print signal altstack */
        log_always("  Signal altstack: %p (size: %zu, flags: 0x%x)",
                  cur->signal_altstack.ss_sp, cur->signal_altstack.ss_size,
                  cur->signal_altstack.ss_flags);
    }
}

/* Print LibOS IPC state */
__attribute__((unused)) static void print_ipc_state(void) {
    log_always("=== LibOS IPC State ===");
    log_always("IPC Namespace:");
    log_always("  IPC namespace initialized");
    log_always("  Process communication active");
    
    /* TODO: Add more detailed IPC state printing when available */
}

/* Print system information obtained from PAL */
__attribute__((unused)) static void print_system_info_from_pal(void) {
    log_always("=== System Information from PAL ===");
    
    /* Get vCPU count */
    uint32_t vcpu_count = PalServerlessGetVcpuCount();
    log_always("vCPU Count: %u", vcpu_count);
    
    /* Get CPU topology */
    size_t threads_cnt = 0, cores_cnt = 0, sockets_cnt = 0;
    int ret = PalServerlessGetCpuTopology(&threads_cnt, &cores_cnt, &sockets_cnt);
    if (ret == 0) {
        log_always("CPU Topology:");
        log_always("  Hardware Threads: %zu", threads_cnt);
        log_always("  CPU Cores: %zu", cores_cnt);
        log_always("  CPU Sockets: %zu", sockets_cnt);
        
        /* Check which CPUs are online */
        log_always("  Online CPUs: ");
        for (size_t i = 0; i < threads_cnt && i < 16; i++) { /* Limit output to first 16 */
            if (PalServerlessIsCpuOnline(i)) {
                log_always("    CPU %zu: online", i);
            } else {
                log_always("    CPU %zu: offline", i);
            }
        }
        if (threads_cnt > 16) {
            log_always("    ... (showing first 16 of %zu total)", threads_cnt);
        }
    } else {
        log_always("Failed to get CPU topology: %s", unix_strerror(ret));
    }
    
    /* Get memory information */
    size_t mem_total = PalServerlessGetMemTotal();
    if (mem_total > 0) {
        log_always("Total Memory: %zu bytes (%zu MB)", mem_total, mem_total / (1024 * 1024));
    } else {
        log_always("Total Memory: unavailable");
    }
    
    /* Get host type */
    const char* host_type = PalServerlessGetHostType();
    if (host_type) {
        log_always("Host Type: %s", host_type);
    } else {
        log_always("Host Type: unavailable");
    }
    
    /* Get PAL internal thread count */
    size_t pal_thread_count = PalServerlessGetPalThreadCount();
    log_always("PAL Internal Thread Count: %zu", pal_thread_count);
    
    log_always("=== End System Information from PAL ===");
}

/* Main system state printing function */
__attribute__((unused)) static void print_system_state(const char* phase) {
#ifdef DEBUG_PRINT
    log_always("==========================================");
    log_always("=== %s: Complete LibOS Kernel State ===", phase);
    log_always("==========================================");

    /* states from PAL */
    print_system_info_from_pal();
    
    /* Print all subsystem states */
    print_all_threads_state();
    print_memory_management_state(); 
    print_process_state();
    print_filesystem_state();
    print_signal_state();
    print_ipc_state();
    
    log_always("==========================================");
    log_always("=== End %s: LibOS Kernel State ===", phase);
    log_always("==========================================");
#else
    __UNUSED(phase);
#endif
}

static int capture_cpu_state_from_syscall_context(struct libos_context* context) {
    if (!context) {
        log_error("No ring-3 context provided for checkpoint");
        return -EINVAL;
    }
    
    /* Extract ring-3 CPU state from syscall context */
    PAL_CONTEXT* regs = context->regs;
    if (!regs) {
        log_error("No PAL context in syscall context");
        return -EINVAL;
    }
    
    g_serverless_checkpoint->user_state.cpu_context.rax = regs->rax;
    g_serverless_checkpoint->user_state.cpu_context.rbx = regs->rbx;
    g_serverless_checkpoint->user_state.cpu_context.rcx = regs->rcx;
    g_serverless_checkpoint->user_state.cpu_context.rdx = regs->rdx;
    g_serverless_checkpoint->user_state.cpu_context.rsi = regs->rsi;
    g_serverless_checkpoint->user_state.cpu_context.rdi = regs->rdi;
    g_serverless_checkpoint->user_state.cpu_context.rbp = regs->rbp;
    g_serverless_checkpoint->user_state.cpu_context.rsp = regs->rsp;
    g_serverless_checkpoint->user_state.cpu_context.r8  = regs->r8;
    g_serverless_checkpoint->user_state.cpu_context.r9  = regs->r9;
    g_serverless_checkpoint->user_state.cpu_context.r10 = regs->r10;
    g_serverless_checkpoint->user_state.cpu_context.r11 = regs->r11;
    g_serverless_checkpoint->user_state.cpu_context.r12 = regs->r12;
    g_serverless_checkpoint->user_state.cpu_context.r13 = regs->r13;
    g_serverless_checkpoint->user_state.cpu_context.r14 = regs->r14;
    g_serverless_checkpoint->user_state.cpu_context.r15 = regs->r15;
    
    /* RIP (point to instruction after the syscall) */
    g_serverless_checkpoint->user_state.cpu_context.rip = regs->rip;
    g_serverless_checkpoint->user_state.cpu_context.rflags = regs->efl;
    
    /* Capture segment registers (ring-3 values) */
    g_serverless_checkpoint->user_state.cpu_context.cs = regs->csgsfsss;
    g_serverless_checkpoint->user_state.cpu_context.ss = regs->csgsfsss;
    g_serverless_checkpoint->user_state.cpu_context.ds = regs->csgsfsss;
    g_serverless_checkpoint->user_state.cpu_context.es = regs->csgsfsss;
    g_serverless_checkpoint->user_state.cpu_context.fs = regs->csgsfsss;
    g_serverless_checkpoint->user_state.cpu_context.gs = regs->csgsfsss;
    
    log_debug("Captured ring-3 CPU state (RIP: 0x%lx, RSP: 0x%lx, RFLAGS: 0x%lx)",
             g_serverless_checkpoint->user_state.cpu_context.rip,
             g_serverless_checkpoint->user_state.cpu_context.rsp,
             g_serverless_checkpoint->user_state.cpu_context.rflags);
    
    return 0;
}

/*
 * Create checkpoint of ring-3 target program state
 * This should be called from the syscall handler with the ring-3 context
 */
int serverless_create_checkpoint_from_syscall(int checkpoint_point, PAL_CONTEXT* ring3_context) {
    if (g_serverless_checkpoint->metadata.checkpoint_created) {
        log_debug("Ring-3 program checkpoint already exists, skipping creation");
        return 0;
    }

    log_always("g_serverless_checkpoint address = %p, enabled=%d", g_serverless_checkpoint, g_serverless_checkpoint->metadata.enabled);
    
    if (!g_serverless_checkpoint->metadata.enabled) {
        log_debug("Serverless checkpointing disabled");
        return 0;
    }

    log_always("=== Starting: Checkpoint Creation (point=%d) ===", checkpoint_point);
    
    /* 
     * CRITICAL: Disable interrupts during entire checkpoint process to prevent thread preemption
     * This ensures no other threads (IPC, idle, bottomhalves) can interfere with our memory capture
     */
    cli(); /* Disable interrupts to prevent preemption */
    /* Acquire checkpoint barrier on all CPUs to synchronize multi-core checkpoint operation */
    PalServerlessCheckpointBarrierAcquire();
    
    /* Print complete LibOS kernel state before checkpoint */
    print_system_state("PRE-CHECKPOINT");
    
    /* Create libos_context wrapper for ring3_context */
    struct libos_context context_wrapper = {
        .regs = ring3_context,
        .syscall_nr = -1,  /* Not a real syscall */
        .tls = 0
    };
    
    /* Capture ring-3 CPU state from syscall context */
    int ret = capture_cpu_state_from_syscall_context(&context_wrapper);
    if (ret < 0) {
        log_error("Failed to capture ring-3 CPU state: %s", unix_strerror(ret));
        sti(); /* Re-enable interrupts before returning */
        PalServerlessCheckpointBarrierRelease(); /* Release barrier on failure */
        return ret;
    }
    
    /* Capture both ring-3 user memory and ring-0 kernel memory in one unified pass */
    ret = capture_all_memory_contents();
    if (ret < 0) {
        log_error("Failed to capture memory contents: %s", unix_strerror(ret));
        sti(); /* Re-enable interrupts before returning */
        PalServerlessCheckpointBarrierRelease(); /* Release barrier on failure */
        return ret;
    }
    
    /* Mark checkpoint as created */
    g_serverless_checkpoint->metadata.checkpoint_created = true;
    
    /* Release checkpoint barrier on all CPUs to allow normal operation */
    PalServerlessCheckpointBarrierRelease();
    sti(); /* Re-enable interrupts after checkpoint completion */
    
    log_debug("Ring-3 program checkpoint created successfully (%zu memory regions)", 
              g_serverless_checkpoint->user_state.memory_mgmt.num_regions);
    
    /* Print static allocator usage statistics */
    CR_malloc_stats();
    
    log_always("=== Serverless Checkpoint Creation Completed ===");
    return 0;
}

/*
 * SYSCALL Wrapper for syscall handler - gets ring-3 context from TCB
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

__attribute__((unused)) static void do_debug_exit(void) {
    log_always("Debug exit requested - terminating process to analyze logs");
    
    /* In LibOS, use the proper exit mechanism through syscall */
    libos_syscall_exit_group(1);
    
    /* Should not reach here, but just in case */
    __builtin_unreachable();
}

/*
 * SYSCALL Wrapper for syscall handler - modifies ring-3 context in TCB to restore state
 */
int serverless_restore_checkpoint(void) {
    if (!g_serverless_checkpoint->metadata.checkpoint_created) {
        log_error("No ring-3 program checkpoint available");
        return -ENOENT;
    }
    
    log_always("=== Starting: Restore ===");
    
    /*
     * CRITICAL: Disable interrupts during entire restore process to prevent thread preemption
     * This ensures atomic restoration of both ring-0 kernel state and ring-3 program state
     */
    cli(); /* Disable interrupts to prevent preemption */
    PalServerlessCheckpointBarrierAcquire();
    
    /* Print complete LibOS kernel state before restore */
    print_system_state("PRE-RESTORE");
    
    // // TODO(chuqi): debug exit to analyze the logs (without actual restore)
    // do_debug_exit();

    /* Restore both ring-3 user memory and ring-0 kernel memory in unified approach */
    int ret = restore_all_memory_contents();
    if (ret < 0) {
        log_error("Failed to restore memory contents: %s", unix_strerror(ret));
        PalServerlessCheckpointBarrierRelease(); /* Release barrier on failure */
        sti(); /* Re-enable interrupts before returning */
        return ret;
    }
    
    /* Get ring-3 context from the current TCB and modify it directly */
    /* This will change where the syscall returns to when it completes */
    PAL_CONTEXT* ring3_context = LIBOS_TCB_GET(context.regs);
    if (!ring3_context) {
        log_error("Cannot access ring-3 CPU context for restore");
        PalServerlessCheckpointBarrierRelease(); /* Release barrier on failure */
        sti(); /* Re-enable interrupts before returning */
        return -EFAULT;
    }
    
    /* Restore ring-3 CPU state by directly modifying the context */
    ring3_context->rax = g_serverless_checkpoint->user_state.cpu_context.rax;
    ring3_context->rbx = g_serverless_checkpoint->user_state.cpu_context.rbx;
    ring3_context->rcx = g_serverless_checkpoint->user_state.cpu_context.rcx;
    ring3_context->rdx = g_serverless_checkpoint->user_state.cpu_context.rdx;
    ring3_context->rsi = g_serverless_checkpoint->user_state.cpu_context.rsi;
    ring3_context->rdi = g_serverless_checkpoint->user_state.cpu_context.rdi;
    ring3_context->rbp = g_serverless_checkpoint->user_state.cpu_context.rbp;
    ring3_context->rsp = g_serverless_checkpoint->user_state.cpu_context.rsp;
    ring3_context->r8  = g_serverless_checkpoint->user_state.cpu_context.r8;
    ring3_context->r9  = g_serverless_checkpoint->user_state.cpu_context.r9;
    ring3_context->r10 = g_serverless_checkpoint->user_state.cpu_context.r10;
    ring3_context->r11 = g_serverless_checkpoint->user_state.cpu_context.r11;
    ring3_context->r12 = g_serverless_checkpoint->user_state.cpu_context.r12;
    ring3_context->r13 = g_serverless_checkpoint->user_state.cpu_context.r13;
    ring3_context->r14 = g_serverless_checkpoint->user_state.cpu_context.r14;
    ring3_context->r15 = g_serverless_checkpoint->user_state.cpu_context.r15;
    
    /* Most importantly, set RIP to the checkpointed instruction */
    /* This is where the syscall will return to */
    ring3_context->rip = g_serverless_checkpoint->user_state.cpu_context.rip;
    ring3_context->efl = g_serverless_checkpoint->user_state.cpu_context.rflags;
    
    /* Restore segment registers */
    ring3_context->csgsfsss = g_serverless_checkpoint->user_state.cpu_context.cs;
    
    /* Release checkpoint barrier on all CPUs to allow normal operation */
    PalServerlessCheckpointBarrierRelease();
    sti();
    
    log_debug("Ring-3 program checkpoint restored - syscall will return to checkpointed state");
    log_debug("Restored RIP: 0x%lx, RSP: 0x%lx", 
             g_serverless_checkpoint->user_state.cpu_context.rip,
             g_serverless_checkpoint->user_state.cpu_context.rsp);
    
    log_always("=== Serverless Checkpoint Restore Completed ===");
    
    /* Return success - the syscall return mechanism will now jump to checkpointed state */
    return 0;
}

/*
 * Check if checkpoint exists
 */
bool serverless_has_checkpoint(void) {
    return g_serverless_checkpoint && g_serverless_checkpoint->metadata.checkpoint_created;
}

/*
 * Clear checkpoint
 */
int serverless_clear_checkpoint(void) {
    if (!g_serverless_checkpoint) {
        log_debug("No checkpoint state allocated");
        return 0;
    }
    
    /* Free ring-3 memory region snapshots */
    if (g_serverless_checkpoint->user_state.memory_mgmt.regions) {
        for (size_t i = 0; i < g_serverless_checkpoint->user_state.memory_mgmt.num_regions; i++) {
            free(g_serverless_checkpoint->user_state.memory_mgmt.regions[i].snapshot_data);
        }
        free(g_serverless_checkpoint->user_state.memory_mgmt.regions);
        g_serverless_checkpoint->user_state.memory_mgmt.regions = NULL;
        g_serverless_checkpoint->user_state.memory_mgmt.num_regions = 0;
    }
    
    /* Free ring-0 kernel memory snapshots */
    if (g_serverless_checkpoint->kernel_state.memory_mgmt.regions) {
        for (size_t i = 0; i < g_serverless_checkpoint->kernel_state.memory_mgmt.num_regions; i++) {
            free(g_serverless_checkpoint->kernel_state.memory_mgmt.regions[i].snapshot_data);
        }
        free(g_serverless_checkpoint->kernel_state.memory_mgmt.regions);
        g_serverless_checkpoint->kernel_state.memory_mgmt.regions = NULL;
        g_serverless_checkpoint->kernel_state.memory_mgmt.num_regions = 0;
    }
    
    /* Reset state */
    bool enabled = g_serverless_checkpoint->metadata.enabled; /* Preserve enabled flag */
    memset(g_serverless_checkpoint, 0, sizeof(struct serverless_checkpoint_state));
    g_serverless_checkpoint->metadata.enabled = enabled;
    
    /* Reset the static allocator */
    CR_malloc_reset();
    return 0;
}

/*
 * Initialize serverless checkpoint system.
 * Called during LibOS initialization.
 */
int init_serverless_checkpoint(void) {
    /* Initialize checkpoint base and end addresses */
    g_checkpoint_base = (uint64_t)&g_serverless_checkpoint_section.checkpoint_metadata;
    g_checkpoint_end = g_checkpoint_base + sizeof(g_serverless_checkpoint_section);
    
    /* Initialize the checkpoint state (using static page-aligned global variable) */
    memset(g_serverless_checkpoint, 0, sizeof(struct serverless_checkpoint_state));
    g_serverless_checkpoint->metadata.enabled = true;
    g_serverless_checkpoint->metadata.version = 1;
    
    /* Initialize the static allocator */
    CR_malloc_reset();
    
    /* Calculate page-aligned checkpoint section start for debugging */
    uintptr_t checkpoint_section_start = (uintptr_t)&g_serverless_checkpoint_section & ~0xFFF; /* Page-aligned start */
    assert(checkpoint_section_start == (uintptr_t)g_checkpoint_base);
    return 0;
}