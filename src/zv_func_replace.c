#include <linux/sched.h>
#include <linux/sched/mm.h>  // for get_task_mm
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <asm/tlbflush.h>

#include "../include/zv_core.h"
#include "../include/zv_config.h"
#include "../include/zv_func_replace.h"
#include "../include/asm.h"
#include "../include/zv_log.h"
#include "../include/zv_func_search.h"

/* Varivables */
static pid_t pid_parent = 0;
static pid_t pid_clone = 0;
int func_count;
struct zv_hijack_target_func funcs[128];
char inst_temp[PAGE_SIZE * 4]; /*Malloc space to handle inst*/
char proc_buffer[128];
struct mutex m;
int main_cow_triggered;
unsigned long addr_space;


static ssize_t zv_proc_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos);
static int zv_parse_funcs_info(const char* buffer, int* out_count, struct zv_hijack_target_func *out_funcs);
static int zv_lookup_target_func(u64 inst_addr,unsigned char flag);
static void zv_int3_handler_dispatcher(int func_index,unsigned char flagn);
static void zv_handle_first_int3_hit(int func_index, unsigned char flag, bool is_replace);
static void zv_handle_int3_return(int func_index , bool is_jump);
static void zv_trigger_cow_for_executable_pages(void);
static pte_t *zv_get_user_pte(struct mm_struct *mm, unsigned long addr);
void zv_print_addr_pte(const char *name,u64 addr);

static const struct proc_ops zv_func_replace_proc_fops = {
    .proc_write = zv_proc_write,
};

int zv_func_replace_init(void) {
    /* create proc entry for clone */
    struct proc_dir_entry *entry;

    /* Variables init */
    main_cow_triggered = 0;
    func_count = 0;
    pid_parent = 0;
    pid_clone = 0;

    entry = proc_create("zv_func_replace", 0666, NULL, &zv_func_replace_proc_fops);
    if (!entry) {
        zv_log_write(LOG_NONE, "Func_replace", "failed to create zv_func_replace_entry");
        return -ENOMEM;
    }

    mutex_init(&m);
    return 0;
}

void zv_func_replace_exit(void) {
    remove_proc_entry("zv_func_replace", NULL);
    zv_log_write(LOG_NORMAL, "Func_replace", "Function replacement module exit");
}

static ssize_t zv_proc_write(
    struct file *file,
    const char __user *ubuf, 
    size_t count, 
    loff_t *ppos
) {
    int ret;

    /* Basic verify */
    if (count > BUFF_SIZE - 1) {
        return -EINVAL;
    } 
    
    if (copy_from_user(proc_buffer, ubuf, count)) {
        return -EFAULT;
    }
    proc_buffer[count] = '\0';

    ret = zv_parse_funcs_info(proc_buffer, &func_count, funcs);
    if (ret < 0) {
        return ret;
    }

    zv_log_write(LOG_NORMAL, "Func_replace", "Successfully parsed %d functions", func_count);
    return count;
}

static int zv_parse_funcs_info(
    const char* buffer,
    int* out_count,
    struct zv_hijack_target_func *out_funcs
) {
    int pos = 0;
    int read_offset;
    int parsed_count;
    int i;

    /* Parse the num of functions */
    if (sscanf(buffer, "count:%d\n%n", &parsed_count, &read_offset) != 1) {
        zv_log_write(LOG_NONE, "Func_replace", "Failed to parse function count");
        return -EINVAL;
    }

    /* Basic range check */
    if (parsed_count <= 0 || parsed_count > 128) {
        zv_log_write(LOG_NONE, "Func_replace", "Invalid function count: %d", parsed_count);
        return -EINVAL;
    }

    pos += read_offset;

    /* Parse function */
    for (i = 0; i < parsed_count; i ++) {
        if (sscanf(buffer + pos, "%s %02hhx%02hhx %02hhx\n%n",
                out_funcs[i].func_name,
                &(out_funcs[i].saved_bytes[0]),
                &(out_funcs[i].saved_bytes[1]),
                &(out_funcs[i].hijack_flag),
                &read_offset) != 4
        ) {
            zv_log_write(LOG_NONE, "Func_replace", "Failed to parse function %d", i);
            return -EINVAL;
        }

        /* Basic check: name is non-null */
        if (strlen(out_funcs[i].func_name) == 0) {
            zv_log_write(LOG_NONE, "Func_replace", "Empty function name at index %d", i);
            return -EINVAL;
        }

        /* Basic check: flag range check */
        if ((out_funcs[i].hijack_flag & 0x7F) >= 128) {
            zv_log_write(LOG_NONE, "Func_replace", "Invalid flag for function %s: %02hhx", 
                        out_funcs[i].func_name, out_funcs[i].hijack_flag);
            return -EINVAL;
        }

        /* Init address */
        out_funcs[i].origin_addr = 0;

        zv_log_write(LOG_NONE, "Func_replace", "Parsed func: %s bytes:%02hhx%02hhx flag:%02hhx",
            out_funcs[i].func_name, out_funcs[i].saved_bytes[0], out_funcs[i].saved_bytes[1], out_funcs[i].hijack_flag);
        
        pos += read_offset;
    }

    *out_count = parsed_count;
    return 0;
}

