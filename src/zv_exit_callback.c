/* 
 * In view of the modular nature of VM_Exit Callback processing
 * and the amount of code, split it separately into this file. 
 *
 */

#include <linux/smp.h>
#include <linux/io.h>
#include <asm/desc.h>

#include <../include/zv_config.h>
#include <../include/zv_types.h>
#include <../include/zv_log.h>
#include <../include/zv_core.h>
#include <../include/zv_mmu.h>
#include <../include/asm.h>
#include <../include/zv_exit_callback.h>

/* Shutdown Variables */
int g_is_shutdown_trigger_set = 0;
volatile u64 g_shutdown_jiffies = 0;

/* Static funtions definition */
static void zv_trigger_shutdown_timer(void);
static int zv_is_system_shutdowning(void);
static int zv_check_shutdown_timer_expired(void);
static void zv_advance_vm_guest_rip(void);
static void zv_remove_int1_exception_from_vm(void);
static u64 zv_get_reg_value_from_index(
    struct zv_vm_exit_guest_register* guest_context,
    int index
);
static void zv_set_reg_value_from_index(
    struct zv_vm_exit_guest_register* guest_context,
    int index,
    u64 reg_value
);
static void zv_disable_desc_monitor(void);
static u64 zv_calc_dest_mem_addr(
    struct zv_vm_exit_guest_register* guest_context,
    u64 inst_info
);
static u64 zv_get_value_from_memory(u64 inst_info, u64 addr);
static void zv_set_value_to_memory(u64 inst_info, u64 addr, u64 value);
static int zv_check_gdtr(int cpu_id);



// exit_callback
static void zv_vm_exit_callback_interrupts(
    int cpu_id,
    u64 exit_qual,
    struct zv_vm_exit_guest_register* guest_context
);
static void zv_vm_exit_callback_interrupt_debug(
    int cpu_id,
    u64 exit_qual,
    struct zv_vm_exit_guest_register* guest_context
);
static void zv_vm_exit_callback_interrupt_breakpoint(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context
);
static void zv_vm_exit_callback_init_signal(int cpu_id);
static void zv_vm_exit_callback_starup_signal(int cpu_id);
static void zv_vm_exit_callback_cpuid(struct zv_vm_exit_guest_register* guest_context);
static void zv_vm_exit_callback_invd(void);
static void zv_vm_exit_callback_access_cr(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context,
    u64 exit_reason,
    u64 exit_qual
);
static void zv_vm_exit_callback_wrmsr(int cpu_id);
static void zv_vm_exit_callback_access_gdtr_idtr(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context
);
static void zv_vm_exit_callback_access_ldtr_tr(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context
);
static void zv_vm_exit_callback_ept_violation(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context,
    u64 exit_reason,
    u64 exit_qual,
    u64 guest_linear,
    u64 guest_physical
);
static void zv_vm_exit_callback_pre_timer_expired(int cpu_id);
static void zv_vm_exit_callback_vmcall(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context
);
static void zv_shutdown_vm_this_core(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context
);
static void zv_fill_context_from_vm_guest(
    struct zv_vm_exit_guest_register* guest_context,
    struct zv_vm_full_context* full_context
);
static void zv_restore_context_from_vm_guest(
    int cpu_id,
    struct zv_vm_full_context* full_context,
    u64 guest_rsp
);

