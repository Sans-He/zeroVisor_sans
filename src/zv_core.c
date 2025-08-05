#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/utsname.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/irqflags.h>
#include <linux/kthread.h>

#include <asm/io.h>
#include <asm/desc.h>
#include <asm/desc_defs.h>
#include <asm/debugreg.h>
#include <asm/hw_breakpoint.h>

#include "../include/zv_core.h"
#include "../include/zv_log.h"
#include "../include/zv_config.h"
#include "../include/zv_types.h"
#include "../include/symbol.h"
#include "../include/zv_mmu.h"
#include "../include/zv_mem_manager.h"
#include "../include/asm.h"

/* Variables*/
int g_kernel_version_index = -1;
u64 g_max_ram_size = 0;

// VMCS Memory variables
void* g_vmxon_region_log_addr[MAX_PROCESSOR_COUNT] = {NULL, };
void* g_guest_vmcs_log_addr[MAX_PROCESSOR_COUNT] = {NULL, };
void* g_vm_exit_stack_addr[MAX_PROCESSOR_COUNT] = {NULL, };
void* g_io_bitmap_addrA[MAX_PROCESSOR_COUNT] = {NULL, };
void* g_io_bitmap_addrB[MAX_PROCESSOR_COUNT] = {NULL, };
void* g_msr_bitmap_addr[MAX_PROCESSOR_COUNT] = {NULL, };
void* g_virt_apic_page_addr[MAX_PROCESSOR_COUNT] = {NULL, };
u64 g_stack_size = MAX_STACK_SIZE;

// Memory pool variables
static spinlock_t g_mem_pool_lock;
struct zv_memory_pool g_memory_pool= {0, };

// Atomic variables for multi-core environment
atomic_t g_mutex_lock_flag;
atomic_t g_thread_run_flags;
atomic_t g_sync_flags;
atomic_t g_enter_count;
atomic_t g_vmlaunch_success_cnt;
atomic_t g_thread_complete_cnt;

// Some variables used for restoring the scene
struct desc_ptr g_gdtr_array[MAX_PROCESSOR_COUNT];

// Page Table related variables
u64 g_vm_host_phy_pml4 = 0;
u64 g_vm_init_phy_pml4 = 0;

// VM_Thread 
struct task_struct *g_vm_start_thread_id[MAX_PROCESSOR_COUNT]= {NULL, };

extern struct mutex module_mutex;

// kallsyms_lookup_name address exported by self
typedef unsigned long (*kallsyms_lookup_name_t)(const char *);
static kallsyms_lookup_name_t kallsyms_lookup_name = (kallsyms_lookup_name_t)KALLSYMS_LOOKUP_NAME_ADDR;


/* Static functions declarations */ 
static void zv_print_logo(void);
static int zv_get_kernel_version_index(void);
static int zv_check_kaslr(void);
static int zv_correct_symbol(void);
static void zv_alloc_vmcs_memory(void);
static void zv_protect_vmcs(void);
static int zv_setup_memory_pool(void);
static int zv_vm_thread_start(void* argument);
static int zv_init_vmx(int cpu_id);
static void zv_protect_gdt(int cpu_id);
static void zv_setup_vm_host_register(struct zv_vm_host_register* zv_vm_host_register);
static void zv_setup_vm_guest_register(
    struct zv_vm_guest_register* zv_vm_guest_register,
    const struct zv_vm_host_register* zv_vm_host_register
);
static void blank_bp_func(void);
static unsigned long zv_encode_dr7(
    int index,
    unsigned int len,
    unsigned int type
);
static u64 zv_get_desc_base(u64 offset);
static u64 zv_get_desc_access(u64 offset);
static void zv_setup_vm_control_register(
    struct zv_vm_control_register* zv_vm_control_register,
    int cpu_id
);
static void zv_vm_set_msr_write_bitmap(
    struct zv_vm_control_register* zv_vm_control_register,
    u64 msr_number
);
static void zv_setup_vmcs(
    const struct zv_vm_host_register* zv_vm_host_register,
    const struct zv_vm_guest_register* zv_vm_guest_register,
    const struct zv_vm_control_register* zv_vm_control_register
);
static void zv_print_vm_result(const char* string, int result);
static void zv_dup_page_table_for_host(void);

/* support for ZEROVISOR_USE_SHUTDOWN*/
#if ZEROVISOR_USE_SHUTDOWN
/* Variables*/
// static struct notifier_block* g_reboot_nb_ptr = NULL;
// static struct notifier_block g_reboot_nb = {
//     .notifier_call = 
// }

/* Functions*/


#endif

static int __init zeroVisor_init(void) {
    // variables init
    u32 eax, ebx, ecx, edx;
    int cpu_id;
    int cpu_count;
    int i;


    // sub-modules init
    zv_log_init();

    zv_log_write(LOG_NORMAL, "Core", "Hello, zeroVisor!");
    
    zv_print_logo();

    /* Check Kernel Version FOR use pre-definition symbol*/
    if (zv_get_kernel_version_index() == -1) {
        zv_log_write(LOG_NONE, "Core", "Kernel Version is not supported");
        zv_log_error(ERROR_KERNEL_VERSION_MISMATCH);
        return 0;
    }

    /* Check kASLR and correct symbol address*/
    if (zv_check_kaslr() == 1) {
        zv_log_write(LOG_DEBUG, "Core", "Kernel ASLR is enabled.");
        zv_correct_symbol();
    }

    /* Check VMX support*/
    cpuid_count(1, 0, &eax, &ebx, &ecx, &edx);
    zv_log_write(LOG_DETAIL, "Core", "Initialize VMX");
    zv_log_write(LOG_DETAIL, "Core", "  [*] Check virtualization, %08X, %08X, %08X, %08X", 
        eax, ebx, ecx, edx);
    if (ecx & CPUID_1_ECX_VMX) {
        zv_log_write(LOG_DETAIL, "Core", "  [*] VMX support");
    } else {
        zv_log_write(LOG_NONE, "Core", "  [!] VMX not support");
        zv_log_error(ERROR_HW_NOT_SUPPORT);
        return 0;
    }

#if ZEROVISOR_USE_SHUTDOWN
    /* Add callback funtion for checking system shutdown*/
    // TODO
#endif

    /* 
     * Check total RAM size.
     * To Cover system reserved area (3GB ~ 4GB)
     * if system has under 4GB RAM, zeroVisor sets 4GB to the variable.
     * if system has upper 4GB RAM, zeroVisor sets 1GB more than Original size to the variable.
     */
    g_max_ram_size = zv_get_max_ram_size();
    zv_log_write(LOG_DEBUG, "Core", "totalram_pages %ld, size %ld, "
		"g_max_ram_size %ld", totalram_pages(), totalram_pages() * VAL_4KB,
		g_max_ram_size); 
    g_max_ram_size = g_max_ram_size < VAL_4GB ? VAL_4GB : g_max_ram_size + VAL_1GB;

    cpu_id = smp_processor_id();
    cpu_count = num_online_cpus();

    zv_log_write(LOG_NORMAL, "Core", "CPU Count: %d", cpu_count);
    zv_log_write(LOG_NORMAL, "Core", "Booting CPU ID: %d", cpu_id);

    /* Allcate the required memory */
    zv_alloc_vmcs_memory();

    if (zv_alloc_ept_pages() != 0) {
        zv_log_error(ERROR_MEMORY_ALLOC_FAIL);
        goto ERROR_HANDLE;
    }
    zv_setup_ept_pagetables();

    /* Protect the memory */
    zv_protect_ept_pages();
    zv_protect_vmcs();

    /* Setup Memory for zeroVisor
     * After loaded and two world are separated. so zeroVisor should be
     * use own memory pool to prevent interference of the guest.
     */
    if (zv_setup_memory_pool() != 0) {
        return 0;
    }

    /* Atomic_set variables for multi-core environment */
	atomic_set(&g_mutex_lock_flag, 0);
    atomic_set(&g_thread_run_flags, cpu_count);
	atomic_set(&g_sync_flags, cpu_count);
	atomic_set(&g_enter_count, 0);
    atomic_set(&g_vmlaunch_success_cnt, 0);
    atomic_set(&g_thread_complete_cnt, 0);

    /* Create thread for each core */
    for (i = 0; i < cpu_count; i ++) {
        g_vm_start_thread_id[i] = (struct task_struct*)kthread_create_on_node(
            zv_vm_thread_start, NULL, cpu_to_node(i), "vm_thread");

        if (g_vm_start_thread_id[i]) {
            kthread_bind(g_vm_start_thread_id[i], i);
        } else {
            zv_log_write(LOG_NORMAL, "Core", "VMX [%d] Thread Run Fail", i);
        }
    }

    /* Duplicate page table for the host and zeroVisor */
    zv_dup_page_table_for_host();

    /*
	 * Execute thread for each core except this core.
	 * If zv_vm_thread_start() is executed, task scheduling is prohibited. So,
	 * If task switching is occured when this core run loop below, some core could
	 * not run zv_vm_thread_start().
	 */
    for (i = 0; i < cpu_count; i ++) {
        if (i != cpu_id) {
            wake_up_process(g_vm_start_thread_id[i]);
            zv_log_write(LOG_DEBUG, "Core", "VMX [%d] Thread Run Success", i);
        }
    }

    /* Execute thread for this core */
    wake_up_process(g_vm_start_thread_id[cpu_id]);
    zv_log_write(LOG_DEBUG, "Core", "VMX [%d] Thread Run Success", cpu_id);

    zv_log_write(LOG_DEBUG, "Core", "Waiting for complete");

    while (true) {
        if (atomic_read(&g_thread_complete_cnt) == cpu_count) break;
        msleep(100);
    }

    zv_log_write(LOG_NORMAL, "Core", "Execution Complete");
    
    zv_log_write(LOG_NORMAL, "Core", "zeroVisor Boot Success !");

    return 0;

ERROR_HANDLE:
    /* Free all the allocated memory block */
    zv_free_all();
    return 0;
}

static void __exit zeroVisor_exit(void) {
    /* Free all the allocated memory block */
    zv_free_all();

    zv_log_write(LOG_NORMAL, "Core", "GoodBye, zeroVisor!");
    zv_log_exit();
}

/*
 * Print logo.
 */
