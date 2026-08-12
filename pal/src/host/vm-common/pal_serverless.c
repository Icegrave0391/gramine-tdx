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

/* For page table primitives */
#include "kernel_memory.h"

/* For virtio device state that must be excluded from checkpoint/restore */
#include "kernel_virtio.h"

/*
 * Custom section attributes for page-aligned checkpoint code and data.
 * These sections will be placed in separate page-aligned regions by the linker.
 * 
 * - .pal.serverless.data: All static/global data for checkpoint (page-aligned)
 * - .pal.serverless.text: All checkpoint-related functions (page-aligned)
 */
#define SERVERLESS_DATA   __attribute__((section(".pal.serverless.data")))
#define SERVERLESS_CODE   __attribute__((section(".pal.serverless.text")))

/* PKS-related protection APIs (MSR_IA32_PKRS, PKS_PROT_KEY, PTE_KEY_* live in pal_serverless.h) */
#define CR4_PKS         (1ULL << 24)
#define CR0_WP          (1ULL << 16)
#define PTE_KEY_MASK    (((1ULL << PTE_KEY_BITS) - 1) << PTE_KEY_SHIFT)
typedef uint32_t u32;
typedef uint64_t u64;

#define PKS_DISABLE_R   (1ULL << 0)
#define PKS_DISABLE_W   (1ULL << 1)


static inline void __cpuid(u32 leaf, u32 subleaf,
                         u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf));
}

static bool cpu_has_pks(void)
{
    u32 eax, ebx, ecx, edx;
    __cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    return (edx >> 31) & 1;
}

int pks_init(void) {
    if (!cpu_has_pks()) {
        log_always("CPU does not support PKS");
        return -PAL_ERROR_INVAL;
    }

    /* Enable CR0.WP (PKS's W permission is dependent on CR0.WP) */
    u64 cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_WP; // Set WP bit
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));


    /* Ensure CR4.PKS */
    u64 cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_PKS; // Set PKS bit
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    log_always("PKS initialized: CR4=0x%lx, CR0=0x%lx", cr4, cr0);
    return 0;
}

static inline void __set_pte_protection(u64 *pte, bool should_protect) {
    u64 key = should_protect ? PKS_PROT_KEY : 0;
    *pte = (*pte & ~PTE_KEY_MASK) | ((key << PTE_KEY_SHIFT) & PTE_KEY_MASK);
}

/*
 * PKS call gate.
 *
 * PKRS is a per-CPU register holding 2 bits (AD=access-disable, WD=write-disable) per key. The
 * default armed state denies both read and write for PKS_PROT_KEY, so ordinary kernel code cannot
 * touch page tables, the SM sections or the checkpoint arena. The gate opens key 1 for the duration
 * of an SM operation and closes it on exit.
 *
 * Two properties matter for correctness:
 *
 * - Interrupts are masked inside the gate. PKRS is not saved/restored across context switches, so a
 *   preemption with the gate open would leak key-1 access to an unrelated thread.
 * - Entry/exit nest. The LibOS C/R handler opens the gate around the whole syscall, and the SM page
 *   table helpers it calls open it again; exit must therefore restore the previous PKRS value rather
 *   than unconditionally revoking.
 */
#define PKS_KEY_RW_MASK ((PKS_DISABLE_W | PKS_DISABLE_R) << (PKS_PROT_KEY * 2))

static inline u64 __pks_gate_enter(void) {
    u64 saved_pkrs = rdmsr(MSR_IA32_PKRS);
    wrmsr(MSR_IA32_PKRS, saved_pkrs & ~PKS_KEY_RW_MASK); /* grant RW on key 1 */
    return saved_pkrs;
}

static inline void __pks_gate_exit(u64 saved_pkrs) {
    wrmsr(MSR_IA32_PKRS, saved_pkrs); /* restore previous (possibly still-open) state */
}

/* Arm the deny-by-default policy for the calling CPU: no read, no write on key 1. */
static inline void __pks_arm_deny(void) {
    wrmsr(MSR_IA32_PKRS, rdmsr(MSR_IA32_PKRS) | PKS_KEY_RW_MASK);
}

/*
 * Grant/revoke key-1 access around a region of SM code, masking interrupts for the duration.
 * `SM_GATE_ENTER` must be paired with `SM_GATE_EXIT` on every return path.
 */