/* Process vm_exit event */
void zv_vm_exit_callback(struct zv_vm_exit_guest_register* guest_context) {
    u64 exit_reason;
	u64 exit_qual;
	u64 guest_linear;
	u64 guest_physical;
	int cpu_id;
	u64 info_field;

    cpu_id = smp_processor_id();
    zv_read_vmcs(VM_DATA_EXIT_REASON, &exit_reason);
	zv_read_vmcs(VM_DATA_EXIT_QUALIFICATION, &exit_qual);
	zv_read_vmcs(VM_DATA_GUEST_LINEAR_ADDR, &guest_linear);
	zv_read_vmcs(VM_DATA_GUEST_PHY_ADDR, &guest_physical);

	zv_read_vmcs(VM_DATA_VM_EXIT_INT_INFO, &info_field);
    zv_log_write(LOG_DETAIL, "Core", "VM [%d] Exit interrupt Info Field: %016lX", 
        cpu_id, info_field);

    /* Check system is shutdowning and shutdown timer is expired */
    zv_trigger_shutdown_timer();
    zv_check_shutdown_timer_expired();

    zv_write_vmcs(VM_CTRL_VM_ENTRY_INST_LENGTH, 0);

    switch(exit_reason & 0xFFFF) {
        case VM_EXIT_REASON_EXCEPT_OR_NMI:
            zv_vm_exit_callback_interrupts(cpu_id, exit_qual, guest_context);
            break;
        
        case VM_EXIT_REASON_EXT_INTTERUPT:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] Externel Interrupt", cpu_id);
            break;
        
        case VM_EXIT_REASON_TRIPLE_FAULT:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] Triple fault", cpu_id);
            zv_advance_vm_guest_rip();
            break;
        
        case VM_EXIT_REASON_INIT_SIGNAL:
            zv_vm_exit_callback_init_signal(cpu_id);
            break;
        
        case VM_EXIT_REASON_START_UP_IPI:
            zv_vm_exit_callback_starup_signal(cpu_id);
            break;

        case VM_EXIT_REASON_IO_SMI:
        case VM_EXIT_REASON_OTHER_SMI:
        case VM_EXIT_REASON_INT_WINDOW:
        case VM_EXIT_REASON_NMI_WINDOW:
        case VM_EXIT_REASON_TASK_SWITCH:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] VM_EXIT_REASON_IO_SMI, INT, NMI",
                cpu_id);
            zv_advance_vm_guest_rip();
            break;
        
        case VM_EXIT_REASON_CPUID:
            zv_vm_exit_callback_cpuid(guest_context);
            break;
        
        case VM_EXIT_REASON_GETSEC:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] Instruction GETSEC call", cpu_id);
            zv_advance_vm_guest_rip();
            break;
        
        case VM_EXIT_REASON_HLT:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] Instruction HLT call", cpu_id);
            zv_advance_vm_guest_rip();
            break;
        
        case VM_EXIT_REASON_INVD:
            zv_vm_exit_callback_invd();
            break;

        case VM_EXIT_REASON_INVLPG:
        case VM_EXIT_REASON_RDPMC:
        case VM_EXIT_REASON_RDTSC:
        case VM_EXIT_REASON_RSM:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] VM_EXIT_REASON_INVLPG, "
				"RDPMC, RDTSC, RSM", cpu_id);
            zv_advance_vm_guest_rip();
            break;

        case VM_EXIT_REASON_VMCALL:
            zv_vm_exit_callback_vmcall(cpu_id, guest_context);
            break;

        case VM_EXIT_REASON_VMCLEAR:
        case VM_EXIT_REASON_VMLAUNCH:
        case VM_EXIT_REASON_VMPTRLD:
        case VM_EXIT_REASON_VMPTRST:
        case VM_EXIT_REASON_VMREAD:
        case VM_EXIT_REASON_VMRESUME:
        case VM_EXIT_REASON_VMWRITE:
        case VM_EXIT_REASON_VMXOFF:
        case VM_EXIT_REASON_VMXON:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] Vritualization Instruction Detected", cpu_id);
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] Skip VT Instruction", cpu_id);
            zv_advance_vm_guest_rip();
            break;
        
        case VM_EXIT_REASON_CTRL_REG_ACCESS:
            zv_vm_exit_callback_access_cr(cpu_id, guest_context, exit_reason, exit_qual);
            break;

        case VM_EXIT_REASON_MOV_DR:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] Detect MOVE DR", cpu_id);
            zv_advance_vm_guest_rip();
            break;
        
        case VM_EXIT_REASON_IO_INST:
        case VM_EXIT_REASON_RDMSR:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] VM_EXIT_REASON_IO_INST, "
                "RDMSR", cpu_id);
            zv_advance_vm_guest_rip();
            break;

        case VM_EXIT_REASON_WRMSR:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] VM_EXIT_REASON_WRMSR", cpu_id);
            zv_vm_exit_callback_wrmsr(cpu_id);
            break;

        case VM_EXIT_REASON_VM_ENTRY_FAILURE_INV_GUEST:
        case VM_EXIT_REASON_VM_ENTRY_FAILURE_MSR_LOAD:
        case VM_EXIT_REASON_MWAIT:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] VM_EXIT_REASON_VM_ENTRY_"
				"FAILURE_INV_GUEST, MSR_LOAD", cpu_id);
            zv_advance_vm_guest_rip();
            break;

        /* For hardware breakpoint interoperation */
		case VM_EXIT_REASON_MONITOR_TRAP_FLAG:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] VM_EXIT_REASON_TRAP_FLAG", cpu_id);
            zv_advance_vm_guest_rip();
            break;

        case VM_EXIT_REASON_MONITOR:
        case VM_EXIT_REASON_PAUSE:
        case VM_EXIT_REASON_VM_ENTRY_FAILURE_MACHINE_CHECK:
        case VM_EXIT_REASON_TRP_BELOW_THRESHOLD:
        case VM_EXIT_REASON_APIC_ACCESS:
        case VM_EXIT_REASON_VIRTUALIZED_EOI:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] VM_EXIT_REASON_MONITOR, "
				"PAUSE, VM_ENTRY_FAILURE_MACHINE_CHECK,...", cpu_id);
            zv_advance_vm_guest_rip();
            break;

		case VM_EXIT_REASON_ACCESS_GDTR_OR_IDTR:
            zv_vm_exit_callback_access_gdtr_idtr(cpu_id, guest_context);
            break;
        
		case VM_EXIT_REASON_ACCESS_LDTR_OR_TR:
            zv_vm_exit_callback_access_ldtr_tr(cpu_id, guest_context);
            break;

        case VM_EXIT_REASON_EPT_VIOLATION:
            zv_vm_exit_callback_ept_violation(
                cpu_id,
                guest_context,
                exit_reason,
                exit_qual,
                guest_linear,
                guest_physical
            );
            break;

        case VM_EXIT_REASON_EPT_MISCONFIGURATION:
        case VM_EXIT_REASON_INVEPT:
        case VM_EXIT_REASON_RDTSCP:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] VM_EXIT_REASON_MISCONFIGURATION, "
                "INVEPT, RDTSCP", cpu_id);
            zv_advance_vm_guest_rip();
            break;
        
        case VM_EXIT_REASON_VMX_PREEMP_TIMER_EXPIRED:
            zv_vm_exit_callback_pre_timer_expired(cpu_id);
            break;

        case VM_EXIT_REASON_INVVPID:
        case VM_EXIT_REASON_WBINVD:
        case VM_EXIT_REASON_XSETBV:
        case VM_EXIT_REASON_APIC_WRITE:
        case VM_EXIT_REASON_RDRAND:
        case VM_EXIT_REASON_INVPCID:
        case VM_EXIT_REASON_VMFUNC:
        case VM_EXIT_REASON_RDSEED:
        case VM_EXIT_REASON_XSAVES:
        case VM_EXIT_REASON_XRSTORS:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] INVVPID ...", cpu_id);
            zv_advance_vm_guest_rip();
            break;

        default:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] VM EXIT REASON DEFAULT !",
                cpu_id);
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] Exit Reason: %d - %016lX",
                cpu_id, (u32)exit_reason, exit_reason);
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] VM [%d] Exit Qualification: %d - %016lX",
				cpu_id, (u32)exit_qual, exit_qual);
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] Guest Linear: %d - %016lX",
				cpu_id, (u32)guest_linear, guest_linear);
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] Guest Physical: %d - %016lX",
				cpu_id, (u32)guest_physical, guest_physical);
            zv_advance_vm_guest_rip();
            break;
    }
}