/* when zeroVisor catch int3, use this function to handle */
void zv_handle_function_hijack(u64 inst_addr, unsigned long guest_cr3){
    int func_index;
    unsigned char flag;
    int ret;
    unsigned long host_cr3;

    /* save current cr3 and change cr3 */
    mutex_lock(&m);
    host_cr3 = zv_get_cr3();
    zv_set_cr3(guest_cr3);

    ret = copy_from_user(
        &flag,
        (usr_char)(((void *)(inst_addr + 1))),
        1
    );
    if (ret != 0) {
        zv_log_write(LOG_NONE, "Func_replace", "Failed to read flag from user space");
        goto cleanup;
    }
    
    func_index = zv_lookup_target_func(inst_addr, flag);
    if (func_index < 0) {
        zv_log_write(LOG_NONE, "Func_replace", "Function not found for flag %02hhx", flag);
        goto cleanup;
    }
    
    if(strcmp(funcs[func_index].func_name,"main") == 0){ 
        /* if we catch function main, we insert syscall fork */
        if(! main_cow_triggered) {
            /* to trigger cow, we set all executable vma writable */
            zv_trigger_cow_for_executable_pages();
            main_cow_triggered = 1;
        }
        /* args len should include len of flag */
        zv_int3_handler_dispatcher(func_index, flag);
    }else{
        zv_int3_handler_dispatcher(func_index, flag);
    }

cleanup:
    /*recover cr3*/
    zv_set_cr3(host_cr3);
    mutex_unlock(&m);
}

/**
 * zv_lookup_target_func - Look up hijack target function and update address
 * @inst_addr: Address of the INT3 instruction
 * @flag: Flag byte containing function identifier and return status
 *        bit 7: INT3_RETURN flag (1=returning from int3, 0=first int3 hit)
 *        bits 0-6: function identifier (0-127)
 * 
 * Return: Function index (>=0) on success, -1 on failure
 */
static int zv_lookup_target_func(u64 inst_addr, unsigned char flag){
    int i;
    unsigned char func_id = FUNC_INDEX(flag);
    
    for (i = 0; i < func_count; i ++){
        if(funcs[i].hijack_flag == func_id) {
            /* First catch INT3 -> store origin_addr */
            if(! (flag & INT3_RETURN)) funcs[i].origin_addr = inst_addr;
            
            zv_log_write(LOG_NORMAL, "Func_replace", 
                        "Found function: %s, flag:%02hhx, addr:%llx",
                        funcs[i].func_name, funcs[i].hijack_flag, funcs[i].origin_addr);
            
            return i;
        }
    }

    return -1;
}

static void zv_trigger_cow_for_executable_pages(void){
    struct vm_area_struct *vma;
    struct mm_struct *mm = current->mm;
    int ret;

    if (!mm) {
        zv_log_write(LOG_NONE, "Func_replace", "Current process has no mm_struct");
        return;
    }

    zv_log_write(LOG_NORMAL, "Func_replace", "Setting executable pages writable for COW trigger");
    
    ret = mmap_write_lock_killable(mm);
    if (ret) {
        zv_log_write(LOG_NONE, "Func_replace", "Failed to acquire mmap write lock");
        return;
    }

    for (vma = mm->mmap; vma; vma = vma->vm_next){
        if (vma->vm_flags & VM_EXEC) {
            vma->vm_flags |= VM_WRITE | VM_MAYWRITE;
            vma->vm_flags &= ~ VM_DENYWRITE;
        }
    }

    mmap_write_unlock(mm);
    zv_log_write(LOG_DETAIL, "Func_replace", "COW trigger setup completed");
}