#define SM_GATE_ENTER()                                                     \
    u64 _gate_rflags;                                                       \
    __asm__ volatile("pushfq; popq %0" : "=r"(_gate_rflags) :: "memory");   \
    cli();                                                                  \
    u64 _gate_saved_pkrs = __pks_gate_enter()

#define SM_GATE_EXIT()                                                      \
    do {                                                                    \
        __pks_gate_exit(_gate_saved_pkrs);                                  \
        if (_gate_rflags & 0x200) /* RFLAGS.IF was set on entry */          \
            sti();                                                          \
    } while (0)

/* Linker-provided symbols for PAL serverless sections */
extern char __pal_serverless_data_start;
extern char __pal_serverless_data_end;
extern char __pal_serverless_text_start;
extern char __pal_serverless_text_end;

/* === Memory Isolation === */
SERVERLESS_DATA
static u64 g_libos_sm_data_start;
SERVERLESS_DATA
static u64 g_libos_sm_data_end;
SERVERLESS_DATA
static u64 g_libos_sm_code_start;
SERVERLESS_DATA
static u64 g_libos_sm_code_end;

SERVERLESS_DATA
static u64 g_pal_sm_data_start;
SERVERLESS_DATA
static u64 g_pal_sm_data_end;
SERVERLESS_DATA
static u64 g_pal_sm_code_start;
SERVERLESS_DATA
static u64 g_pal_sm_code_end;

#define PTE_ADDR_MASK   0x00000ffffffff000ULL
#define PTE_FLAG_MASK   0x0000000000000FFFULL

/* check if the target memory overlaps with the serverless module */
static bool _is_page_within_serverless_module(u64 addr) {
    assert(IS_ALIGNED(addr, PAGE_SIZE));
    if ((addr >= g_pal_sm_data_start && addr < g_pal_sm_data_end) ||
        (addr >= g_pal_sm_code_start && addr < g_pal_sm_code_end) ||
        (addr >= g_libos_sm_data_start && addr < g_libos_sm_data_end) ||
        (addr >= g_libos_sm_code_start && addr < g_libos_sm_code_end)) {
        return true;
    }
    return false;
}

static bool _is_page_a_PTP(u64 addr) {
    assert(IS_ALIGNED(addr, PAGE_SIZE));
    if (addr >= PAGE_TABLES_ADDR && addr < PAGE_TABLES_ADDR + PAGE_TABLES_SIZE) {
        return true;
    }
    return false;
}

/* Page table interface */

/*
 * Perform page table walk to find the PTE for the given virtual address `addr`.
 * We allow the kernel to do page table walk. No checks are required.
 */
SERVERLESS_CODE
static int _do_find_page_table_entry(uint64_t addr, uint64_t** out_pte_addr) {
    assert(g_pml4_table_base);
    uint64_t* pml4_table = (uint64_t*)g_pml4_table_base;

    /* the mask covers bits 12:43 which covers the address space [0, 16TB); note that we do not
     * cover up to 12:51 because bit 47 or 51 can be specially used (e.g. "shared" bit in TDX) */
    const uint64_t page_table_entry_addr_mask = 0x00000ffffffff000UL;

    /* there is a single entry in the PML4 table, see also memory_pagetables_init();
     * in this entry, bits 12:51 contain the address of the PDPT table */
    uint64_t* pdpt_table = (uint64_t*)(pml4_table[0] & page_table_entry_addr_mask);

    /* each PDPT table entry covers 1GB of memory, starting from addr 0x00 */
    size_t pdpt_table_idx = addr / 1024 / 1024 / 1024;
    uint64_t* pd_table = (uint64_t*)(pdpt_table[pdpt_table_idx] & page_table_entry_addr_mask);

    /* each PD table entry covers 2MB of memory in the 1GB memory region determined via PDPT table
     * entry (recall that there are 512 PD entries in one PD table) */
    size_t pd_table_idx = (addr / 1024 / 1024 / 2) % 512;
    uint64_t* pt_table = (uint64_t*)(pd_table[pd_table_idx] & page_table_entry_addr_mask);

    /* each PT table entry covers 4KB of memory in the 2MB memory region determined via PD table
     * entry (recall that there are 512 PD entries in one PT table) */
    size_t pt_table_idx = (addr / 1024 / 4) % 512;

    /* sanity check: must arrive at the same page address as in `addr` */
    uint64_t page_addr = pt_table[pt_table_idx] & page_table_entry_addr_mask;
    if ((addr & page_table_entry_addr_mask) != page_addr)
        return -PAL_ERROR_INVAL;

    *out_pte_addr = &pt_table[pt_table_idx];

    return 0;
}