static void zv_print_logo(void) {
    zv_log_write(LOG_NORMAL, "Core", "");
    zv_log_write(LOG_NORMAL, "Core", "███████╗███████╗██████╗  ██████╗     ██╗   ██╗██╗███████╗ ██████╗ ██████╗ ");
    zv_log_write(LOG_NORMAL, "Core", "╚══███╔╝██╔════╝██╔══██╗██╔═══██╗    ██║   ██║██║██╔════╝██╔═══██╗██╔══██╗");
    zv_log_write(LOG_NORMAL, "Core", "  ███╔╝ █████╗  ██████╔╝██║   ██║    ██║   ██║██║███████╗██║   ██║██████╔╝");
    zv_log_write(LOG_NORMAL, "Core", "  ███╔╝ █████╗  ██████╔╝██║   ██║    ██║   ██║██║███████╗██║   ██║██████╔╝");
    zv_log_write(LOG_NORMAL, "Core", " ███╔╝  ██╔══╝  ██╔══██╗██║   ██║    ╚██╗ ██╔╝██║╚════██║██║   ██║██╔══██╗");
    zv_log_write(LOG_NORMAL, "Core", "███████╗███████╗██║  ██║╚██████╔╝     ╚████╔╝ ██║███████║╚██████╔╝██║  ██║");
    zv_log_write(LOG_NORMAL, "Core", "╚══════╝╚══════╝╚═╝  ╚═╝ ╚═════╝       ╚═══╝  ╚═╝╚══════╝ ╚═════╝ ╚═╝  ╚═╝");
    zv_log_write(LOG_NORMAL, "Core", "");
    zv_log_write(LOG_NORMAL, "Core", "                      A Type1.5 Hypervisor Skeleton");
    zv_log_write(LOG_NORMAL, "Core", "");
}

/*
 * Find matched index of current kernel version.
 */
static int zv_get_kernel_version_index(void) {
    int i;
    int match_index = -1;
    struct new_utsname* name;

    name = utsname();

    /* search kernel version table*/
    for (i = 0;  i < (sizeof(g_kernel_version) / sizeof(char*)); i ++) {
        if (strcmp(name->version, g_kernel_version[i]) == 0) {
            match_index = i;
            break;
        }
    }

    // transfer match_index to global varibale
    g_kernel_version_index = match_index;
    return match_index;
}

/*
 * Get address of kernel symbol.
 * kallsyms_lookup_name() does not have all symbol address which are in System.map.
 * if not found by kallsyms_lookup_name(), use pre-defined symbol table.
 */
u64 zv_get_symbol_address(char* symbol) {
    u64 symbol_address = 0;
    int i;

    symbol_address = kallsyms_lookup_name(symbol);

    // if cant found by kallsyms_lookup_name()
    if (symbol_address == 0) {
        if (g_kernel_version_index == -1) return 0;

        for (i = 0; i < SYMBOL_MAX_COUNT; i ++) {
            if (strcmp(g_symbol_table_array[g_kernel_version_index].symbol[i].name, symbol) == 0) {
                symbol_address = g_symbol_table_array[g_kernel_version_index].symbol[i].addr;
                break;
            }
        }
    }

    return symbol_address;
}

/*
 * Check kernel ASLR.
 */
static int zv_check_kaslr(void) {
    u64 static_text_addr;
    u64 dynamic_text_addr = kallsyms_lookup_name("_etext");
    int i;


    for (i = 0; i < SYMBOL_MAX_COUNT; i ++) {
        if (strcmp(g_symbol_table_array[g_kernel_version_index].symbol[i].name, "_etext") == 0) {
            static_text_addr = g_symbol_table_array[g_kernel_version_index].symbol[i].addr;
            break;
        }
    }

    if (static_text_addr != dynamic_text_addr) {
        zv_log_write(LOG_DEBUG, "Core", "_etext System.map=%lX Kallsyms=%lX", 
            static_text_addr, dynamic_text_addr);
        return 1;
    }

    return 0;
}

/*
 *  Correct static symbol address offset by kASLR.
 */
static int zv_correct_symbol(void) {
    u64 static_text_addr;
    u64 dynamic_text_addr = kallsyms_lookup_name("_etext");
    u64 delta;
    int i;
    

    for (i = 0; i < SYMBOL_MAX_COUNT; i ++) {
        if (strcmp(g_symbol_table_array[g_kernel_version_index].symbol[i].name, "_etext") == 0) {
            static_text_addr = g_symbol_table_array[g_kernel_version_index].symbol[i].addr;
            break;
        }
    }

    delta = dynamic_text_addr - static_text_addr;

    zv_log_write(LOG_DEBUG, "Core", "Correct the symbol offset caused by kASLR: %#lX", delta);

    for(i = 0; i < SYMBOL_MAX_COUNT; i ++) {
        g_symbol_table_array[g_kernel_version_index].symbol[i].addr += delta;
    }

    return 0;
}

/*
 * Allocate memory for VMCS.
 */
static void zv_alloc_vmcs_memory(void) {
    int cpu_count;
    int i;

    cpu_count = num_online_cpus();

    zv_log_write(LOG_DEBUG, "Core", "Alloc VMCS Memory");

    for (i = 0; i < cpu_count; i ++) {
        g_vmxon_region_log_addr[i] = zv_kmalloc(VMCS_SIZE, GFP_KERNEL);
        g_guest_vmcs_log_addr[i] = zv_kmalloc(VMCS_SIZE, GFP_KERNEL);

        g_vm_exit_stack_addr[i] = (void*)zv_vmalloc(g_stack_size);

        g_io_bitmap_addrA[i] = zv_kmalloc(IO_BITMAP_SIZE, GFP_KERNEL);
        g_io_bitmap_addrB[i] = zv_kmalloc(IO_BITMAP_SIZE, GFP_KERNEL);

        g_msr_bitmap_addr[i] = zv_kmalloc(IO_BITMAP_SIZE, GFP_KERNEL);
		g_virt_apic_page_addr[i] = zv_kmalloc(VIRT_APIC_PAGE_SIZE, GFP_KERNEL);

        if (   (! g_vmxon_region_log_addr[i])
            || (! g_guest_vmcs_log_addr[i])
            || (! g_vm_exit_stack_addr[i])
            || (! g_io_bitmap_addrA[i])
            || (! g_io_bitmap_addrB[i])
            || (! g_msr_bitmap_addr[i])
            || (! g_virt_apic_page_addr[i]) 
        ) {
            zv_log_write(LOG_DEBUG, "Core", "zv_alloc_vmcs_memory allocate fail");
            return ;
        } else {
            zv_log_write(LOG_DEBUG, "Core", "[*] VM[%d] Alloc Host VMCS %016lX",
                i, g_vmxon_region_log_addr[i]);
            zv_log_write(LOG_DEBUG, "Core", "[*] VM[%d] Alloc Guest VMCS %016lX",
                i, g_guest_vmcs_log_addr[i]);
            zv_log_write(LOG_DEBUG, "Core", "[*] VM[%d] Stack Addr %016lX",
                i, g_vm_exit_stack_addr[i]);
            zv_log_write(LOG_DEBUG, "Core", "[*] VM[%d] IO BitmapA Addr %016lX",
                i, g_io_bitmap_addrA[i]);
            zv_log_write(LOG_DEBUG, "Core", "[*] VM[%d] IO BitmapB Addr %016lX",
                i, g_io_bitmap_addrB[i]);
            zv_log_write(LOG_DEBUG, "Core", "[*] VM[%d] MSR Bitmap Addr %016lX",
                i, g_msr_bitmap_addr[i]); 
            zv_log_write(LOG_DEBUG, "Core", "[*] VM[%d] Virt APIC Page Addr %016lX",
                i, g_virt_apic_page_addr[i]);  
        }
    }
}

/* Hiding memory range from the guest */
void zv_hide_range(u64 start_addr, u64 end_addr, int alloc_type) {
    u64 i;
    u64 data;
    u64 phy_addr;
    u64 align_end_addr;

    /* Round up the end address */
    align_end_addr = (end_addr + PAGE_SIZE - 1) & MASK_PAGEADDR;

    for (i = (start_addr & MASK_PAGEADDR); i < align_end_addr; i += PAGE_SIZE) {
        data = *((u64*)i);

        if (alloc_type == ALLOC_KMALLOC) {
            phy_addr = virt_to_phys((void*)i);
        } else { // ALLOC_VMALLOC
            phy_addr = PFN_PHYS(vmalloc_to_pfn((void*)i));
        }

        zv_set_ept_hide_page(phy_addr);
    }
}

/* Lock memory range from the guest */
void zv_lock_range(u64 start_addr, u64 end_addr, int alloc_type) {
    u64 i;
    u64 phy_addr;
    u64 align_end_addr;

    /* Round up the end address */
    align_end_addr = (end_addr + PAGE_SIZE - 1) & MASK_PAGEADDR;

    for (i = (start_addr & MASK_PAGEADDR); i < align_end_addr; i += PAGE_SIZE) {
        if (alloc_type == ALLOC_KMALLOC) {
            phy_addr = virt_to_phys((void*)i);
        } else { // ALLOC_VMALLOC
            phy_addr = PFN_PHYS(vmalloc_to_pfn((void*)i));
        }
    }

    zv_set_ept_lock_page(phy_addr);
}

/* Protect VMCS structure */
static void zv_protect_vmcs(void) {
    int i;
    int cpu_count;

    cpu_count = num_online_cpus();
    zv_log_write(LOG_DEBUG, "MMU", "Protect VMCS");

    for (i = 0 ; i < cpu_count; i ++)
	{
		zv_hide_range((u64)g_vmxon_region_log_addr[i],
			(u64)g_vmxon_region_log_addr[i] + VMCS_SIZE, ALLOC_KMALLOC);
		zv_hide_range((u64)g_guest_vmcs_log_addr[i],
			(u64)g_guest_vmcs_log_addr[i] + VMCS_SIZE, ALLOC_KMALLOC);
		zv_hide_range((u64)g_vm_exit_stack_addr[i],
			(u64)g_vm_exit_stack_addr[i] + g_stack_size, ALLOC_VMALLOC);
		zv_hide_range((u64)g_io_bitmap_addrA[i],
			(u64)g_io_bitmap_addrA[i] + IO_BITMAP_SIZE, ALLOC_KMALLOC);
		zv_hide_range((u64)g_io_bitmap_addrB[i],
			(u64)g_io_bitmap_addrB[i] + IO_BITMAP_SIZE, ALLOC_KMALLOC);
		zv_hide_range((u64)g_msr_bitmap_addr[i],
			(u64)g_msr_bitmap_addr[i] + IO_BITMAP_SIZE, ALLOC_KMALLOC);
		zv_hide_range((u64)g_virt_apic_page_addr[i],
			(u64)g_virt_apic_page_addr[i] + VIRT_APIC_PAGE_SIZE, ALLOC_KMALLOC);
	}
}

/*
 * Allocate memory for zeroVisor.
 * After loaded and two world are separated. so zeroVisor should be
 * use own memory pool to prevent interference of the guest.
 */