static void zv_int3_handler_dispatcher(
    int func_index,
    unsigned char flag
) {
    if (! (flag & INT3_RETURN)) {
        zv_handle_first_int3_hit(func_index, flag, (pid_parent == 0 || (!(current->pid == pid_parent))));
    } else {
        if(strcmp(funcs[func_index].func_name,"main") == 0) zv_handle_int3_return(func_index,false);
        else zv_handle_int3_return(func_index,true);
    }
}

/* Handle INT3 first hit */
static void zv_handle_first_int3_hit(
    int func_index,
    unsigned char flag,
    bool is_replace
) {
    unsigned long addr;
    int ret;
    struct zv_replace_inst inst_struct;

    if(pid_parent == 0) pid_parent = current->pid;

    /* Recovery the origin instruments */
    ret = copy_to_user(
        (usr_char)(funcs[func_index].origin_addr),
        funcs[func_index].saved_bytes,
        2
    );
    /* Do nothing*/
    if (!is_replace) {
        zv_write_vmcs(VM_GUEST_RIP, funcs[func_index].origin_addr);
        return;
    }

    /*Find inst and copy it into temp*/
    ret = zv_func_search(flag,&inst_struct);

    memcpy(
        inst_temp,
        inst_struct.inst_addr,
        inst_struct.inst_size
    );

    /*Handle inst(TO_DO)*/
    

    /* Flag INT3 has been hit */
    inst_temp[inst_struct.inst_size - 1] = flag | INT3_RETURN; //The end of the inst may not be inst.size -1 because of the previous process of inst

    /* Allocate memory for inserted instruments and copy*/
    addr = vm_mmap(
        NULL,
        0,
        PAGE_SIZE * 4,
        VM_READ| VM_WRITE | VM_EXEC, 
        MAP_ANONYMOUS | MAP_PRIVATE,
        0
    );

    ret = copy_to_user(
        (usr_char)addr,
        inst_temp,
        inst_struct.inst_size
    );

    zv_write_vmcs(VM_GUEST_RIP, (u64)addr); 
    return;
}

static void zv_handle_int3_return(int func_index , bool is_jump) {
    if (current->pid != pid_parent && pid_clone == 0) {
        pid_clone = current->pid;
        zv_log_write(LOG_NORMAL, "Func_replace", "Child pid:%d", pid_clone);
    }
    /* reset RIP */
    if(is_jump){
        zv_log_write(LOG_DEBUG, "Func_replace","reset and jump rip to: %16llx",funcs[func_index].origin_addr + 5);
        zv_write_vmcs(VM_GUEST_RIP, funcs[func_index].origin_addr + 5);
    }
    else {
        zv_log_write(LOG_DEBUG, "Func_replace","reset rip to: %16llx",funcs[func_index].origin_addr);
        zv_write_vmcs(VM_GUEST_RIP, funcs[func_index].origin_addr);
    }
}

//functions to debug
static pte_t *zv_get_user_pte(struct mm_struct *mm, unsigned long addr)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte; 

    if (!mm)
        return NULL;

    /* PGD */
    pgd = pgd_offset(mm, addr);
    if (pgd_none(*pgd) || pgd_bad(*pgd))
        return NULL;

    p4d = p4d_offset(pgd, addr);
    if (p4d_none(*p4d) || p4d_bad(*p4d))
        return NULL;

    pud = pud_offset(p4d, addr);
    if (pud_none(*pud) || pud_bad(*pud))
        return NULL;

    pmd = pmd_offset(pud, addr);
    if (pmd_none(*pmd) || pmd_bad(*pmd))
        return NULL;

    pte = pte_offset_map(pmd, addr);
    if (!pte)
        return NULL;

    return pte;
}

void zv_print_addr_pte(const char *name,u64 addr){
    pte_t *pte = zv_get_user_pte(current->mm,addr);
    if(pte == NULL) {
        zv_log_write(LOG_DEBUG, "Func_replace", "%s_PTE:%d",name,0);
        return;
    }
    zv_log_write(LOG_DEBUG, "Func_replace", "%s_PTE:%lx",name,pte_val(*pte));
}