SERVERLESS_CODE
int SM_find_page_table_entry(uint64_t addr, uint64_t** out_pte_addr) {
    SM_GATE_ENTER();

    int ret = _do_find_page_table_entry(addr, out_pte_addr);

    SM_GATE_EXIT();
    return ret;
}

SERVERLESS_CODE
int SM_update_memory_perms(int64_t addr, size_t size, bool write, bool execute, bool present, bool usermode) {
    int ret = 0;
    
    /* Debug: always print permission changes for usermode addresses */
    if (usermode && present) {
        log_debug("SM_update_memory_perms: range 0x%lx-0x%lx, W=%d X=%d present=%d usermode=%d",
                  addr, addr + size, write, execute, present, usermode);
    }

    SM_GATE_ENTER();

    for (u64 mark_addr = addr; mark_addr < addr + size; mark_addr += PAGE_SIZE) {
        u64 *pte_addr;
        ret = _do_find_page_table_entry(mark_addr, &pte_addr);
        if (ret < 0)
            goto out;
        
        /* Enforcement: flat mapping */
        assert(((*pte_addr & PTE_ADDR_MASK) == mark_addr));

        /* per-page copy: PTP/SM pages force S-mode below, and that must not leak into the
         * permissions computed for the remaining (possibly usermode) pages of this range */
        bool page_usermode = usermode;

        /* Enforcement: PTP and SM memory are attached with PKS key */
        if (_is_page_a_PTP(mark_addr) || _is_page_within_serverless_module(mark_addr)) {
            __set_pte_protection(pte_addr, true);
            page_usermode = false; // PTP and SM memory cannot be usermode accessible
            assert(present);  // PTP and SM memory must be present
        }

        if (!present) {
            *pte_addr &= ~1UL; /* mark not present */
            continue;
        }

        /* Enforcement: Kernel mode W^X */
        // assert(!(write && execute));
        if (!page_usermode && write && execute) {
            log_always("Warning: Setting KERN range 0x%lx - 0x%lx attempting to set both write and execute permissions on address 0x%lx", addr, addr + size, mark_addr);
        }

        uint64_t bits = 1UL; /* present bit is always set, since page is at least readable */
        if (write)
            bits |= 1UL << 1;
        if (page_usermode)
            bits |= 1UL << 2;
        if (!execute)
            bits |= 1UL << 63; /* NX/XD bit */
        
        *pte_addr = (*pte_addr & ~((1UL << 63) + 7UL)) | bits;
        
        /* Debug: verify PTE for execute segments */
        if (execute && present && mark_addr == addr) {
            log_debug("  PTE after update: 0x%lx (P=%d W=%d U=%d NX=%d)",
                      *pte_addr,
                      (int)(*pte_addr & 1),
                      (int)((*pte_addr >> 1) & 1),
                      (int)((*pte_addr >> 2) & 1),
                      (int)((*pte_addr >> 63) & 1));
        }
    }
out:
    SM_GATE_EXIT();
    return ret;
}

SERVERLESS_CODE
int SM_update_memory_uncacheable(int64_t addr, size_t size, bool mark) {
    int ret = 0;

    SM_GATE_ENTER();

    for (u64 mark_addr = addr; mark_addr < addr + size; mark_addr += PAGE_SIZE) {
        u64 *pte_addr;
        ret = _do_find_page_table_entry(mark_addr, &pte_addr);
        if (ret < 0)
            goto out;

        /* Enforcement: flat mapping */
        assert(((*pte_addr & PTE_ADDR_MASK) == mark_addr));

        if (mark)
            *pte_addr |= 1UL << 4; /* PCD bit */
        else
            *pte_addr &= ~(1UL << 4);
    }
out:
    SM_GATE_EXIT();
    return ret;
}