static int zv_setup_memory_pool(void) {
    u64 i;
    u64 size;

    spin_lock_init(&g_mem_pool_lock);

    /* Allocate 1 page per 2MB */
    g_memory_pool.max_count = g_max_ram_size / VAL_2MB;
    size = g_memory_pool.max_count * VAL_4KB;
    g_memory_pool.pool = NULL;

    g_memory_pool.pool = (u64*)zv_vmalloc(size);
    if (! g_memory_pool.pool) goto ERROR;

    memset(g_memory_pool.pool, 0, size);

    for (i = 0; i < g_memory_pool.max_count; i ++) {
        g_memory_pool.pool[i] = (u64)zv_kmalloc(VAL_4KB, GFP_KERNEL);
        if (! g_memory_pool.pool[i]) goto ERROR;
    }

    g_memory_pool.pop_index = 0;

    return 0;

ERROR:
    if (g_memory_pool.pool) {
        for (i = 0; i < g_memory_pool.max_count; i ++) {
            if (g_memory_pool.pool[i]) {
                zv_kfree((void*)g_memory_pool.pool[i]);
            }
        }

        zv_kfree(g_memory_pool.pool);
    }

    return -1;
}

/* 
 *  Enable VT-x and run zeroVisor
 *  This thread function will be run on each Core.
 */
static int zv_vm_thread_start(void* argument) {
    struct zv_vm_host_register* host_register;
    struct zv_vm_guest_register* guest_register;
    struct zv_vm_control_register* control_register;
    u64 vm_err_number;
    unsigned long irqs;
    int result;
    int cpu_id;
    int cpu_count;

	cpu_id = smp_processor_id();
    cpu_count = num_online_cpus();

    host_register = zv_kmalloc(sizeof(struct zv_vm_host_register), GFP_KERNEL);
    guest_register = zv_kmalloc(sizeof(struct zv_vm_guest_register), GFP_KERNEL);
    control_register = zv_kmalloc(sizeof(struct zv_vm_control_register), GFP_KERNEL);
    
    if ((! host_register) || (! guest_register) || (! control_register)) {
        zv_log_write(LOG_NONE, "Core", "VM [%d] Register Allocate fail", cpu_id);
        return 0;
    }

    memset(host_register, 0, sizeof(struct zv_vm_host_register));
    memset(guest_register, 0, sizeof(struct zv_vm_guest_register));
    memset(control_register, 0, sizeof(struct zv_vm_control_register));

    /* Disable preempt */
    preempt_disable();

    /* Until all thread run to this */
    zv_log_write(LOG_DEBUG, "Core", "VM [%d] Wait until thread executed", cpu_id);
    atomic_dec(&g_thread_run_flags);

    while (true) {
        if (atomic_read(&g_thread_run_flags) == 0) break;

        mdelay(1);
    } 

    zv_log_write(LOG_DEBUG, "Core", "VM [%d] Complete to wait until thread executed", cpu_id);

    /* Disable interrupt before VM Launch */
    local_irq_save(irqs);
	zv_log_write(LOG_DEBUG, "Core", "VM [%d] IRQ Lock complete", cpu_id);

    /* Sync all cores stable status */
    zv_log_write(LOG_DEBUG, "Core", "VM [%d] Wait until stable status", cpu_id);

    mdelay(1000);
    atomic_dec(&g_sync_flags);

    while (true) {
        if (atomic_read(&g_sync_flags) == 0) break;

        mdelay(100);
    }

    zv_log_write(LOG_DEBUG, "Core", "VM [%d] Ready to go!", cpu_id);

    /* Initialize VMX one by one */
    while (atomic_read(&g_enter_count) != cpu_id) mdelay(1);

    if (zv_init_vmx(cpu_id) != 1) {
        atomic_inc(&g_enter_count); // continue next core even if this core error.
        zv_log_write(LOG_NONE, "Core", "VM [%d] zv_init_vmx fail", cpu_id);
        goto ERROR;
    }

    /* Protect GDI & IDT */
    zv_protect_gdt(cpu_id);

    /* Setup Registers */
    zv_setup_vm_host_register(host_register);
    zv_setup_vm_guest_register(guest_register, host_register);
    zv_setup_vm_control_register(control_register, cpu_id);

    /* Setup VMCS */
    zv_setup_vmcs(host_register, guest_register, control_register);

    zv_log_write(LOG_DEBUG, "Core", "VM [%d], Launch Start", cpu_id);
    result = zv_vm_launch();

	atomic_inc(&g_enter_count);

    if (result == 0) {
        zv_log_write(LOG_NONE, "Core", "    [*] VM [%d] Launch Valid Fail", cpu_id);
        zv_read_vmcs(VM_DATA_INST_ERROR, &vm_err_number);
        zv_log_write(LOG_NONE, "Core", "    [*] VM [%d] Error Number [%d]",
            cpu_id, (int)vm_err_number);
        zv_log_error(ERROR_LAUNCH_FAIL);
        
        goto ERROR;
    } else if (result == -1) {
        zv_log_write(LOG_NONE, "Core", "    [*] VM [%d] Launch Invalid Fail", cpu_id);
        zv_log_error(ERROR_LAUNCH_FAIL);

        goto ERROR;
    } else {
        zv_log_write(LOG_DEBUG, "Core", "   [*] VM [%d] Launch Success", cpu_id);
        atomic_inc(&g_vmlaunch_success_cnt);
    }

    /* Synchronize all core */
    while (true) {
        if (atomic_read(&g_vmlaunch_success_cnt) == cpu_count) break;

        mdelay(10);
    }

    /* Enable interrupt */
	preempt_enable();
	local_irq_restore(irqs);

    atomic_inc(&g_thread_complete_cnt);
    
    return 0;

ERROR:
    /* Enable interrupt */
    preempt_enable();
    local_irq_restore(irqs);

    return 0;
}

/* Initialize VMX context (VMCS) */
static int zv_init_vmx(int cpu_id) {
    u64 vmx_msr;
    u64 msr;
    u64 cr4;
    u64 cr0;
    u32* vmx_VMCS_log_addr;
	u32* vmx_VMCS_phy_addr;
	u32* guest_VMCS_log_addr;
	u32* guest_VMCS_phy_addr;
	u64 value;
	int result;

    vmx_msr = zv_rdmsr(MSR_IA32_VMX_BASIC);
    zv_log_write(LOG_DETAIL, "Core", "  [*] IA32_VMX_BASIC MSR Value: %016lX", vmx_msr);

    value = zv_rdmsr(MSR_IA32_VMX_ENTRY_CTLS);
    zv_log_write(LOG_DETAIL, "Core", "  [*] IA32_VMX_ENTRY_CTLS MSR Value: %016lX", value);

    value = zv_rdmsr(MSR_IA32_VMX_EXIT_CTLS);
    zv_log_write(LOG_DETAIL, "Core", "  [*] IA32_VMX_EXIT_CTLS MSR Value: %016lX", value);

    msr = zv_rdmsr(MSR_IA32_FEAT_CTL);
    zv_log_write(LOG_DETAIL, "Core", "  [*] IA32_FEAT_CTL MSR Value: %016lX", msr);

    msr = zv_rdmsr(MSR_IA32_VMX_PROCBASED_CTLS);
    zv_log_write(LOG_DETAIL, "Core", "  [*] IA32_VMX_PROCBASED_CTLS MSR VAlue: %016lX", msr);

    msr = zv_rdmsr(MSR_IA32_VMX_PROCBASED_CTLS2);
    zv_log_write(LOG_DETAIL, "Core", "  [*] IA32_VMX_PROCBASED_CTLS2 MSR VAlue: %016lX", msr);
    
	msr = zv_rdmsr(MSR_IA32_VMX_EPT_VPID_CAP);
    zv_log_write(LOG_DETAIL, "Core", "  [*] IA32_VMX_EPT_VPID MSR VAlue: %016lX", msr);
    
	msr = zv_rdmsr(MSR_EFER);
    zv_log_write(LOG_DETAIL, "Core", "  [*] IA32_EFER MSR VAlue: %016lX", msr);

    cr0 = zv_get_cr0();
    zv_log_write(LOG_DETAIL, "Core", "  [*] CR0: %016lX", cr0);

    cr4 = zv_get_cr4();
    zv_log_write(LOG_DETAIL, "Core", "  [*] Before Enable VMX CR4: %016lX", cr4);

    /* Enable VMX */
    zv_enable_vmx();
    zv_log_write(LOG_DETAIL, "Core", "  [*] Enable VMX");

    cr4 = zv_get_cr4();
    zv_log_write(LOG_DETAIL, "Core", "  [*] After Enable VMX CR4: %016lX", cr4);

    vmx_VMCS_log_addr = (u32*)(g_vmxon_region_log_addr[cpu_id]);
    vmx_VMCS_phy_addr = (u32*)virt_to_phys(vmx_VMCS_log_addr);

    zv_log_write(LOG_DETAIL, "Core", "  [*] Alloc Physical VMCS %016lX", vmx_VMCS_phy_addr);
    zv_log_write(LOG_DETAIL, "Core", "  [*] Start VMX");

    /* First data of VMCS should be VMX revision number */
    vmx_VMCS_log_addr[0] = (u32)vmx_msr;
    result = zv_start_vmx(&vmx_VMCS_phy_addr);
    if (result) {
        zv_log_write(LOG_DETAIL, "Core", "  [*] VMXON Success");
    } else {
        zv_log_write(LOG_NONE, "Core", "  [*] VMXON Fail");
        zv_log_error(ERROR_LAUNCH_FAIL);
        return 0;
    }

    /* Allocate kernel memory for Guest VMCS */
    zv_log_write(LOG_DETAIL, "Core", "Preparing Guest");
    guest_VMCS_log_addr = (u32*)(g_guest_vmcs_log_addr[cpu_id]);
    guest_VMCS_phy_addr = (u32*)virt_to_phys(guest_VMCS_log_addr);

    zv_log_write(LOG_DETAIL, "Core", "  [*] Alloc Physical Guest VMCS %016lX", guest_VMCS_phy_addr);
    
	/* First data of VMCS should be VMX revision number */
	guest_VMCS_log_addr[0] = (u32)vmx_msr;
    result = zv_clear_vmcs(&guest_VMCS_phy_addr);
    if (result) {
        zv_log_write(LOG_DETAIL, "Core", "  [*] Guest VMCS Clear Success");
    } else {
        zv_log_write(LOG_NONE, "Core", "  [*] Guest VMCS Clear Fail");
        zv_log_error(ERROR_LAUNCH_FAIL);
        return 0;
    }

    /* Load VMCS */
    result = zv_load_vmcs((void**)&guest_VMCS_phy_addr);
    if (result) {
        zv_log_write(LOG_DETAIL, "Core", "  [*] Guest VMCS Load Success");
    } else {
        zv_log_write(LOG_NONE, "Core", "  [*] Guest VMCS Load Fail");
        zv_log_error(ERROR_LAUNCH_FAIL);
        return 0;
    }

    return 1;
}