/* Trigger shutdown timer if the system is shutdowning */
static void zv_trigger_shutdown_timer(void) {
    if (zv_is_system_shutdowning() == 0) return;

    if (g_is_shutdown_trigger_set == 0) {
        g_is_shutdown_trigger_set = 1;
        g_shutdown_jiffies = jiffies;
    }
    
    return;
}

/* Check and return shutdown status */
static int zv_is_system_shutdowning(void) {
    // int cpu_id = smp_processor_id();

    if ((system_state == SYSTEM_RUNNING)
        || (system_state == SYSTEM_BOOTING)
    ) {
        return 0;
    }

    return 1;
}

/* Check time is over after the shutdown timer is triggered */
static int zv_check_shutdown_timer_expired(void) {
    u64 value;
    
    if (g_is_shutdown_trigger_set == 0) return 0;

    value = jiffies - g_shutdown_jiffies;

    if (jiffies_to_msecs(value) >= SHUTDOWN_TIME_LIMIT_MS) {
        zv_log_write(LOG_NONE, "VMExit", "VM [%d] Shutdown timer is expired", 
            smp_processor_id());
        zv_log_error(ERROR_SHUTDOWN_TIME_OUT);

        g_shutdown_jiffies = jiffies;
        return 1;
    }

    return 0;
}

/* Skip guest instruction */
static void zv_advance_vm_guest_rip(void) {
    u64 inst_delta;
    u64 rip;

    zv_read_vmcs(VM_DATA_VM_EXIT_INST_LENGTH, &inst_delta);
    zv_read_vmcs(VM_GUEST_RIP, &rip);
    zv_log_write(LOG_DETAIL, "VMExit", "VM_DATA_VM_EXIT_INST_LENGTH: %016lX, "
		"VM_GUEST_RIP: %016lX", inst_delta, rip);
    zv_write_vmcs(VM_GUEST_RIP, rip + inst_delta);
}

/* Process interrupt callback */
static void zv_vm_exit_callback_interrupts(
    int cpu_id,
    u64 exit_qual,
    struct zv_vm_exit_guest_register* guest_context
) {
    u64 info_field;
    u32 int_type;
    u32 vector;
    u64 guest_rip;

    // Read VM Exit interrupt information field
    zv_read_vmcs(VM_DATA_VM_EXIT_INT_INFO, &info_field);
    zv_read_vmcs(VM_GUEST_RIP, &guest_rip);
    
    /* interupt type */
    int_type = VM_EXIT_INT_INFO_INT_TYPE(info_field);
    /* interrupts vector ID */
    vector = VM_EXIT_INT_INFO_VECTOR(info_field);
    
    zv_log_write(LOG_DETAIL, "VMExit", "VM [%d] Exception/Interrupt: type=%d, vector=%d", 
        cpu_id, int_type, vector);

    switch (int_type) {
        case VM_EXIT_INT_TYPE_EXT:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] External Interrupt Vector %d Detected", cpu_id, vector);
            break;
            
        case VM_EXIT_INT_TYPE_NMI:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] NMI Interrupt Detected", cpu_id);
            break;
            
        case VM_EXIT_INT_TYPE_HW:
            switch (vector) {
                case EXCEPTION_VECTOR_DEBUG:
                    zv_vm_exit_callback_interrupt_debug(cpu_id, exit_qual, guest_context);
                    break;
                    
                case EXCEPTION_VECTOR_PAGE_FAULT:
                    zv_log_write(LOG_NONE, "VMExit", "VM [%d] Page Fault Exception Detected", cpu_id);
                    break;
                    
                case EXCEPTION_VECTOR_GENERAL_PROTECTION:
                    zv_log_write(LOG_NONE, "VMExit", "VM [%d] General Protection Fault Detected", cpu_id);
                    break;
                    
                default:
                    zv_log_write(LOG_NONE, "VMExit", "VM [%d] Hardware Exception Vector %d Detected", cpu_id, vector);
                    break;
            }
            break;
            
        case VM_EXIT_INT_TYPE_PRIV_SW:
            switch (vector) {
                case EXCEPTION_VECTOR_DEBUG:
                    zv_log_write(LOG_NONE, "VMExit", "VM [%d] Debug Exception from INT1 Instruction Detected (privileged software)", cpu_id);
                    break;
                    
                default:
                    zv_log_write(LOG_NONE, "VMExit", "VM [%d] Privileged Software Exception Vector %d Detected", cpu_id, vector);
                    break;
            }
            break;
            
        case VM_EXIT_INT_TYPE_SW: 
            switch (vector) {
                case EXCEPTION_VECTOR_BREAKPOINT:
                    zv_log_write(LOG_NONE, "VMExit", "VM [%d] INT3 Breakpoint Exception Detected at RIP: %016lX", 
                        cpu_id, guest_rip);
                    zv_vm_exit_callback_interrupt_breakpoint(cpu_id, guest_context);
                    break;
                    
                case EXCEPTION_VECTOR_OVERFLOW:
                    zv_log_write(LOG_NONE, "VMExit", "VM [%d] Overflow Exception from INTO Instruction Detected", cpu_id);
                    break;
                    
                default:
                    zv_log_write(LOG_NONE, "VMExit", "VM [%d] Software Exception Vector %d Detected", cpu_id, vector);
                    break;
            }
            break;
            
        default:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] Unknown Interrupt Type %d, Vector %d Detected", 
                cpu_id, int_type, vector);
            break;
    }

    if(zv_is_system_shutdowning() == 0) {
        // Blank timer operation for normal operation
    }
}