/* Helper function to get thread state name */
SERVERLESS_CODE
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
SERVERLESS_CODE
static struct pal_handle* get_pal_handle_from_thread(struct thread* thread) {
    if (!thread)
        return NULL;
    
    /* Calculate TCB address from thread address */
    uintptr_t tcb_addr = (uintptr_t)thread - offsetof(struct pal_tcb_vm, kernel_thread);
    struct pal_tcb_vm* tcb = (struct pal_tcb_vm*)tcb_addr;
    
    return tcb->thread_handle;
}

/* Callback function to print information about a single PAL thread */
SERVERLESS_CODE
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
SERVERLESS_CODE
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
SERVERLESS_CODE
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
SERVERLESS_CODE
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
SERVERLESS_CODE
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
SERVERLESS_CODE
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
SERVERLESS_CODE
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
SERVERLESS_CODE
size_t PalServerlessGetPalThreadCount(void) {
    size_t count = sched_get_thread_count();
    
    /* Also perform detailed thread walkthrough for debugging */
    walk_pal_thread_list();
    
    return count;
}

/*
 * Acquire checkpoint barrier on all CPUs for checkpoint/restore operations.
 * 
 * This function sets per-CPU barrier flags that cause idle and background threads
 * to pause their operations. This ensures system state consistency during
 * checkpoint/restore operations.
 */
SERVERLESS_CODE
void PalServerlessCheckpointBarrierAcquire(void) {
    checkpoint_barrier_acquire_all();
}

/*
 * Release checkpoint barrier on all CPUs.
 * 
 * This function clears per-CPU barrier flags, allowing idle and background threads
 * to resume their normal operations after checkpoint/restore is complete.
 */
SERVERLESS_CODE
void PalServerlessCheckpointBarrierRelease(void) {
    checkpoint_barrier_release_all();
}

/*
 * Report whether `addr`'s page holds kernel state that must NOT be rolled back by
 * checkpoint/restore.
 *
 * Two categories qualify, both being live kernel/host state rather than function memory:
 *
 * 1. virtio driver private state. A virtqueue's bookkeeping is split in two: the
 *    descriptor/avail/used rings live in memory shared with the VMM, while the driver-side indices
 *    (`cached_avail_idx`, `seen_used`, `free_desc`, buffer positions) live in PAL private memory.
 *    Restoring only the private half rewinds the driver's view while the VMM keeps its own, so the
 *    driver re-publishes descriptors the VMM already consumed. Symptoms: console output reprinted
 *    on every restore, and I/O completions (e.g. for sleeping threads) mis-attributed.
 *
 * 2. PAL kernel thread stacks. They hold the suspended execution context of the other kernel
 *    threads parked by the C/R barrier; rewinding them makes those threads resume on stale frames
 *    and jump to a garbage address (seen as an instruction-fetch #PF at RIP=0).
 *
 * Returns true if the page must be excluded from checkpoint/restore.
 */
SERVERLESS_CODE
bool PalServerlessIsNonRollbackPage(uint64_t addr) {
    uint64_t page = ALIGN_DOWN(addr, PAGE_SIZE);

    if (virtio_console_is_private_state_page(page))
        return true;

    /* kernel thread stacks hold live execution context of other (parked) kernel threads */
    if (thread_is_kernel_stack_page(page))
        return true;

    return false;
}

/*
 * Check whether the page containing `addr` is present and writable in the page tables.
 *
 * The checkpoint/restore engine needs this because LibOS VMA permissions do not always match the
 * actual PTE permissions (e.g. the single "PAL internal memory" VMA covers both read-only LibOS
 * text/rodata and writable data). Since pks_init() enables CR0.WP, a ring-0 write to a read-only
 * page faults, so C/R must skip such pages instead of trusting VMA `prot`.
 *
 * Returns true only if the PTE exists, is present and has the W bit set.
 */
SERVERLESS_CODE
bool PalServerlessIsPageWritable(uint64_t addr) {
    uint64_t* pte_addr = NULL;

    SM_GATE_ENTER();

    bool writable = false;
    if (_do_find_page_table_entry(ALIGN_DOWN(addr, PAGE_SIZE), &pte_addr) == 0 && pte_addr) {
        uint64_t pte = *pte_addr;
        writable = (pte & 1UL) /* present */ && (pte & (1UL << 1)) /* writable */;
    }

    SM_GATE_EXIT();
    return writable;
}