/* Protect GDT and IDT */
static void zv_protect_gdt(int cpu_id) {
    struct desc_ptr idtr;

    native_store_gdt(&g_gdtr_array[cpu_id]);
    store_idt(&idtr); // some native_* functions erase in 5.X + kernel

    zv_log_write(LOG_DEBUG, "Core", "VM [%d] Protect GDT IDT", cpu_id);
    zv_log_write(LOG_DEBUG, "Core", "VM [%d]    [*] GDTR Base %16lX, Size %d",
        cpu_id, g_gdtr_array[cpu_id].address, g_gdtr_array[cpu_id].size);
    zv_log_write(LOG_DEBUG, "Core", "VM [%d]    [*] IDTR Base %16lX, Size %d",
        cpu_id, idtr.address, idtr.size);

    zv_lock_range(idtr.address, (idtr.address + 0xFFF) & MASK_PAGEADDR, ALLOC_VMALLOC);
}

/* Setup the host registers */
static void zv_setup_vm_host_register(struct zv_vm_host_register* zv_vm_host_register) {
    struct desc_ptr gdtr;
	struct desc_ptr idtr;
	struct ldttss_desc* tss;
	u64 base0 = 0;
	u64 base1 = 0;
	u64 base2 = 0;
	u64 base3 = 0;
	char* vm_exit_stack;
	u64 stack_size = g_stack_size;
	int cpu_id;

	cpu_id = smp_processor_id();

    /* Allocate kernel stack for VM Exit */
    vm_exit_stack = (char*)(g_vm_exit_stack_addr[cpu_id]);
    memset(vm_exit_stack, 0, stack_size);

    native_store_gdt(&gdtr);
    store_idt(&idtr);

    zv_log_write(LOG_DETAIL, "Core", "VM [%d] Setup Host register", cpu_id);
    zv_log_write(LOG_DETAIL, "Core", "  [*] GDTR Address: %016lX", gdtr.address);
    zv_log_write(LOG_DETAIL, "Core", "  [*] GDTR Size: %d", gdtr.size);
    zv_log_write(LOG_DETAIL, "Core", "  [*] IDTR Address: %016lX", idtr.address);
    zv_log_write(LOG_DETAIL, "Core", "  [*] IDTR Size: %d", idtr.size);

    zv_vm_host_register->cr0 = zv_get_cr0();

    /* Using Shadow CR3 for world seoaration */
    zv_vm_host_register->cr3 = g_vm_host_phy_pml4;
    
    zv_vm_host_register->cr4 = zv_get_cr4();

    zv_vm_host_register->rsp = (u64)vm_exit_stack + stack_size - 0x1000;
    zv_vm_host_register->rip = (u64)zv_vm_exit_callback_stub;

    zv_vm_host_register->cs_selector = __KERNEL_CS;
	zv_vm_host_register->ss_selector = __KERNEL_DS;
	zv_vm_host_register->ds_selector = __KERNEL_DS;
	zv_vm_host_register->es_selector = __KERNEL_DS;
	zv_vm_host_register->fs_selector = __KERNEL_DS;
	zv_vm_host_register->gs_selector = __KERNEL_DS;
    zv_vm_host_register->tr_selector = zv_get_tr();

    zv_vm_host_register->fs_base_addr = zv_rdmsr(MSR_FS_BASE);
    zv_vm_host_register->gs_base_addr = zv_rdmsr(MSR_GS_BASE);

    tss = (struct ldttss_desc*)(gdtr.address + 
        (zv_vm_host_register->tr_selector & ~MASK_GDT_ACCESS));
    base0 = tss->base0;
    base1 = tss->base1;
	base2 = tss->base2;
	base3 = tss->base3;
    zv_vm_host_register->tr_base_addr = 
        base0 | (base1 << 16) | (base2 << 24) | (base3 << 32);

    zv_vm_host_register->gdtr_base_addr = gdtr.address;
    zv_vm_host_register->idtr_base_addr = idtr.address;

    zv_vm_host_register->ia32_sys_enter_cs = zv_rdmsr(MSR_IA32_SYSENTER_CS);
	zv_vm_host_register->ia32_sys_enter_esp = zv_rdmsr(MSR_IA32_SYSENTER_ESP);
	zv_vm_host_register->ia32_sys_enter_eip = zv_rdmsr(MSR_IA32_SYSENTER_EIP);

    zv_vm_host_register->ia32_perf_global_ctrl = zv_rdmsr(MSR_CORE_PERF_GLOBAL_CTRL);
	zv_vm_host_register->ia32_pat = zv_rdmsr(MSR_IA32_CR_PAT);
	zv_vm_host_register->ia32_efer = zv_rdmsr(MSR_EFER);
}

/* Setup the guest registers */
static void zv_setup_vm_guest_register(
    struct zv_vm_guest_register* zv_vm_guest_register,
    const struct zv_vm_host_register* zv_vm_host_register
) {
    struct desc_ptr gdtr;
	struct desc_ptr idtr;
	struct desc_struct* gdt;
	struct ldttss_desc* ldt;
	struct ldttss_desc* tss;
	u64 base0 = 0;
	u64 base1 = 0;
	u64 base2 = 0;
	u64 base3 = 0;
	u64 access = 0;
	u64 qwLimit0 = 0;
	u64 qwLimit1 = 0;
	int cpu_id;
	unsigned long dr6;
	unsigned long dr7 = 0;

	cpu_id = smp_processor_id();

    native_store_gdt(&gdtr);
    store_idt(&idtr);

    zv_log_write(LOG_DETAIL, "Core", "VM [%d] Setup Guest Register");

    zv_vm_guest_register->cr0 = zv_vm_host_register->cr0;
	zv_vm_guest_register->cr3 = zv_get_cr3();
	zv_vm_guest_register->cr4 = zv_vm_host_register->cr4;

    /* Hardware Breakpoint Hook functions define */
    set_debugreg((unsigned long)blank_bp_func, 0);
	set_debugreg((unsigned long)blank_bp_func, 1);
	set_debugreg((unsigned long)blank_bp_func, 2);
	set_debugreg((unsigned long)blank_bp_func, 3);

    dr7 = zv_encode_dr7(0, X86_BREAKPOINT_LEN_X, X86_BREAKPOINT_EXECUTE);
    dr7 |= zv_encode_dr7(1, X86_BREAKPOINT_LEN_X, X86_BREAKPOINT_EXECUTE);
	dr7 |= zv_encode_dr7(2, X86_BREAKPOINT_LEN_X, X86_BREAKPOINT_EXECUTE);
	dr7 |= zv_encode_dr7(3, X86_BREAKPOINT_LEN_X, X86_BREAKPOINT_EXECUTE);
	dr7 |= (0x01 << 10);

    zv_vm_guest_register->dr7 = dr7;

	get_debugreg(dr6, 6);
	dr6 &= 0xfffffffffffffff0;
	set_debugreg(dr6, 6);

    zv_log_write(LOG_DETAIL, "Core", "ORG dr6: %lx", dr6);
    zv_log_write(LOG_DETAIL, "Core", "ORG dr7: %lx", dr7);

    zv_vm_guest_register->rflags = zv_get_rflags();
	/* Under two registers are set when VM launch. */
	zv_vm_guest_register->rsp = 0xFFFFFFFFFFFFFFFF;
	zv_vm_guest_register->rip = 0xFFFFFFFFFFFFFFFF;

	zv_vm_guest_register->cs_selector = zv_get_cs();
	zv_vm_guest_register->ss_selector = zv_get_ss();
	zv_vm_guest_register->ds_selector = zv_get_ds();
	zv_vm_guest_register->es_selector = zv_get_es();
	zv_vm_guest_register->fs_selector = zv_get_fs();
	zv_vm_guest_register->gs_selector = zv_get_gs();
	zv_vm_guest_register->ldtr_selector = zv_get_ldtr();
	zv_vm_guest_register->tr_selector = zv_get_tr();

    zv_vm_guest_register->cs_base_addr =
		zv_get_desc_base(zv_vm_guest_register->cs_selector);
	zv_vm_guest_register->ss_base_addr =
		zv_get_desc_base(zv_vm_guest_register->ss_selector);
	zv_vm_guest_register->ds_base_addr =
		zv_get_desc_base(zv_vm_guest_register->ds_selector);
	zv_vm_guest_register->es_base_addr =
		zv_get_desc_base(zv_vm_guest_register->es_selector);

	zv_vm_guest_register->fs_base_addr = zv_rdmsr(MSR_FS_BASE);
	zv_vm_guest_register->gs_base_addr = zv_rdmsr(MSR_GS_BASE);

    /* Construct ldtr_base_addr */
    if (! zv_vm_guest_register->ldtr_selector) {
        zv_vm_guest_register->ldtr_base_addr = 0;
    } else {
        ldt = (struct ldttss_desc*)(gdtr.address + 
            (zv_vm_guest_register->ldtr_selector & ~MASK_GDT_ACCESS));
        base0 = ldt->base0;
        base1 = ldt->base1;
		base2 = ldt->base2;
		base3 = ldt->base3;

        zv_vm_guest_register->ldtr_base_addr = 
            base0 | (base1 << 16) | (base2 << 24) | (base3 << 32);
    }

    /* Construct tr_base_addr */
    if (! zv_vm_guest_register->tr_selector) {
        zv_vm_guest_register->tr_base_addr = 0x00;
    } else {
        tss = (struct ldttss_desc*)(gdtr.address + 
            (zv_vm_guest_register->tr_selector & ~MASK_GDT_ACCESS));
        base0 = tss->base0;
        base1 = tss->base1;
		base2 = tss->base2;
		base3 = tss->base3;

        zv_vm_guest_register->tr_base_addr = 
            base0 | (base1 << 16) | (base2 << 24) | (base3 << 32);
    }

    zv_vm_guest_register->cs_limit = 0xFFFFFFFF;
	zv_vm_guest_register->ss_limit = 0xFFFFFFFF;
	zv_vm_guest_register->ds_limit = 0xFFFFFFFF;
	zv_vm_guest_register->es_limit = 0xFFFFFFFF;
	zv_vm_guest_register->fs_limit = 0xFFFFFFFF;
	zv_vm_guest_register->gs_limit = 0xFFFFFFFF;

    /* Construct ldtr_limit */
    if (! zv_vm_guest_register->ldtr_selector) {
        zv_vm_guest_register->ldtr_limit = 0;
    } else {
        ldt = (struct ldttss_desc*)(gdtr.address + 
            (zv_vm_guest_register->ldtr_selector & ~MASK_GDT_ACCESS));

        qwLimit0 = ldt->limit0;
        qwLimit1 = ldt->limit1;
        
        zv_vm_guest_register->ldtr_limit = qwLimit0 | (qwLimit1 << 16);
    }

    /* Construct tr_limit */
    if (! zv_vm_guest_register->tr_selector) {
        zv_vm_guest_register->tr_limit = 0;
    } else {
        tss = (struct ldttss_desc*)(gdtr.address + 
            (zv_vm_guest_register->tr_selector & ~MASK_GDT_ACCESS));

        qwLimit0 = tss->limit0;
        qwLimit1 = tss->limit1;
        
        zv_vm_guest_register->tr_limit = qwLimit0 | (qwLimit1 << 16);
    }

    zv_vm_guest_register->cs_access =
		zv_get_desc_access(zv_vm_guest_register->cs_selector);
	zv_vm_guest_register->ss_access =
		zv_get_desc_access(zv_vm_guest_register->ss_selector);
	zv_vm_guest_register->ds_access =
		zv_get_desc_access(zv_vm_guest_register->ds_selector);
	zv_vm_guest_register->es_access =
		zv_get_desc_access(zv_vm_guest_register->es_selector);
	zv_vm_guest_register->fs_access =
		zv_get_desc_access(zv_vm_guest_register->fs_selector);
	zv_vm_guest_register->gs_access =
		zv_get_desc_access(zv_vm_guest_register->gs_selector);

    /* Construct ldtr_access */
    if (! zv_vm_guest_register->ldtr_selector) {
        zv_vm_guest_register->ldtr_access = 0x10000;
    } else {
        ldt = (struct ldttss_desc*)(gdtr.address + 
            (zv_vm_guest_register->ldtr_selector & ~MASK_GDT_ACCESS));
        gdt = (struct desc_struct*)ldt;
        
        /* type: 4, s: 1, dpl: 2, p: 1; limit: 4, avl: 1, l: 1, d: 1, g: 1 */
        access = 0 
            | (gdt->type)
            | (gdt->s << 4)
            | (gdt->dpl << 5)
            | (gdt->p << 7)
            | (gdt->avl << 12)
            | (gdt->l << 13)
            | (gdt->d << 14)
            | (gdt->g << 15);

        zv_vm_guest_register->ldtr_access = access;
    }

    /* Construct tr_access */
    if (! zv_vm_guest_register->tr_selector) {
        zv_vm_guest_register->tr_access = 0;
    } else {
        tss = (struct ldttss_desc*)(gdtr.address + 
            (zv_vm_guest_register->tr_selector & ~MASK_GDT_ACCESS));
        gdt = (struct desc_struct*)tss;

        /* type: 4, s: 1, dpl: 2, p: 1; limit: 4, avl: 1, l: 1, d: 1, g: 1 */
        access = 0 
            | (gdt->type)
            | (gdt->s << 4)
            | (gdt->dpl << 5)
            | (gdt->p << 7)
            | (gdt->avl << 12)
            | (gdt->l << 13)
            | (gdt->d << 14)
            | (gdt->g << 15);

        zv_vm_guest_register->tr_access = access;
    }

    zv_vm_guest_register->gdtr_base_addr = zv_vm_host_register->gdtr_base_addr;
	zv_vm_guest_register->idtr_base_addr = zv_vm_host_register->idtr_base_addr;
	zv_vm_guest_register->gdtr_limit = gdtr.size;
	zv_vm_guest_register->idtr_limit = idtr.size;

	zv_vm_guest_register->ia32_debug_ctrl = 0;
	zv_vm_guest_register->ia32_sys_enter_cs = zv_vm_host_register->ia32_sys_enter_cs;
	zv_vm_guest_register->ia32_sys_enter_esp = zv_vm_host_register->ia32_sys_enter_esp;
	zv_vm_guest_register->ia32_sys_enter_eip = zv_vm_host_register->ia32_sys_enter_eip;
	zv_vm_guest_register->vmcs_link_ptr = 0xFFFFFFFFFFFFFFFF;

	zv_vm_guest_register->ia32_perf_global_ctrl = 
        zv_vm_host_register->ia32_perf_global_ctrl;
	zv_vm_guest_register->ia32_pat = zv_vm_host_register->ia32_pat;
	zv_vm_guest_register->ia32_efer = zv_vm_host_register->ia32_efer;
}