/* Remove INT1 exception from the guest */
static void zv_remove_int1_exception_from_vm(void) {
    u64 info_field;

    zv_read_vmcs(VM_CTRL_VM_ENTRY_INT_INFO_FIELD, &info_field);
	info_field &= ~((u64) 0x01 << 1);
	zv_write_vmcs(VM_CTRL_VM_ENTRY_INT_INFO_FIELD, info_field);
}

/* Process INT3 breakpoint exception */
static void zv_vm_exit_callback_interrupt_breakpoint(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context
) {
    u64 guest_rip;
    u64 inst_length;
    
    zv_read_vmcs(VM_GUEST_RIP, &guest_rip);
    zv_read_vmcs(VM_DATA_VM_EXIT_INST_LENGTH, &inst_length);
    
    zv_log_write(LOG_NONE, "VMExit", "VM [%d] INT3 Breakpoint at RIP: %016lX, instruction length: %d", 
        cpu_id, guest_rip, (u32)inst_length);
    
    // Log register state for debugging
    zv_log_write(LOG_DETAIL, "VMExit", "VM [%d] RAX: %016lX, RBX: %016lX, RCX: %016lX, RDX: %016lX", 
        cpu_id, guest_context->rax, guest_context->rbx, guest_context->rcx, guest_context->rdx);
    zv_log_write(LOG_DETAIL, "VMExit", "VM [%d] RSI: %016lX, RDI: %016lX, RBP: %016lX", 
        cpu_id, guest_context->rsi, guest_context->rdi, guest_context->rbp);
    zv_log_write(LOG_DETAIL, "VMExit", "VM [%d] R8: %016lX, R9: %016lX, R10: %016lX, R11: %016lX", 
        cpu_id, guest_context->r8, guest_context->r9, guest_context->r10, guest_context->r11);
    zv_log_write(LOG_DETAIL, "VMExit", "VM [%d] R12: %016lX, R13: %016lX, R14: %016lX, R15: %016lX", 
        cpu_id, guest_context->r12, guest_context->r13, guest_context->r14, guest_context->r15);
    
    // Here you can add your breakpoint handling logic:
    // - Analyze the instruction that caused the breakpoint
    // - Implement step-by-step debugging
    // - Set/remove dynamic breakpoints
    // - Communicate with a debugger
    
    // For now, just advance RIP to skip the INT3 instruction
    zv_advance_vm_guest_rip();
    
    zv_log_write(LOG_NONE, "VMExit", "VM [%d] INT3 breakpoint handled, continuing execution", cpu_id);
}

/* Process hardware debug exception */
static void zv_vm_exit_callback_interrupt_debug(
    int cpu_id,
    u64 exit_qual,
    struct zv_vm_exit_guest_register* guest_context
) {
    unsigned long dr7;
    unsigned long dr6;
    
    // For debug exceptions via VM Exit, exit_qual contains the DR6 information
    dr6 = (unsigned long)exit_qual;

    // Clear DR6 debug status register (clear specific debug event flags)
    dr6 &= 0xfffffffffffffff0;
    set_debugreg(dr6, 6);

    /* When the guest is resumed, let the guest skip hardware breakpoint. */
    zv_read_vmcs(VM_GUEST_RFLAGS, (u64*)&dr7);
    dr7 |= RFLAGS_BIT_RF;
    zv_write_vmcs(VM_GUEST_RFLAGS, dr7);

    zv_remove_int1_exception_from_vm();
}

/* Process INIT IPI */
static void zv_vm_exit_callback_init_signal(int cpu_id) {
    u64 status;

    zv_read_vmcs(VM_GUEST_ACTIVITY_STATE, &status);
    zv_log_write(LOG_NONE, "VMExit", "VM [%d] Activity Status %016lX", cpu_id, status);
}

/* Process Starup IPI */
static void zv_vm_exit_callback_starup_signal(int cpu_id) {
    u64 status;

    zv_read_vmcs(VM_GUEST_ACTIVITY_STATE, &status);
    zv_log_write(LOG_NONE, "VMExit", "VM [%d] Change Activity Status to Active %016lX",
        cpu_id, status);
}

/* Process CPUID */
static void zv_vm_exit_callback_cpuid(struct zv_vm_exit_guest_register* guest_context) {
    cpuid_count(
        guest_context->rax,
        guest_context->rcx,
        (u32*)&guest_context->rax,
        (u32*)&guest_context->rbx,
		(u32*)&guest_context->rcx,
        (u32*)&guest_context->rdx
    );

    zv_log_write(LOG_DETAIL, "VMExit", "VM_EXIT_REASON_CPUID Result: %08X, %08X, %08X, %08X",
        (u32)guest_context->rax, (u32) guest_context->rbx, (u32)guest_context->rcx, (u32)guest_context->rdx);

    zv_advance_vm_guest_rip();
}

/* Process INVD */
static void zv_vm_exit_callback_invd(void) {
    zv_invd();
    zv_advance_vm_guest_rip();
}