/*
 * Open the PKS gate for the caller and return the previous PKRS value.
 *
 * Used by the LibOS C/R engine, which reads and writes the checkpoint arena in the key-1 protected
 * `.libos.serverless.data` section for the whole duration of syscall 999. Callers must pass the
 * returned token to PalServerlessGateExit(). Interrupts are left untouched here: the C/R paths
 * already run under cli(), and the LibOS gate spans operations (memcpy of whole regions) too long to
 * keep interrupts masked purely for the gate's sake.
 */
SERVERLESS_CODE
uint64_t PalServerlessGateEnter(void) {
    return __pks_gate_enter();
}

/* Close the PKS gate, restoring the PKRS value captured by PalServerlessGateEnter(). */
SERVERLESS_CODE
void PalServerlessGateExit(uint64_t token) {
    __pks_gate_exit(token);
}

/* Arm the deny-by-default policy for the calling CPU: no read, no write on key 1.
 * PKRS is per-CPU, so every CPU (BSP and each AP) must call this after its own pks_init(). */
void pks_arm_current_cpu(void) {
    __pks_arm_deny();
}

static void memory_set_protection_key(u64 base, u64 end) {

    for (uint64_t addr = base; addr < end; addr += PAGE_SIZE) {
        uint64_t *pte_addr = NULL;
        if (memory_find_page_table_entry(addr, &pte_addr) == 0 && pte_addr) {
            __set_pte_protection(pte_addr, true);
        } else {
            log_always("Warning: Failed to find PTE for PAL SM data address 0x%lx", addr);
        }
    }
}

SERVERLESS_CODE
void PalServerlessModuleInit(uint64_t libos_sm_data_base, uint64_t libos_sm_data_end,
                             uint64_t libos_sm_code_base, uint64_t libos_sm_code_end) {
    u64 pal_sm_data_base = (u64)&__pal_serverless_data_start;
    u64 pal_sm_data_end = (u64)&__pal_serverless_data_end;
    u64 pal_sm_code_base = (u64)&__pal_serverless_text_start;
    u64 pal_sm_code_end = (u64)&__pal_serverless_text_end;

    g_pal_sm_data_start = pal_sm_data_base;
    g_pal_sm_data_end = pal_sm_data_end;
    g_pal_sm_code_start = pal_sm_code_base;
    g_pal_sm_code_end = pal_sm_code_end;

    g_libos_sm_data_start = libos_sm_data_base;
    g_libos_sm_data_end =  libos_sm_data_end;
    g_libos_sm_code_start = libos_sm_code_base;
    g_libos_sm_code_end = libos_sm_code_end;

    /* Set up PKS protection key for LibOS's serverless security monitor module */
    assert(IS_ALIGNED(libos_sm_data_base, PAGE_SIZE) && IS_ALIGNED(libos_sm_data_end, PAGE_SIZE));
    assert(IS_ALIGNED(libos_sm_code_base, PAGE_SIZE) && IS_ALIGNED(libos_sm_code_end, PAGE_SIZE));

    memory_set_protection_key(libos_sm_data_base, libos_sm_data_end);
    memory_set_protection_key(libos_sm_code_base, libos_sm_code_end);

    /* Set up PKS protection key for PAL's serverless security monitor module */
    memory_set_protection_key(pal_sm_code_base, pal_sm_code_end);
    memory_set_protection_key(pal_sm_data_base, pal_sm_data_end);
    
    /* Set up PKS protection key for the entire page table */
    memory_set_protection_key(PAGE_TABLES_ADDR, PAGE_TABLES_ADDR + PAGE_TABLES_SIZE);

    /* The key bits just written are cached in TLB entries tagged with the old (key 0) value, so
     * flush before the deny policy can take effect. Single vCPU is running at this point in boot;
     * APs pick up the policy in pks_arm_current_cpu() during their own startup. */
    flush_tlb();

    /*
     * Arm the deny-by-default policy: from here on, ordinary kernel code cannot read or write page
     * tables, the SM sections or the checkpoint arena. Only code inside an SM gate can.
     */
    pks_arm_current_cpu();

    log_always("PKS armed: key %d denied by default (PKRS=0x%lx)", PKS_PROT_KEY,
               rdmsr(MSR_IA32_PKRS));
}