/* Blank function just for placeholder */
static void blank_bp_func(void) {
    return;
}

/* Convert DR7 from debug register index, length, type */
static unsigned long zv_encode_dr7(
    int index,
    unsigned int len,
    unsigned int type
) {
    unsigned long value;

    value = (len | type) & 0xf;
    value <<= (DR_CONTROL_SHIFT + index * DR_CONTROL_SIZE);
    value |= (DR_GLOBAL_ENABLE << (index * DR_ENABLE_SIZE));

	return value;
}

/* Get base address of the descriptor */
static u64 zv_get_desc_base(u64 offset) {
    struct desc_ptr gdtr;
	struct desc_struct* gdt;
	u64 qwTotalBase = 0;
	u64 base0 = 0;
	u64 base1 = 0;
	u64 base2 = 0;

	if (offset == 0)
	{
		return 0;
	}

	native_store_gdt(&gdtr);
	gdt = (struct desc_struct*)(gdtr.address + (offset & ~MASK_GDT_ACCESS));

	base0 = gdt->base0;
	base1 = gdt->base1;
	base2 = gdt->base2;

	qwTotalBase = base0 | (base1 << 16) | (base2 << 24);
	return qwTotalBase;
}

/* Get access type of the descriptor */
static u64 zv_get_desc_access(u64 offset) {
    struct desc_ptr gdtr;
	struct desc_struct* gdt;
	u64 access = 0;

	if (offset == 0)
	{
		/* Return unused value. */
		return 0x10000;
	}

	native_store_gdt(&gdtr);
	gdt = (struct desc_struct*)(gdtr.address + (offset & ~MASK_GDT_ACCESS));
	/* type: 4, s: 1, dpl: 2, p: 1; limit: 4, avl: 1, l: 1, d: 1, g: 1 */
	access = 0 
            | (gdt->type)
            | (gdt->s << 4)
            | (gdt->dpl << 5)
            | (gdt->p << 7)
            | (gdt->avl << 12)
            | (gdt->l << 13)
            | (gdt->d << 14)
            | (gdt->g << 15);

	return access;
}

/* Setup the VM control register */
static void zv_setup_vm_control_register(
    struct zv_vm_control_register* zv_vm_control_register,
    int cpu_id
) {
    u64 sec_flags = 0;
    zv_log_write(LOG_DETAIL, "Core", "VM [%d] Setup VM Control Register", cpu_id);

    sec_flags |= VM_BIT_VM_SEC_PROC_CTRL_DESC_TABLE;
    sec_flags |= VM_BIT_VM_SEC_PROC_CTRL_UNREST_GUEST;
    sec_flags |= VM_BIT_VM_SEC_PROC_CTRL_USE_EPT;

    /* Enable rdtscp instrument */
    sec_flags |= VM_BIT_VM_SEC_PROC_CTRL_ENABLE_RDTSCP;
    /* Enable xsaves/xrstors instruments */
    sec_flags |= VM_BIT_VM_SEC_PROC_CTRL_ENABLE_XSAVES_XRSTORS;
    
    if (( zv_rdmsr(MSR_IA32_VMX_PROCBASED_CTLS2) >> 32) 
        & VM_BIT_VM_SEC_PROC_CTRL_ENABLE_INVPCID) {
        zv_log_write(LOG_DEBUG, "Core", "VM [%d] Support Enable INVPCID", cpu_id);
        sec_flags |= VM_BIT_VM_SEC_PROC_CTRL_ENABLE_INVPCID;
    }

    if (( zv_rdmsr(MSR_IA32_VMX_PROCBASED_CTLS2) >> 32) 
        & VM_BIT_VM_SEC_PROC_CTRL_ENABLE_USER_WAIT_PAUSE) {
        zv_log_write(LOG_DEBUG, "Core", "VM [%d] Support Enable USER_WAIT_PAUSE", cpu_id);
        /* Enable tpause,umonitor or umwait instruments */
        sec_flags |= VM_BIT_VM_SEC_PROC_CTRL_ENABLE_USER_WAIT_PAUSE;
    }

    zv_vm_control_register->pin_based_ctrl = 
        ( zv_rdmsr(MSR_IA32_VMX_TRUE_PINBASED_CTLS)
        | VM_BIT_VM_PIN_BASED_USE_PRE_TIMER
        ) & 0xFFFFFFFF; 

    zv_vm_control_register->pri_proc_based_ctrl = 
        ( zv_rdmsr(MSR_IA32_VMX_TRUE_PROCBASED_CTLS)
        | VM_BIT_VM_PRI_PROC_CTRL_USE_IO_BITMAP
        | VM_BIT_VM_PRI_PROC_CTRL_USE_MSR_BITMAP
        | VM_BIT_VM_PRI_PROC_CTRL_USE_SEC_CTRL
        | VM_BIT_VM_PRI_PROC_CTRL_USE_MOVE_DR
        ) & 0xFFFFFFFF;

    zv_vm_control_register->sec_proc_based_ctrl =
		( zv_rdmsr(MSR_IA32_VMX_PROCBASED_CTLS2)
        | sec_flags
        ) & 0xFFFFFFFF;

	zv_vm_control_register->vm_entry_ctrl_field =
		( zv_rdmsr(MSR_IA32_VMX_TRUE_ENTRY_CTRLS)
        | VM_BIT_VM_ENTRY_CTRL_IA32E_MODE_GUEST
        | VM_BIT_VM_ENTRY_LOAD_DEBUG_CTRL
        ) & 0xFFFFFFFF;

    zv_vm_control_register->vm_exti_ctrl_field = 
        ( zv_rdmsr(MSR_IA32_VMX_TRUE_EXIT_CTRLS)
        | VM_BIT_VM_EXIT_CTRL_HOST_ADDR_SIZE
        | VM_BIT_VM_EXIT_SAVE_DEBUG_CTRL
        | VM_BIT_VM_EXIT_CTRL_SAVE_PRE_TIMER
        | VM_BIT_VM_EXIT_CTRL_SAVE_IA32_EFER
        ) & 0xFFFFFFFF;

    /* Hardware_BreakPoint enable : 0x02 unable: 0x00 */
	zv_vm_control_register->except_bitmap = 0x02;

    zv_vm_control_register->io_bitmap_addrA = (u64)(g_io_bitmap_addrA[cpu_id]);
	zv_vm_control_register->io_bitmap_addrB = (u64)(g_io_bitmap_addrB[cpu_id]);
	zv_vm_control_register->msr_bitmap_addr = (u64)(g_msr_bitmap_addr[cpu_id]);
	zv_vm_control_register->virt_apic_page_addr = (u64)(g_virt_apic_page_addr[cpu_id]);
    
    memset((char*)zv_vm_control_register->io_bitmap_addrA, 0, 0x1000);
	memset((char*)zv_vm_control_register->io_bitmap_addrB, 0, 0x1000);
	memset((char*)zv_vm_control_register->msr_bitmap_addr, 0, 0x1000);
	memset((char*)zv_vm_control_register->virt_apic_page_addr, 0, 0x1000);

	/* Registers related SYSENTER, SYSCALL MSR are write-protected. */
    zv_vm_set_msr_write_bitmap(zv_vm_control_register, MSR_IA32_SYSENTER_CS);
	zv_vm_set_msr_write_bitmap(zv_vm_control_register, MSR_IA32_SYSENTER_ESP);
	zv_vm_set_msr_write_bitmap(zv_vm_control_register, MSR_IA32_SYSENTER_EIP);
	zv_vm_set_msr_write_bitmap(zv_vm_control_register, MSR_IA32_STAR);
	zv_vm_set_msr_write_bitmap(zv_vm_control_register, MSR_IA32_LSTAR);
	zv_vm_set_msr_write_bitmap(zv_vm_control_register, MSR_IA32_FMASK);

    zv_vm_control_register->io_bitmap_addrA =
		(u64)virt_to_phys((void*)zv_vm_control_register->io_bitmap_addrA);
	zv_vm_control_register->io_bitmap_addrB =
		(u64)virt_to_phys((void*)zv_vm_control_register->io_bitmap_addrB);
	zv_vm_control_register->msr_bitmap_addr =
		(u64)virt_to_phys((void*)zv_vm_control_register->msr_bitmap_addr);
	zv_vm_control_register->virt_apic_page_addr =
		(u64)virt_to_phys((void*)zv_vm_control_register->virt_apic_page_addr);
    
    zv_vm_control_register->ept_ptr =
		(u64)virt_to_phys((void*)g_ept_info.pml4_page_addr_array[0])
        | VM_BIT_EPT_PAGE_WALK_LENGTH_BITMAP
        | VM_BIT_EPT_MEM_TYPE_WB;

    zv_vm_control_register->cr4_guest_host_mask = CR4_BIT_VMXE;
    zv_vm_control_register->cr4_read_shadow = CR4_BIT_VMXE;
}

