.section .text
.code64

# Exported functions
.globl zv_enable_vmx
.globl zv_disable_vmx
.globl zv_start_vmx
.globl zv_clear_vmcs
.globl zv_load_vmcs
.globl zv_read_vmcs
.globl zv_write_vmcs
.globl zv_stop_vmx
.globl zv_get_cr0
.globl zv_set_cr0
.globl zv_get_cr2
.globl zv_get_cr3
.globl zv_set_cr3
.globl zv_get_cr4
.globl zv_set_cr4
.globl zv_get_cr8
.globl zv_get_cs
.globl zv_get_ss
.globl zv_get_ds
.globl zv_get_es
.globl zv_get_fs
.globl zv_get_gs
.globl zv_get_tr
.globl zv_get_dr7
.globl zv_get_rflags
.globl zv_get_ldtr
.globl zv_rdmsr
.globl zv_wrmsr
.globl zv_vm_launch
.globl zv_vm_resume
.globl zv_vm_exit_callback_stub
.globl zv_invd
.globl zv_flush_gdtr
.globl zv_gen_int
.globl zv_pause_loop
.globl zv_vm_call
.globl zv_restore_context_from_stack

# Imported functions
.extern zv_vm_exit_callback
.extern zv_vm_resume_fail_callback

# Macros (using GAS .macro directive)
.macro PUSHAQ
    pushq %rbp
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rdi
    pushq %rsi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

.macro POPAQ
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rsi
    popq %rdi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    popq %rbp
.endm

# Enable VMX.
zv_enable_vmx:
    movq %cr4, %rax
    btsq $13, %rax
    movq %rax, %cr4
    ret

# Disable VMX.
zv_disable_vmx:
    movq %cr4, %rax
    btcq $13, %rax
    movq %rax, %cr4
    ret

# Start VMX.
zv_start_vmx:
    # call disable_A20
    vmxon (%rdi)
    jc .Lvmx_error
    jz .Lvmx_error

    movq $1, %rax
    jmp .Lvmx_end

.Lvmx_error:
    movq $0, %rax
.Lvmx_end:
    # call enable_A20
    ret

# Clear VMCS.
zv_clear_vmcs:
    vmclear (%rdi)
    jc .Lvmclr_error
    jz .Lvmclr_error

    movq $1, %rax
    jmp .Lvmclr_end

.Lvmclr_error:
    movq $0, %rax
.Lvmclr_end:
    ret

# Load VMCS.
zv_load_vmcs:
    vmptrld (%rdi)
    jc .Lvmld_error
    jz .Lvmld_error

    movq $1, %rax
    jmp .Lvmld_end

.Lvmld_error:
    movq $0, %rax
.Lvmld_end:
    ret

# Write data to VMCS.
zv_write_vmcs:
    vmwrite %rsi, %rdi
    jc .Lvmwrt_error
    jz .Lvmwrt_error

    movq $1, %rax
    jmp .Lvmwrt_end

.Lvmwrt_error:
    movq $0, %rax
.Lvmwrt_end:
    ret

# Read data from VMCS.
zv_read_vmcs:
    vmread %rdi, (%rsi)
    jc .Lvmrd_error
    jz .Lvmrd_error

    movq $1, %rax
    jmp .Lvmrd_end

.Lvmrd_error:
    movq $0, %rax
.Lvmrd_end:
    ret

# Stop VMX.
zv_stop_vmx:
    vmxoff
    ret

# Get CR0.
zv_get_cr0:
    movq %cr0, %rax
    ret

# Set CR0.
zv_set_cr0:
    movq %rdi, %cr0
    ret

# Get CR2.
zv_get_cr2:
    movq %cr2, %rax
    ret

# Get CR3.
zv_get_cr3:
    movq %cr3, %rax
    ret

# Set CR3.
zv_set_cr3:
    movq %rdi, %cr3
    ret

# Get CR4.
zv_get_cr4:
    movq %cr4, %rax
    ret

# Set CR4.
zv_set_cr4:
    movq %rdi, %cr4
    ret

# Get CR8.
zv_get_cr8:
    movq %cr8, %rax
    ret

# Get CS.
zv_get_cs:
    movw %cs, %ax
    movzwq %ax, %rax
    ret

# Get SS.
zv_get_ss:
    movw %ss, %ax
    movzwq %ax, %rax
    ret

# Get DS.
zv_get_ds:
    movw %ds, %ax
    movzwq %ax, %rax
    ret

# Get ES.
zv_get_es:
    movw %es, %ax
    movzwq %ax, %rax
    ret

# Get FS.
zv_get_fs:
    movw %fs, %ax
    movzwq %ax, %rax
    ret