/* Process RW event for control register */
static void zv_vm_exit_callback_access_cr(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context,
    u64 exit_reason,
    u64 exit_qual
) {
    u64 reg_value = 0;
	u64 prev_cr3 = 0;

    if (VM_EXIT_QUAL_CTRL_REG_ACC_GET_ACC_TYPE(exit_qual) == 
        VM_EXIT_QUAL_CTRL_REG_ACC_MOVE_FROM_CR) {
        switch(VM_EXIT_QUAL_CTRL_REG_ACC_GET_CTRL_REG_NUMBER(exit_qual)) {
            case REG_NUM_CR0:
                reg_value = zv_get_cr0();
                break;
            
            case REG_NUM_CR2:
				reg_value = zv_get_cr2();
				break;

			case REG_NUM_CR3:
				reg_value = zv_get_cr3();
				break;

			case REG_NUM_CR4:
				reg_value = zv_get_cr4();
				break;

			case REG_NUM_CR8:
				reg_value = zv_get_cr8();
				break;
        }

        zv_set_reg_value_from_index(
            guest_context,
            VM_EXIT_QUAL_CTRL_REG_ACC_GET_GP_REG_NUMBER(exit_qual),
            reg_value  
        );
    } else if (VM_EXIT_QUAL_CTRL_REG_ACC_GET_ACC_TYPE(exit_qual) == 
                VM_EXIT_QUAL_CTRL_REG_ACC_MOVE_TO_CR) {
        reg_value = zv_get_reg_value_from_index(
            guest_context,
            VM_EXIT_QUAL_CTRL_REG_ACC_GET_GP_REG_NUMBER(exit_qual)
        );

        switch (VM_EXIT_QUAL_CTRL_REG_ACC_GET_CTRL_REG_NUMBER(exit_qual)) {
            case REG_NUM_CR0:
				zv_write_vmcs(VM_GUEST_CR0, reg_value);
				break;

			case REG_NUM_CR2:
				/* zv_write_vmcs(VM_GUEST_CR2, reg_value); */
				break;

			case REG_NUM_CR3:
				zv_read_vmcs(VM_GUEST_CR3, &prev_cr3);
				zv_write_vmcs(VM_GUEST_CR3, reg_value);
				break;

			case REG_NUM_CR4:
				/* VMXE bit should be set! for unrestricted guest. */
				reg_value |= ((u64) CR4_BIT_VMXE);
				zv_write_vmcs(VM_GUEST_CR4, reg_value);
				break;

			case REG_NUM_CR8:
				/* zv_write_vmcs(VM_GUEST_CR8, reg_value); */
				break;
        }
    } else {
        zv_log_write(LOG_NONE, "VMExit", "VM [%d] VM_EXIT_QUAL_CTRL_REG is "
			"not move from reg_value: %d", 
            cpu_id, (int)VM_EXIT_QUAL_CTRL_REG_ACC_GET_ACC_TYPE(exit_qual));
    }

    zv_advance_vm_guest_rip();
}

/* Get register value of index in guest context */
static u64 zv_get_reg_value_from_index(
    struct zv_vm_exit_guest_register* guest_context,
    int index
) {
    u64 reg_value = 0;

    switch (index) {
        case REG_NUM_RAX:
			reg_value = guest_context->rax;
			break;

		case REG_NUM_RCX:
			reg_value = guest_context->rcx;
			break;

		case REG_NUM_RDX:
			reg_value = guest_context->rdx;
			break;

		case REG_NUM_RBX:
			reg_value = guest_context->rbx;
			break;

		case REG_NUM_RSP:
			/* reg_value = guest_context->rsp; */
			break;

		case REG_NUM_RBP:
			reg_value = guest_context->rbp;
			break;

		case REG_NUM_RSI:
			reg_value = guest_context->rsi;
			break;

		case REG_NUM_RDI:
			reg_value = guest_context->rdi;
			break;

		case REG_NUM_R8:
			reg_value = guest_context->r8;
			break;

		case REG_NUM_R9:
			reg_value = guest_context->r9;
			break;

		case REG_NUM_R10:
			reg_value = guest_context->r10;
			break;

		case REG_NUM_R11:
			reg_value = guest_context->r11;
			break;

		case REG_NUM_R12:
			reg_value = guest_context->r12;
			break;

		case REG_NUM_R13:
			reg_value = guest_context->r13;
			break;

		case REG_NUM_R14:
			reg_value = guest_context->r14;
			break;

		case REG_NUM_R15:
			reg_value = guest_context->r15;
			break;
    }

    return reg_value;
}

/* Set value to register of index in guest context */
static void zv_set_reg_value_from_index(
    struct zv_vm_exit_guest_register* guest_context,
    int index,
    u64 reg_value
) {
    switch (index) {
        case REG_NUM_RAX:
			guest_context->rax = reg_value;
			break;

		case REG_NUM_RCX:
			guest_context->rcx = reg_value;
			break;

		case REG_NUM_RDX:
			guest_context->rdx = reg_value;
			break;

		case REG_NUM_RBX:
			guest_context->rbx = reg_value;
			break;

		case REG_NUM_RSP:
			/* guest_context->rsp = reg_value; */
			break;

		case REG_NUM_RBP:
			guest_context->rbp = reg_value;
			break;

		case REG_NUM_RSI:
			guest_context->rsi = reg_value;
			break;

		case REG_NUM_RDI:
			guest_context->rdi = reg_value;
			break;

		case REG_NUM_R8:
			guest_context->r8 = reg_value;
			break;

		case REG_NUM_R9:
			guest_context->r9 = reg_value;
			break;

		case REG_NUM_R10:
			guest_context->r10 = reg_value;
			break;

		case REG_NUM_R11:
			guest_context->r11 = reg_value;
			break;

		case REG_NUM_R12:
			guest_context->r12 = reg_value;
			break;

		case REG_NUM_R13:
			guest_context->r13 = reg_value;
			break;

		case REG_NUM_R14:
			guest_context->r14 = reg_value;
			break;

		case REG_NUM_R15:
			guest_context->r15 = reg_value;
			break;
    }
}