/* Set MSR write bitmap to get a VM exit event of modification */
static void zv_vm_set_msr_write_bitmap(
    struct zv_vm_control_register* zv_vm_control_register,
    u64 msr_number
) {
    u64 byte_offset;
	u64 bit_offset;
	u64 bitmap_add = 2048;

	byte_offset = (msr_number & 0xFFFFFFF) / 8;
	bit_offset = (msr_number & 0xFFFFFFF) % 8;

	if (msr_number >= 0xC0000000) bitmap_add += 1024;

	((u8*)zv_vm_control_register->msr_bitmap_addr)[bitmap_add + byte_offset] =
		((u8*)zv_vm_control_register->msr_bitmap_addr)[bitmap_add + byte_offset]
        | (0x01 << bit_offset);
}

/* Setup VMCS */
static void zv_setup_vmcs(
    const struct zv_vm_host_register* zv_vm_host_register,
    const struct zv_vm_guest_register* zv_vm_guest_register,
    const struct zv_vm_control_register* zv_vm_control_register
) {
    int result;
	int cpu_id;
	u64 value;

	cpu_id = smp_processor_id();

    zv_log_write(LOG_DETAIL, "Core", "VM [%d] Setup VMCS", cpu_id);

	/* Setup host information. */
    zv_log_write(LOG_DETAIL, "Core", "  [*] Set Host Register");
    result = zv_write_vmcs(VM_HOST_CR0, zv_vm_host_register->cr0);
	zv_print_vm_result("    [*] CR0", result);
	result = zv_write_vmcs(VM_HOST_CR3, zv_vm_host_register->cr3);
	zv_print_vm_result("    [*] CR3", result);
	result = zv_write_vmcs(VM_HOST_CR4, zv_vm_host_register->cr4);
	zv_print_vm_result("    [*] CR4", result);
	result = zv_write_vmcs(VM_HOST_RSP, zv_vm_host_register->rsp);
	zv_print_vm_result("    [*] RSP", result);
	result = zv_write_vmcs(VM_HOST_RIP, zv_vm_host_register->rip);
	zv_print_vm_result("    [*] RIP", result);
	result = zv_write_vmcs(VM_HOST_CS_SELECTOR, zv_vm_host_register->cs_selector);
	zv_print_vm_result("    [*] CS Selector", result);
	result = zv_write_vmcs(VM_HOST_SS_SELECTOR, zv_vm_host_register->ss_selector);
	zv_print_vm_result("    [*] SS Selector", result);
	result = zv_write_vmcs(VM_HOST_DS_SELECTOR, zv_vm_host_register->ds_selector);
	zv_print_vm_result("    [*] DS Selector", result);
	result = zv_write_vmcs(VM_HOST_ES_SELECTOR, zv_vm_host_register->es_selector);
	zv_print_vm_result("    [*] ES Selector", result);
	result = zv_write_vmcs(VM_HOST_FS_SELECTOR, zv_vm_host_register->fs_selector);
	zv_print_vm_result("    [*] FS Selector", result);
	result = zv_write_vmcs(VM_HOST_GS_SELECTOR, zv_vm_host_register->gs_selector);
	zv_print_vm_result("    [*] GS Selector", result);
	result = zv_write_vmcs(VM_HOST_TR_SELECTOR, zv_vm_host_register->tr_selector);
	zv_print_vm_result("    [*] TR Selector", result);

    result = zv_write_vmcs(VM_HOST_FS_BASE, zv_vm_host_register->fs_base_addr);
	zv_print_vm_result("    [*] FS Base", result);
	result = zv_write_vmcs(VM_HOST_GS_BASE, zv_vm_host_register->gs_base_addr);
	zv_print_vm_result("    [*] GS Base", result);
	result = zv_write_vmcs(VM_HOST_TR_BASE, zv_vm_host_register->tr_base_addr);
	zv_print_vm_result("    [*] TR Base", result);
	result = zv_write_vmcs(VM_HOST_GDTR_BASE, zv_vm_host_register->gdtr_base_addr);
	zv_print_vm_result("    [*] GDTR Base", result);
	result = zv_write_vmcs(VM_HOST_IDTR_BASE, zv_vm_host_register->idtr_base_addr);
	zv_print_vm_result("    [*] IDTR Base", result);

    result = zv_write_vmcs(VM_HOST_IA32_SYSENTER_CS,
		zv_vm_host_register->ia32_sys_enter_cs);
	zv_print_vm_result("    [*] SYSENTER_CS Base", result);
	result = zv_write_vmcs(VM_HOST_IA32_SYSENTER_ESP,
		zv_vm_host_register->ia32_sys_enter_esp);
	zv_print_vm_result("    [*] SYSENTER_ESP", result);
	result = zv_write_vmcs(VM_HOST_IA32_SYSENTER_EIP,
		zv_vm_host_register->ia32_sys_enter_eip);
	zv_print_vm_result("    [*] SYSENTER_EIP", result);
	result = zv_write_vmcs(VM_HOST_PERF_GLOBAL_CTRL,
		zv_vm_host_register->ia32_perf_global_ctrl);
	zv_print_vm_result("    [*] Perf Global Ctrl", result);
	result = zv_write_vmcs(VM_HOST_PAT, zv_vm_host_register->ia32_pat);
	zv_print_vm_result("    [*] PAT", result);
	result = zv_write_vmcs(VM_HOST_EFER, zv_vm_host_register->ia32_efer);
	zv_print_vm_result("    [*] EFER", result);

    /* Setup guest information. */
    zv_log_write(LOG_DETAIL, "Core", "  [*] Set Guest Register");
	result = zv_write_vmcs(VM_GUEST_CR0, zv_vm_guest_register->cr0);
	zv_print_vm_result("    [*] CR0", result);
	result = zv_write_vmcs(VM_GUEST_CR3, zv_vm_guest_register->cr3);
	zv_print_vm_result("    [*] CR3", result);
	result = zv_write_vmcs(VM_GUEST_CR4, zv_vm_guest_register->cr4);
	zv_print_vm_result("    [*] CR4", result);
	result = zv_write_vmcs(VM_GUEST_DR7, zv_vm_guest_register->dr7);
	zv_print_vm_result("    [*] DR7", result);
	result = zv_write_vmcs(VM_GUEST_RSP, zv_vm_guest_register->rsp);
	zv_print_vm_result("    [*] RSP", result);
	result = zv_write_vmcs(VM_GUEST_RIP, zv_vm_guest_register->rip);
	zv_print_vm_result("    [*] RIP", result);
	result = zv_write_vmcs(VM_GUEST_RFLAGS, zv_vm_guest_register->rflags);
	zv_print_vm_result("    [*] RFLAGS", result);
	result = zv_write_vmcs(VM_GUEST_CS_SELECTOR, zv_vm_guest_register->cs_selector);
	zv_print_vm_result("    [*] CS Selector", result);
	result = zv_write_vmcs(VM_GUEST_SS_SELECTOR, zv_vm_guest_register->ss_selector);
	zv_print_vm_result("    [*] SS Selector", result);
	result = zv_write_vmcs(VM_GUEST_DS_SELECTOR, zv_vm_guest_register->ds_selector);
	zv_print_vm_result("    [*] DS Selector", result);
	result = zv_write_vmcs(VM_GUEST_ES_SELECTOR, zv_vm_guest_register->es_selector);
	zv_print_vm_result("    [*] ES Selector", result);
	result = zv_write_vmcs(VM_GUEST_FS_SELECTOR, zv_vm_guest_register->fs_selector);
	zv_print_vm_result("    [*] FS Selector", result);
	result = zv_write_vmcs(VM_GUEST_GS_SELECTOR, zv_vm_guest_register->gs_selector);
	zv_print_vm_result("    [*] GS Selector", result);
	result = zv_write_vmcs(VM_GUEST_LDTR_SELECTOR, zv_vm_guest_register->ldtr_selector);
	zv_print_vm_result("    [*] LDTR Selector", result);
	result = zv_write_vmcs(VM_GUEST_TR_SELECTOR, zv_vm_guest_register->tr_selector);
	zv_print_vm_result("    [*] TR Selector", result);

    result = zv_write_vmcs(VM_GUEST_CS_BASE, zv_vm_guest_register->cs_base_addr);
	zv_print_vm_result("    [*] CS Base", result);
	result = zv_write_vmcs(VM_GUEST_SS_BASE, zv_vm_guest_register->ss_base_addr);
	zv_print_vm_result("    [*] SS Base", result);
	result = zv_write_vmcs(VM_GUEST_DS_BASE, zv_vm_guest_register->ds_base_addr);
	zv_print_vm_result("    [*] DS Base", result);
	result = zv_write_vmcs(VM_GUEST_ES_BASE, zv_vm_guest_register->es_base_addr);
	zv_print_vm_result("    [*] ES Base", result);
	result = zv_write_vmcs(VM_GUEST_FS_BASE, zv_vm_guest_register->fs_base_addr);
	zv_print_vm_result("    [*] FS Base", result);
	result = zv_write_vmcs(VM_GUEST_GS_BASE, zv_vm_guest_register->gs_base_addr);
	zv_print_vm_result("    [*] GS Base", result);
	result = zv_write_vmcs(VM_GUEST_LDTR_BASE, zv_vm_guest_register->ldtr_base_addr);
	zv_print_vm_result("    [*] LDTR Base", result);
	result = zv_write_vmcs(VM_GUEST_TR_BASE, zv_vm_guest_register->tr_base_addr);
	zv_print_vm_result("    [*] TR Base", result);

    result = zv_write_vmcs(VM_GUEST_CS_LIMIT, zv_vm_guest_register->cs_limit);
	zv_print_vm_result("    [*] CS Limit", result);
	result = zv_write_vmcs(VM_GUEST_SS_LIMIT, zv_vm_guest_register->ss_limit);
	zv_print_vm_result("    [*] SS Limit", result);
	result = zv_write_vmcs(VM_GUEST_DS_LIMIT, zv_vm_guest_register->ds_limit);
	zv_print_vm_result("    [*] DS Limit", result);
	result = zv_write_vmcs(VM_GUEST_ES_LIMIT, zv_vm_guest_register->es_limit);
	zv_print_vm_result("    [*] ES Limit", result);
	result = zv_write_vmcs(VM_GUEST_FS_LIMIT, zv_vm_guest_register->fs_limit);
	zv_print_vm_result("    [*] FS Limit", result);
	result = zv_write_vmcs(VM_GUEST_GS_LIMIT, zv_vm_guest_register->gs_limit);
	zv_print_vm_result("    [*] GS Limit", result);
	result = zv_write_vmcs(VM_GUEST_LDTR_LIMIT, zv_vm_guest_register->ldtr_limit);
	zv_print_vm_result("    [*] LDTR Limit", result);
	result = zv_write_vmcs(VM_GUEST_TR_LIMIT, zv_vm_guest_register->tr_limit);
	zv_print_vm_result("    [*] TR Limit", result);

    result = zv_write_vmcs(VM_GUEST_CS_ACC_RIGHT, zv_vm_guest_register->cs_access);
	zv_print_vm_result("    [*] CS Access", result);
	result = zv_write_vmcs(VM_GUEST_SS_ACC_RIGHT, zv_vm_guest_register->ss_access);
	zv_print_vm_result("    [*] SS Access", result);
	result = zv_write_vmcs(VM_GUEST_DS_ACC_RIGHT, zv_vm_guest_register->ds_access);
	zv_print_vm_result("    [*] DS Access", result);
	result = zv_write_vmcs(VM_GUEST_ES_ACC_RIGHT, zv_vm_guest_register->es_access);
	zv_print_vm_result("    [*] ES Access", result);
	result = zv_write_vmcs(VM_GUEST_FS_ACC_RIGHT, zv_vm_guest_register->fs_access);
	zv_print_vm_result("    [*] FS Access", result);
	result = zv_write_vmcs(VM_GUEST_GS_ACC_RIGHT, zv_vm_guest_register->gs_access);
	zv_print_vm_result("    [*] GS Access", result);
	result = zv_write_vmcs(VM_GUEST_LDTR_ACC_RIGHT, zv_vm_guest_register->ldtr_access);
	zv_print_vm_result("    [*] LDTR Access", result);
	result = zv_write_vmcs(VM_GUEST_TR_ACC_RIGHT, zv_vm_guest_register->tr_access);
	zv_print_vm_result("    [*] TR Access", result);

    result = zv_write_vmcs(VM_GUEST_GDTR_BASE, zv_vm_guest_register->gdtr_base_addr);
	zv_print_vm_result("    [*] GDTR Base", result);
	result = zv_write_vmcs(VM_GUEST_IDTR_BASE, zv_vm_guest_register->idtr_base_addr);
	zv_print_vm_result("    [*] IDTR Base", result);
	result = zv_write_vmcs(VM_GUEST_GDTR_LIMIT, zv_vm_guest_register->gdtr_limit);
	zv_print_vm_result("    [*] GDTR Base", result);
	result = zv_write_vmcs(VM_GUEST_IDTR_LIMIT, zv_vm_guest_register->idtr_limit);
	zv_print_vm_result("    [*] IDTR Base", result);

    result = zv_write_vmcs(VM_GUEST_DEBUGCTL, zv_vm_guest_register->ia32_debug_ctrl);
	zv_print_vm_result("    [*] DEBUG CONTROL", result);
	result = zv_write_vmcs(VM_GUEST_IA32_SYSENTER_CS,
		zv_vm_guest_register->ia32_sys_enter_cs);
	zv_print_vm_result("    [*] SYSENTER_CS Base", result);
	result = zv_write_vmcs(VM_GUEST_IA32_SYSENTER_ESP,
		zv_vm_guest_register->ia32_sys_enter_esp);
	zv_print_vm_result("    [*] SYSENTER_ESP", result);
	result = zv_write_vmcs(VM_GUEST_IA32_SYSENTER_EIP,
		zv_vm_guest_register->ia32_sys_enter_eip);
	zv_print_vm_result("    [*] SYSENTER_EIP", result);
	result = zv_write_vmcs(VM_GUEST_PERF_GLOBAL_CTRL,
		zv_vm_guest_register->ia32_perf_global_ctrl);
	zv_print_vm_result("    [*] Perf Global Ctrl", result);
	result = zv_write_vmcs(VM_GUEST_PAT, zv_vm_guest_register->ia32_pat);
	zv_print_vm_result("    [*] PAT", result);
	result = zv_write_vmcs(VM_GUEST_EFER, zv_vm_guest_register->ia32_efer);
	zv_print_vm_result("    [*] EFER", result);

    result = zv_write_vmcs(VM_VMCS_LINK_PTR, zv_vm_guest_register->vmcs_link_ptr);
	zv_print_vm_result("    [*] VMCS Link ptr", result);

	result = zv_write_vmcs(VM_GUEST_INT_STATE, 0);
	zv_print_vm_result("    [*] Guest Int State", result);

	result = zv_write_vmcs(VM_GUEST_ACTIVITY_STATE, 0);
	zv_print_vm_result("    [*] Guest Activity State", result);

	result = zv_write_vmcs(VM_GUEST_SMBASE, 0);
	zv_print_vm_result("    [*] Guest SMBase", result);

	result = zv_write_vmcs(VM_GUEST_PENDING_DBG_EXCEPTS, 0);
	zv_print_vm_result("    [*] Pending DBG Excepts", result);

	value = zv_calc_vm_pre_timer_value();
	result = zv_write_vmcs(VM_GUEST_VMX_PRE_TIMER_VALUE, value);
	zv_print_vm_result("    [*] VM Preemption Timer", result);

    /* Setup VM control information. */
    zv_log_write(LOG_DETAIL, "Core", "  [*] Set VM Control Register");
	result = zv_write_vmcs(VM_CTRL_PIN_BASED_VM_EXE_CTRL,
		zv_vm_control_register->pin_based_ctrl);
	zv_print_vm_result("    [*] PIN Based Ctrl", result);
	result = zv_write_vmcs(VM_CTRL_PRI_PROC_BASED_EXE_CTRL,
		zv_vm_control_register->pri_proc_based_ctrl);
	zv_print_vm_result("    [*] Primary Process Based Ctrl", result);
	result = zv_write_vmcs(VM_CTRL_SEC_PROC_BASED_EXE_CTRL,
		zv_vm_control_register->sec_proc_based_ctrl);
	zv_print_vm_result("    [*] Secondary Process Based Ctrl", result);
	result = zv_write_vmcs(VM_CTRL_EXCEPTION_BITMAP,
		zv_vm_control_register->except_bitmap);
	zv_print_vm_result("    [*] Exception Bitmap", result);
	result = zv_write_vmcs(VM_CTRL_IO_BITMAP_A_ADDR,
		zv_vm_control_register->io_bitmap_addrA);
	zv_print_vm_result("    [*] IO Bitmap A", result);
	result = zv_write_vmcs(VM_CTRL_IO_BITMAP_B_ADDR,
		zv_vm_control_register->io_bitmap_addrB);
	zv_print_vm_result("    [*] IO Bitmap B", result);
	result = zv_write_vmcs(VM_CTRL_EPT_PTR, zv_vm_control_register->ept_ptr);
	zv_print_vm_result("    [*] EPT Ptr", result);
	result = zv_write_vmcs(VM_CTRL_MSR_BITMAPS,
		zv_vm_control_register->msr_bitmap_addr);
	zv_print_vm_result("    [*] MSR Bitmap", result);
	result = zv_write_vmcs(VM_CTRL_VM_ENTRY_CTRLS,
		zv_vm_control_register->vm_entry_ctrl_field);
	zv_print_vm_result("    [*] VM Entry Control", result);
	result = zv_write_vmcs(VM_CTRL_VM_EXIT_CTRLS,
		zv_vm_control_register->vm_exti_ctrl_field);
	zv_print_vm_result("    [*] VM Exit Control", result);
	result = zv_write_vmcs(VM_CTRL_VIRTUAL_APIC_ADDR,
		zv_vm_control_register->virt_apic_page_addr);
	zv_print_vm_result("    [*] Virtual APIC Page", result);
	result = zv_write_vmcs(VM_CTRL_CR0_GUEST_HOST_MASK, 0);
	zv_print_vm_result("    [*] CR0 Guest Host Mask", result);
	result = zv_write_vmcs(VM_CTRL_CR4_GUEST_HOST_MASK,
		zv_vm_control_register->cr4_guest_host_mask);
	zv_print_vm_result("    [*] CR4 Guest Host Mask", result);
	result = zv_write_vmcs(VM_CTRL_CR0_READ_SHADOW, 0);
	zv_print_vm_result("    [*] CR0 Read Shadow", result);
	result = zv_write_vmcs(VM_CTRL_CR4_READ_SHADOW,
		zv_vm_control_register->cr4_read_shadow);
	zv_print_vm_result("    [*] CR4 Read Shadow", result);
	result = zv_write_vmcs(VM_CTRL_CR3_TARGET_VALUE_0, 0);
	zv_print_vm_result("    [*] CR3 Target Value 0", result);
	result = zv_write_vmcs(VM_CTRL_CR3_TARGET_VALUE_1, 0);
	zv_print_vm_result("    [*] CR3 Target Value 1", result);
	result = zv_write_vmcs(VM_CTRL_CR3_TARGET_VALUE_2, 0);
	zv_print_vm_result("    [*] CR3 Target Value 2", result);
	result = zv_write_vmcs(VM_CTRL_CR3_TARGET_VALUE_3, 0);
	zv_print_vm_result("    [*] CR3 Target Value 3", result);

    result = zv_write_vmcs(VM_CTRL_PAGE_FAULT_ERR_CODE_MASK, 0);
	zv_print_vm_result("    [*] Page Fault Error Code Mask", result);
	result = zv_write_vmcs(VM_CTRL_PAGE_FAULT_ERR_CODE_MATCH, 0);
	zv_print_vm_result("    [*] Page Fault Error Code Match", result);
	result = zv_write_vmcs(VM_CTRL_CR3_TARGET_COUNT, 0);
	zv_print_vm_result("    [*] CR3 Target Count", result);
	result = zv_write_vmcs(VM_CTRL_VM_EXIT_MSR_STORE_COUNT, 0);
	zv_print_vm_result("    [*] MSR Store Count", result);
	result = zv_write_vmcs(VM_CTRL_VM_EXIT_MSR_LOAD_COUNT, 0);
	zv_print_vm_result("    [*] MSR Load Count", result);
	result = zv_write_vmcs(VM_CTRL_VM_EXIT_MSR_LOAD_ADDR, 0);
	zv_print_vm_result("    [*] MSR Load Addr", result);
	result = zv_write_vmcs(VM_CTRL_VM_ENTRY_INT_INFO_FIELD, 0);
	zv_print_vm_result("    [*] VM Entry Int Info Field", result);
	result = zv_write_vmcs(VM_CTRL_VM_ENTRY_EXCEPT_ERR_CODE, 0);
	zv_print_vm_result("    [*] VM Entry Except Err Code", result);
	result = zv_write_vmcs(VM_CTRL_VM_ENTRY_INST_LENGTH, 0);
	zv_print_vm_result("    [*] VM Entry Inst Length", result);
	result = zv_write_vmcs(VM_CTRL_VM_ENTRY_MSR_LOAD_COUNT, 0);
	zv_print_vm_result("    [*] VM Entry MSR Load Count", result);
	result = zv_write_vmcs(VM_CTRL_VM_ENTRY_MSR_LOAD_ADDR, 0);
	zv_print_vm_result("    [*] VM Entry MSR Load Addr", result);

    result = zv_write_vmcs(VM_CTRL_TPR_THRESHOLD, 0);
	zv_print_vm_result("    [*] TPR Threashold", result);
	result = zv_write_vmcs(VM_CTRL_EXECUTIVE_VMCS_PTR, 0);
	zv_print_vm_result("    [*] Executive VMCS Ptr", result);
	result = zv_write_vmcs(VM_CTRL_TSC_OFFSET, 0);
	zv_print_vm_result("    [*] TSC Offset", result);
}