# Get GS.
zv_get_gs:
    movw %gs, %ax
    movzwq %ax, %rax
    ret

# Get TR.
zv_get_tr:
    str %ax
    movzwq %ax, %rax
    ret

# Get DR7.
zv_get_dr7:
    movq %dr7, %rax
    ret

# Get RFLAGS.
zv_get_rflags:
    pushfq
    popq %rax
    ret

# Get LDTR.
zv_get_ldtr:
    sldt %ax
    movzwq %ax, %rax
    ret

# Read MSR.
zv_rdmsr:
    pushq %rdx
    pushq %rcx

    xorl %edx, %edx
    xorl %eax, %eax

    movl %edi, %ecx
    rdmsr

    shlq $32, %rdx
    orq %rdx, %rax

    popq %rcx
    popq %rdx
    ret

# Write MSR.
zv_wrmsr:
    pushq %rdx
    pushq %rcx

    movq %rsi, %rdx
    shrq $32, %rdx
    movl %esi, %eax

    movl %edi, %ecx
    wrmsr

    popq %rcx
    popq %rdx
    ret

# Launch VM.
zv_vm_launch:
    pushq %rbx

    # For seamless interoperation, set RSP of the guest to the host.
    movl $0x681C, %ebx       # RSP
    movq %rsp, %rax
    vmwrite %rax, %rbx

    # Get current RIP.
    call .Lget_rip
.Lget_rip:
    popq %rax

    movl $0x681E, %ebx       # RIP
    leaq .Lvm_success(%rip), %rax
    vmwrite %rax, %rbx

    vmlaunch

	# Process fail.
    popq %rbx

    jc .Lvm_error_invalid
    jz .Lvm_error_valid

    movl $1, %eax
    jmp .Lvm_end

.Lvm_error_invalid:
    movl $-1, %eax
    jmp .Lvm_end

.Lvm_error_valid:
    xorl %eax, %eax

.Lvm_end:
    ret

.Lvm_success:
    # Start line of the guest.
    # Now the core is in the guest.
    popq %rbx
    movl $1, %eax
    ret

# Stub of VM exit callback.
zv_vm_exit_callback_stub:
    # Start line of the host.
    # Now the core is in the host.
    PUSHAQ

    # RDI has the pointer of the guest context structure.
    movq %rsp, %rdi

    call zv_vm_exit_callback

    # Resume the guest.
    POPAQ

    vmresume

    # Error occur.
    movq %rax, %rdi
    call zv_vm_resume_fail_callback

.Lhang:
    jmp .Lhang
    ret

# Resume VM.
zv_vm_resume:
    vmresume

    jc .Lresume_error_invalid
    jz .Lresume_error_valid

    movl $1, %eax
    jmp .Lresume_end

.Lresume_error_invalid:
    movl $-1, %eax
    jmp .Lresume_end

.Lresume_error_valid:
    xorl %eax, %eax

.Lresume_end:
    ret

.Lresume_success:
	# Start line of the guest.
	# Now the core is in the guest.
    movl $1, %eax
    ret

# Get current RIP.
zv_get_rip:
    popq %rax
    pushq %rax
    ret

# Process INVD.
zv_invd:
    invd
    ret

# Flush GDTR.
zv_flush_gdtr:
    pushq %rax

    movw %ss, %ax
    movw %ax, %ss
    
    popq %rax
    ret

# Generate interrupt 0xF8.
zv_gen_int:
    pushq %rax
    movq %rdi, %rax
    sti
    int $0xf8
    popq %rax
    ret

# Pause CPU.
zv_pause_loop:
    pause
    ret

# Call vmcall.
zv_vm_call:
    pushq %rax
    pushq %rbx

    movq %rdi, %rax
    movq %rsi, %rbx
    
    vmcall
    
    popq %rbx
    popq %rax
    ret

# Restore context from stack (vm_full_context).
zv_restore_context_from_stack:
    movq %rdi, %rsp

    popq %rax         # cr4
    #movq %rax, %cr4

    popq %rax         # cr3
    movq %rax, %cr3

    popq %rax         # cr0
    #movq %rax, %cr0

    popq %rax         # tr
    #ltr %ax

    popq %rax         # lldt
    #lldt %ax

    popq %rax         # gs
    #movw %ax, %gs

    popq %rax         # fs
    movw %ax, %fs

    popq %rax         # es
    movw %ax, %es

    popq %rax         # ds
    movw %ax, %ds

    popq %rax         # cs
    # ignore cs

    POPAQ             # Restore GP registers
    popfq             # Restore RFLAGS

    ret               # Return to RIP