/* Process write MSR */
static void zv_vm_exit_callback_wrmsr(int cpu_id) {
    zv_log_write(LOG_NONE, "VMExit", "VM [%d] zv_vm_exit_callback_wrmsr", cpu_id);
    zv_insert_exception_to_vm();
}

/* Insert exception to the guest */
void zv_insert_exception_to_vm(void) {
    u64 info_field;
    zv_read_vmcs(VM_CTRL_VM_ENTRY_INT_INFO_FIELD, &info_field);

	info_field |= VM_BIT_VM_ENTRY_INT_INFO_GP;
	//info_field |= VM_BIT_VM_ENTRY_INT_INFO_UD;

	zv_write_vmcs(VM_CTRL_VM_ENTRY_INT_INFO_FIELD, info_field);
	zv_write_vmcs(VM_CTRL_VM_ENTRY_EXCEPT_ERR_CODE, 0);
}

/* Process GDTR, IDTR modification */
static void zv_vm_exit_callback_access_gdtr_idtr(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context
) {
    if (zv_is_system_shutdowning() == 0) {
        zv_log_write(LOG_NONE, "VMExit", "VM [%d] zv_vm_exit_callback_access_gdtr_idtr", cpu_id);
        zv_advance_vm_guest_rip();
    } else {
        zv_disable_desc_monitor();
    }
}

/* Disable descriptor (GDT LDT IDT) monitoring function */
static void zv_disable_desc_monitor(void) {
    u64 reg_value;

	zv_read_vmcs(VM_CTRL_SEC_PROC_BASED_EXE_CTRL, &reg_value);
	reg_value &= ~((u64)(VM_BIT_VM_SEC_PROC_CTRL_DESC_TABLE));
	reg_value &= 0xFFFFFFFF;
	zv_write_vmcs(VM_CTRL_SEC_PROC_BASED_EXE_CTRL, reg_value);
}

/*
 * Process LDTR, TR modification callback.
 * Linux set 0 to LLDT, so this function allows only 0 value setting.
 */
static void zv_vm_exit_callback_access_ldtr_tr(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context
) {
    u64 inst_info;
	u64 dest_addr = 0;
	u64 value;
	int memory = 0;

	zv_read_vmcs(VM_DATA_VM_EXIT_INST_INFO, &inst_info);

    /* Check destination type. */
	if (! VM_INST_INFO_MEM_REG(inst_info)) {
        dest_addr = zv_calc_dest_mem_addr(guest_context, inst_info);
        memory = 1;
	} else {
        dest_addr = zv_get_reg_value_from_index(
            guest_context,
            VM_INST_INFO_REG1(inst_info)
        );
    }

    switch(VM_INST_INFO_INST_IDENTITY(inst_info)) {
        // SLDT
        case VM_INST_INFO_SLDT:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] SLDT is not allowed", cpu_id);
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] zv_vm_exit_callback_ldtr_tr SLDT", cpu_id);
            value = zv_get_ldtr();
            if (memory) {
                zv_set_value_to_memory(inst_info, dest_addr, value);
            } else {
                zv_set_reg_value_from_index(
                    guest_context,
                    (inst_info >> 3) & 0xF,
                    value
                );
            }
            break;

        // STR
        case VM_INST_INFO_STR:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] STR is not allowed", cpu_id);
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] zv_vm_exit_callback_ldtr_tr STR", cpu_id);
            zv_insert_exception_to_vm();
            break;

        // LLDT
        case VM_INST_INFO_LLDT:
            if (memory) {
                zv_log_write(LOG_NONE, "VMExit", "VM [%d] Memory, value[%16lX]", cpu_id, dest_addr);
                zv_log_write(LOG_NONE, "VMExit", "VM [%d] zv_vm_exit_callback_ldtr_tr LLDT 1", cpu_id);
                zv_insert_exception_to_vm();

                value = zv_get_value_from_memory(inst_info, dest_addr);
                zv_write_vmcs(VM_GUEST_LDTR_SELECTOR, value);
            } else {
                if (dest_addr == 0) {
                    zv_write_vmcs(VM_GUEST_LDTR_SELECTOR, dest_addr);
                } else {
                    zv_log_write(LOG_NONE, "VMExit", "VM [%d] LLDT value is not 0, %016lX", cpu_id, dest_addr);
                    zv_log_write(LOG_NONE, "VMExit", "VM [%d] zv_vm_exit_callback_ldtr_tr LLDT 2", cpu_id);
                    zv_insert_exception_to_vm();
                }
            }
            break;

        // LTR
        case VM_INST_INFO_LTR:
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] LTR is not allowed", cpu_id);
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] zv_vm_exit_callback_ldtr_tr LTR", cpu_id);
            zv_insert_exception_to_vm();
            break;
    }

    zv_advance_vm_guest_rip();
}

/* Calculate destination memory address from instuction information in guest context */
static u64 zv_calc_dest_mem_addr(
    struct zv_vm_exit_guest_register* guest_context,
    u64 inst_info
) {
    u64 dest_addr = 0;

    if (! (inst_info & VM_INST_INFO_IDX_REG_INVALID)) {
		dest_addr += zv_get_reg_value_from_index(
            guest_context,
			VM_INST_INFO_IDX_REG(inst_info)
        );
		dest_addr = dest_addr << VM_INST_INFO_SCALE(inst_info);
	}

	if (! (inst_info & VM_INST_INFO_BASE_REG_INVALID)) {
		dest_addr += zv_get_reg_value_from_index(
            guest_context,
			VM_INST_INFO_BASE_REG(inst_info)
        );
	}

	return dest_addr;
}