/* Print message with result `*/
static void zv_print_vm_result(const char* string, int result)
{
	return;

	if (result) {
        zv_log_write(LOG_DETAIL, "Core", "%s Success", string);
	} else {
        zv_log_write(LOG_NONE, "Core", "%s Fail", string);
	}
}

/* Calculate preemption timer value */
u64 zv_calc_vm_pre_timer_value(void) {
    u64 scale;

    scale = zv_rdmsr(MSR_IA32_VMX_MISC);
    scale &= 0x1F;

    return (VM_PRE_TIMER_VALUE >> scale);
}

/*
 * Duplicate page tabel for the host.
 *
 * Memory space of Linux kernel is as follows.
 * 0000000000000000 - 00007fffffffffff (=47 bits) user space, different per mm
 * ffff800000000000 - ffff80ffffffffff (=40 bits) guard hole
 * ffff880000000000 - ffffc7ffffffffff (=64 TB) direct mapping of all phys. memory
 * ffffc80000000000 - ffffc8ffffffffff (=40 bits) hole
 * ffffc90000000000 - ffffe8ffffffffff (=45 bits) vmalloc/ioremap space
 * ffffe90000000000 - ffffe9ffffffffff (=40 bits) hole
 * ffffea0000000000 - ffffeaffffffffff (=40 bits) virtual memory map (1TB)
 * ffffffff80000000 - ffffffffa0000000 (=512 MB)  kernel text mapping, from phys 0
 * ffffffffa0000000 - fffffffffff00000 (=1536 MB) module mapping space
 * 
 */
