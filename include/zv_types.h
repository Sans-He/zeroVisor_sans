#include <linux/types.h>

#include "zv_config.h"

/* Symbol table entry structure */
struct zv_symbol_table_entry {
    char* name;
    u64 addr;
};

/* Symbol table structure */
struct zv_symbol_table {
    struct zv_symbol_table_entry symbol[SYMBOL_MAX_COUNT];
};

/* ZeroVisor Memory Pool structure */
struct zv_memory_pool {
    u64 max_count;
    u64 pop_index;
    u64* pool;
};

/* Host registers for VMCS */
struct zv_vm_host_register
{
	u64 cr0;
	u64 cr3;
	u64 cr4;
	u64 rsp;
	u64 rip;
	u64 cs_selector;
	u64 ss_selector;
	u64 ds_selector;
	u64 es_selector;
	u64 fs_selector;
	u64 gs_selector;
	u64 tr_selector;

	u64 fs_base_addr;
	u64 gs_base_addr;
	u64 tr_base_addr;
	u64 gdtr_base_addr;
	u64 idtr_base_addr;

	u64 ia32_sys_enter_cs;
	u64 ia32_sys_enter_esp;
	u64 ia32_sys_enter_eip;
	u64 ia32_perf_global_ctrl;
	u64 ia32_pat;
	u64 ia32_efer;
};

/* Guest registers for VMCS. */
struct zv_vm_guest_register
{
	u64 cr0;
	u64 cr3;
	u64 cr4;
	u64 dr7;
	u64 rsp;
	u64 rip;
	u64 rflags;
	u64 cs_selector;
	u64 ss_selector;
	u64 ds_selector;
	u64 es_selector;
	u64 fs_selector;
	u64 gs_selector;
	u64 ldtr_selector;
	u64 tr_selector;

	u64 cs_base_addr;
	u64 ss_base_addr;
	u64 ds_base_addr;
	u64 es_base_addr;
	u64 fs_base_addr;
	u64 gs_base_addr;
	u64 ldtr_base_addr;
	u64 tr_base_addr;

	u64 cs_limit;
	u64 ss_limit;
	u64 ds_limit;
	u64 es_limit;
	u64 fs_limit;
	u64 gs_limit;
	u64 ldtr_limit;
	u64 tr_limit;

	u64 cs_access;
	u64 ss_access;
	u64 ds_access;
	u64 es_access;
	u64 fs_access;
	u64 gs_access;
	u64 ldtr_access;
	u64 tr_access;

	u64 gdtr_base_addr;
	u64 idtr_base_addr;
	u64 gdtr_limit;
	u64 idtr_limit;

	u64 ia32_debug_ctrl;
	u64 ia32_sys_enter_cs;
	u64 ia32_sys_enter_esp;
	u64 ia32_sys_enter_eip;
	u64 vmcs_link_ptr;

	u64 ia32_perf_global_ctrl;
	u64 ia32_pat;
	u64 ia32_efer;
};

/* VM control registers for VMCS. */
struct zv_vm_control_register
{
	u64 pin_based_ctrl;
	u64 pri_proc_based_ctrl;
	u64 sec_proc_based_ctrl;
	u64 except_bitmap;
	u64 io_bitmap_addrA;
	u64 io_bitmap_addrB;
	u64 ept_ptr;
	u64 msr_bitmap_addr;
	u64 vm_entry_ctrl_field;
	u64 vm_exit_ctrl_field;
	u64 virt_apic_page_addr;
	u64 cr4_guest_host_mask;
	u64 cr4_read_shadow;
};

/* Guest registers (context) for VM exit. */
struct zv_vm_exit_guest_register
{
	u64 r15;
	u64 r14;
	u64 r13;
	u64 r12;
	u64 r11;
	u64 r10;
	u64 r9;
	u64 r8;
	u64 rsi;
	u64 rdi;
	u64 rdx;
	u64 rcx;
	u64 rbx;
	u64 rax;
	u64 rbp;
};