/* Get value from memory */
static u64 zv_get_value_from_memory(u64 inst_info, u64 addr) {
    u64 value = 0;

    switch(VM_INST_INFO_ADDR_SIZE(inst_info))
	{
		case VM_INST_INFO_ADDR_SIZE_16BIT:
			value = *(u16*)addr;
			break;

		case VM_INST_INFO_ADDR_SIZE_32BIT:
			value = *(u32*)addr;
			break;

		case VM_INST_INFO_ADDR_SIZE_64BIT:
			value = *(u64*)addr;
			break;
	}

	return value;
}

/* Set value to memory */
static void zv_set_value_to_memory(u64 inst_info, u64 addr, u64 value) {
    switch (VM_INST_INFO_ADDR_SIZE(inst_info))
	{
		case VM_INST_INFO_ADDR_SIZE_16BIT:
			*(u16*)addr = (u16)value;
			break;

		case VM_INST_INFO_ADDR_SIZE_32BIT:
			*(u32*)addr = (u64)value;
			break;

		case VM_INST_INFO_ADDR_SIZE_64BIT:
			*(u64*)addr = (u64)value;
			break;
	}
}

/* Process EPT violation */
static void zv_vm_exit_callback_ept_violation(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context,
    u64 exit_reason,
    u64 exit_qual,
    u64 guest_linear,
    u64 guest_physical
) {
    u64 log_addr;

    zv_log_write(LOG_NONE, "VMExit", "VM [%d] EPT Violation is detected",
        cpu_id);
    zv_log_write(LOG_NONE, "VMExit", "VM [%d] Guest Linear: %ld, %016lX", 
        cpu_id, guest_linear, guest_linear);
    zv_log_write(LOG_NONE, "VMExit", "VM [%d] Guest Physical: %ld, %016lX",
        cpu_id, guest_physical, guest_physical);
    zv_log_write(LOG_NONE, "VMExit", "VM [%d] virt_to_phys: virt %016lX phys %016lX",
        cpu_id, guest_linear, virt_to_phys((void*)guest_linear));

    if (zv_is_system_shutdowning() == 0) {
        /* Insert exception to guest */
        zv_insert_exception_to_vm();
        zv_log_error(ERROR_ACCESS_NOT_ALLOWED_MEM);
    } else {
        log_addr = (u64)phys_to_virt(guest_physical);
        zv_set_ept_all_access_page(guest_physical);
    }
}

/* Process VT-timer expire */
static void zv_vm_exit_callback_pre_timer_expired(int cpu_id) {
    u64 value;

    if (zv_is_system_shutdowning() == 0) {
        /* Check gdtr */
        if (zv_check_gdtr(cpu_id)) {
            zv_log_write(LOG_NONE, "VMExit", "VM [%d] GDTR or IDTR Check Error", cpu_id);
            zv_log_error(ERROR_ACCESS_NOT_ALLOWED_MEM);
        }
    }

    /* Reset VM timer */
    value = zv_calc_vm_pre_timer_value();
    zv_write_vmcs(VM_GUEST_VMX_PRE_TIMER_VALUE, value);
}

/* Check malicious descriptors in GDT */
static int zv_check_gdtr(int cpu_id) {
    struct desc_ptr gdtr;
	struct desc_struct* gdt;
	int result = 0;
	int i;
	u64 address;
	u64 size;

	zv_read_vmcs(VM_GUEST_GDTR_BASE, &address);
	zv_read_vmcs(VM_GUEST_GDTR_LIMIT, &size);
	gdtr.address = address;
	gdtr.size = (u16)size;

    if (zv_is_system_shutdowning()) return 0;

    if (gdtr.address != g_gdtr_array[cpu_id].address) {
        zv_log_write(LOG_NORMAL, "VMExit", "VM [%d] Structure not same, Org "
			"Addr %016lX, Size %d, New Addr %016lX, Size %d", 
            cpu_id, g_gdtr_array[cpu_id].address, g_gdtr_array[cpu_id].size,
            gdtr.address, gdtr.size);

        return 1;
    }

    if (gdtr.size >= 0x1000) {
        zv_log_write(LOG_NORMAL, "VMExit", "VM [%d] GDT size is Over, Org "
			"Addr %016lX, Size %d, New Addr %016lX, Size %d", 
            cpu_id, g_gdtr_array[cpu_id].address, g_gdtr_array[cpu_id].size,
            gdtr.address, gdtr.size);
    }

    for (i = 0; i < gdtr.size; i += 8) {
        gdt = (struct desc_struct*)(gdtr.address + i);
        /* Is the descriptor system? Can user level access the descriptor? */
        if((gdt->s == 0) && (gdt->p == 1) && (gdt->dpl == 3))
		{
			if ((gdt->type == GDT_TYPE_64BIT_CALL_GATE) ||
				(gdt->type == GDT_TYPE_64BIT_INTERRUPT_GATE) ||
				(gdt->type == GDT_TYPE_64BIT_TRAP_GATE))
			{
                zv_log_write(LOG_NORMAL, "VMExit", 
                    "VM [%d] GDT index %d - "
                    "base: %02X%04X%04X, limit: %X%04X, "
                    "type: %d, dpl: %d, p: %d",
                    cpu_id, i,
                    gdt->base2, gdt->base1, gdt->base0,  
                    gdt->limit1, gdt->limit0,            
                    gdt->type, gdt->dpl, gdt->p); 

				result = 1;
				break;
			}
			else if ((gdt->type == GDT_TYPE_64BIT_LDT) ||
					 (gdt->type == GDT_TYPE_64BIT_TSS) ||
					 (gdt->type == GDT_TYPE_64BIT_TSS_BUSY))
			{
				/* For 16byte Descriptor. */
				i += 8;
			}
		}
    }

    return result;
}