static void zv_dup_page_table_for_host(void) {
    struct zv_pagetable* org_pml4;
	struct zv_pagetable* org_pdpte_pd;
	struct zv_pagetable* org_pdept;
	struct zv_pagetable* org_pte;
	struct zv_pagetable* vm_pml4;
	struct zv_pagetable* vm_pdpte_pd;
	struct zv_pagetable* vm_pdept;
	struct zv_pagetable* vm_pte;
	int i, j, k;
	struct mm_struct* swapper_mm;
	u64 cur_addr;

    zv_log_write(LOG_DEBUG, "Core", "Duplicate page tables");

    swapper_mm = (struct mm_struct*)zv_get_symbol_address("init_mm");
    if (! swapper_mm) {
        zv_log_write(LOG_NONE, "Core", "Failed to get init_mm");
        return;
    }

    org_pml4 = (struct zv_pagetable*)swapper_mm->pgd;
    g_vm_init_phy_pml4 = virt_to_phys(org_pml4);

    zv_log_write(LOG_DEBUG, "Core", "init_mm: %0x16lX", swapper_mm);
    zv_log_write(LOG_DEBUG, "Core", "init_mm.pgd: %0x16lX", org_pml4);

    vm_pml4 = zv_kmalloc(0x1000, GFP_KERNEL);
    memset(vm_pml4, 0, 0x1000);

    g_vm_host_phy_pml4 = virt_to_phys(vm_pml4);

    zv_log_write(LOG_DEBUG, "Core", "PML4 Logical: %016lX Physical %016lX",
        vm_pml4, g_vm_host_phy_pml4);
    
    /* Create page tables */
    for (i = 0; i < 512; i ++) {
		cur_addr = i * VAL_512GB;

		if ((org_pml4->entry[i] == 0)
            || (org_pml4->entry[i] & MASK_PAGE_SIZE_FLAG)
        ) {
			vm_pml4->entry[i] = org_pml4->entry[i];
			continue;
		}

		/* Direct mapped area (0xffff880000000000 ~ 0xffffc7ffffffffff) are
		 * mapped 1:1 and 512GB size entry.
		 */
		if (((u64)0xffff880000000000 <= cur_addr)
            && (cur_addr < (u64)0xffffc80000000000)
        ) {
			vm_pml4->entry[i] = cur_addr
                | MASK_PAGE_SIZE_FLAG
                | MASK_PRESENT_FLAG
                | MASK_XD_FLAG;

			continue;
		}

		/* Allocate PDPTE_PD and copy */
		vm_pml4->entry[i] = (u64)zv_kmalloc(0x1000, GFP_KERNEL);
		memset((void*)vm_pml4->entry[i], 0, 0x1000);
		vm_pml4->entry[i] = virt_to_phys((void*)(vm_pml4->entry[i]));
		vm_pml4->entry[i] |= org_pml4->entry[i] & MASK_PAGEFLAG;

		/* Run loop to copy PDEPT */
		org_pdpte_pd = (struct zv_pagetable*)(org_pml4->entry[i] & ~(MASK_PAGEFLAG));
		vm_pdpte_pd = (struct zv_pagetable*)(vm_pml4->entry[i] & ~(MASK_PAGEFLAG));
		org_pdpte_pd = phys_to_virt((u64)org_pdpte_pd);
		vm_pdpte_pd = phys_to_virt((u64)vm_pdpte_pd);
        
        zv_log_write(LOG_DETAIL, "Core", "  [*] PML4[%d] %16lX %16lp %16lp",
            i, org_pml4->entry[i], org_pdpte_pd, vm_pdpte_pd);

		for (j = 0; j < 512; j ++) {
			if ((org_pdpte_pd->entry[j] == 0)
                || (org_pdpte_pd->entry[j] & MASK_PAGE_SIZE_FLAG)
            ) {
				vm_pdpte_pd->entry[j] = org_pdpte_pd->entry[j];
				continue;
			}

			/* Allocate PDEPT and copy */
			vm_pdpte_pd->entry[j] = (u64)zv_kmalloc(0x1000, GFP_KERNEL);
			memset((void*)vm_pdpte_pd->entry[j], 0, 0x1000);
			vm_pdpte_pd->entry[j] = virt_to_phys((void*)(vm_pdpte_pd->entry[j]));
			vm_pdpte_pd->entry[j] |= org_pdpte_pd->entry[j] & MASK_PAGEFLAG;

			/* Run loop to copy PDEPT */
			org_pdept = (struct zv_pagetable*)(org_pdpte_pd->entry[j] & ~(MASK_PAGEFLAG));
			vm_pdept = (struct zv_pagetable*)(vm_pdpte_pd->entry[j] & ~(MASK_PAGEFLAG));
			org_pdept = phys_to_virt((u64)org_pdept);
			vm_pdept = phys_to_virt((u64)vm_pdept);

            zv_log_write(LOG_DETAIL, "Core", "  [*] PDPTE_PD[%d] %016lX %016lp %016lp",
                j, org_pdpte_pd->entry[j], org_pdept, vm_pdept);

            for (k = 0; k < 512; k ++) {
				if ((org_pdept->entry[k] == 0)
                    || (org_pdept->entry[k] & MASK_PAGE_SIZE_FLAG)
                ) {
					vm_pdept->entry[k] = org_pdept->entry[k];
					continue;
				}

				/* Allocate PTE and copy */
				vm_pdept->entry[k] = (u64)zv_kmalloc(0x1000, GFP_KERNEL);
				memset((void*)vm_pdept->entry[k], 0, 0x1000);
				vm_pdept->entry[k] = virt_to_phys((void*)(vm_pdept->entry[k]));
				vm_pdept->entry[k] |= org_pdept->entry[k] & MASK_PAGEFLAG;

				/* Run loop to copy PTE */
				org_pte = (struct zv_pagetable*)(org_pdept->entry[k] & ~(MASK_PAGEFLAG));
				vm_pte = (struct zv_pagetable*)(vm_pdept->entry[k] & ~(MASK_PAGEFLAG));
				org_pte = phys_to_virt((u64)org_pte);
				vm_pte = phys_to_virt((u64)vm_pte);

                zv_log_write(LOG_DETAIL, "Core", "  [*] PDEPT[%d] %016lX %016lp %016lp",
                    k, org_pdept->entry[k], org_pte, vm_pte);
				memcpy(vm_pte, org_pte, 0x1000);
			}
		}
	}
}

module_init(zeroVisor_init);
module_exit(zeroVisor_exit);

MODULE_AUTHOR("Hubert Yao");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("zeroVisor: A Type1.5 hypervisor skeleton —— Everything starts from zero.");
MODULE_VERSION("1.0");