/* Process VM resume fail */
void zv_vm_resume_fail_callback(u64 error) {
    u64 value;
	u64 value2;
	u64 value3;

	zv_read_vmcs(VM_GUEST_EFER, &value);
	zv_read_vmcs(VM_CTRL_VM_ENTRY_CTRLS, &value2);
	zv_read_vmcs(VM_GUEST_CR0, &value3);

    if (value & EFER_BIT_LME) {
        zv_log_write(LOG_NONE, "VMExit", "VM is in 64-bit mode, %016lX, %016lX, %016lX",
            value, value2, value3);
    } else {
        zv_log_write(LOG_NONE, "VMExit", "VM is not in 64-bit mode, %016lX, %016lX, %016lX",
            value, value2, value3);
    }

    zv_log_write(LOG_NONE, "VMExit", "VM RESUME FAIL %d !", error);
    zv_log_error(ERROR_LAUNCH_FAIL);
}

/* Process VM call */
static void zv_vm_exit_callback_vmcall(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context
) {
    u64 service_id;
    void* arg;

    service_id = guest_context->rax;
    arg = (void*)guest_context->rbx;

    zv_log_write(LOG_DEBUG, "VMExit", "VM [%d] VMCALL index[%ld]", cpu_id, service_id);
    /* Move RIP to next instruction */
    zv_advance_vm_guest_rip();

    switch (service_id) {
#ifdef ZEROVISOR_USE_SHUTDOWN
        case VM_SERVICE_SHUTDOWN:
            atomic_set(&(g_share_context->shutdown_flag), 1);
            break;
        case VM_SERVICE_SHUTDOWN_THIS_CORE:
            zv_shutdown_vm_this_core(cpu_id, guest_context);
            break;
#endif

        default:
            zv_advance_vm_guest_rip();
            break;
    }
}

/* Shutdown zeroVisor */
static void zv_shutdown_vm_this_core(
    int cpu_id,
    struct zv_vm_exit_guest_register* guest_context
) {
    struct zv_vm_full_context full_context;
	u64 guest_VMCS_log_addr;
	u64 guest_VMCS_phy_addr;
	u64 guest_rsp;

    // zv_log_write(LOG_DEBUG, "Core", "VM [%d] zv_shutdown_vm_this_core is called", cpu_id);

    zv_read_vmcs(VM_GUEST_RSP, &guest_rsp);
    zv_fill_context_from_vm_guest(guest_context, &full_context);

    guest_VMCS_log_addr = (u64)(g_guest_vmcs_log_addr[cpu_id]);
	guest_VMCS_phy_addr = (u64)virt_to_phys((void*)guest_VMCS_log_addr);

    zv_clear_vmcs(&guest_VMCS_phy_addr);
    zv_stop_vmx();

    /* Restore original GDTR/IDTR using kernel APIs */
    zv_log_write(LOG_DEBUG, "Core", "VM [%d] Restoring original GDTR/IDTR", cpu_id);
    zv_log_write(LOG_DEBUG, "Core", "VM [%d] Original GDTR: %016lX, Size: %d", 
        cpu_id, g_gdtr_array[cpu_id].address, g_gdtr_array[cpu_id].size);
    zv_log_write(LOG_DEBUG, "Core", "VM [%d] Original IDTR: %016lX, Size: %d", 
        cpu_id, g_idtr_array[cpu_id].address, g_idtr_array[cpu_id].size);
    
    load_gdt(&g_gdtr_array[cpu_id]);
    load_idt(&g_idtr_array[cpu_id]);

    zv_restore_context_from_vm_guest(cpu_id, &full_context, guest_rsp);
}

/* Fill guest context from the guest VMCS */
static void zv_fill_context_from_vm_guest(
    struct zv_vm_exit_guest_register* guest_context,
    struct zv_vm_full_context* full_context
) {
	memcpy(&(full_context->gp_register), guest_context, sizeof(struct zv_vm_exit_guest_register));

	zv_read_vmcs(VM_GUEST_CS_SELECTOR, &(full_context->cs_selector));
	zv_read_vmcs(VM_GUEST_DS_SELECTOR, &(full_context->ds_selector));
	zv_read_vmcs(VM_GUEST_ES_SELECTOR, &(full_context->es_selector));
	zv_read_vmcs(VM_GUEST_FS_SELECTOR, &(full_context->fs_selector));
	zv_read_vmcs(VM_GUEST_GS_SELECTOR, &(full_context->gs_selector));

	zv_read_vmcs(VM_GUEST_LDTR_SELECTOR, &(full_context->ldtr_selector));
	zv_read_vmcs(VM_GUEST_TR_SELECTOR, &(full_context->tr_selector));

	zv_read_vmcs(VM_GUEST_CR0, &(full_context->cr0));
	zv_read_vmcs(VM_GUEST_CR3, &(full_context->cr3));
	zv_read_vmcs(VM_GUEST_CR4, &(full_context->cr4));
	zv_read_vmcs(VM_GUEST_RIP, &(full_context->rip));
	zv_read_vmcs(VM_GUEST_RFLAGS, &(full_context->rflags));
}

/* Restore the guest VMCS from the full context */
static void zv_restore_context_from_vm_guest(
    int cpu_id,
    struct zv_vm_full_context* full_context,
    u64 guest_rsp
) {
    u64 target_addr;

    /* Copy context to stack and restore */
	target_addr = guest_rsp - sizeof(struct zv_vm_full_context);
	memcpy((void*)target_addr, full_context, sizeof(struct zv_vm_full_context));

	zv_restore_context_from_stack(target_addr);
}