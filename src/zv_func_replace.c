#include <linux/sched.h>
#include <linux/sched/mm.h>  // for get_task_mm
#include <linux/mm.h>
#include <linux/string.h>

#include "../include/zv_core.h"
#include <../include/zv_config.h>
#include "../include/zv_func_replace.h"
#include "../include/asm.h"
#include "../include/zv_log.h"

static pid_t pid_parent;
static pid_t pid_clone;
int func_count;
struct func funcs[128];
char proc_buffer[200];

unsigned char inst_clone[13]={
    0x50,                      //push   %rax
    0x48,0xc7,0xc0,0x39,0x00,0x00,0x00,  //mov    $0x39,%rax
    0x0f,0x05,                     //syscall
    0x58,                      //pop    %rax
    0xcc,                      //int3
};


static inline unsigned long zv_read_cr3(void);
static inline void zv_write_cr3(unsigned long val);
static inline void zv_write_cr0_wp_off(void);
static inline void zv_write_cr0_wp_on(void);
static ssize_t zv_proc_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos);
static int zv_find_func_index(u64 inst_addr,unsigned char flag);
static void zv_func_replace(int func_index,unsigned char flag,unsigned char *inst,int len);



void zv_vm_exit_callback_int3(u64 inst_addr,unsigned long cr3){
    int func_index;
    unsigned char flag;
    int result;
    /*save current cr3 and change cr3*/
    unsigned long host_cr3 = zv_read_cr3();
    zv_write_cr3(cr3);
    result = copy_from_user(&flag,(usr_char)(((void *)(inst_addr+1))),1);
    
    func_index = zv_find_func_index(inst_addr,flag);
    
    if(strcmp(funcs[func_index].name,"main") == 0){
        /*arg len should include flag*/
        zv_func_replace(func_index,flag,inst_clone,13);
    }else{
        zv_func_replace(func_index,flag,NULL,0);
    }

    /*recover cr3*/
    zv_write_cr3(host_cr3);
}

static void zv_func_replace(int func_index,unsigned char flag,unsigned char *inst,int len){
    
    unsigned long addr;
    int result;
    if(!(flag&INT3_RETURN)){
        pid_parent = current->pid;  
        zv_log_write(LOG_NORMAL,"Clone","in process:%d",pid_parent);
        zv_write_cr0_wp_off();
        /*allocate mem for the process*/
        addr = vm_mmap(NULL,0,CEIL(len,PAGE_SIZE) * PAGE_SIZE,
            VM_READ| VM_WRITE | VM_EXEC, 
            MAP_ANONYMOUS | MAP_PRIVATE,
            0
        );

        result = copy_to_user((usr_char)(funcs[func_index].addr),
            funcs[func_index].byte,2);

        /*if inst == NULL, do nothing*/
        if(!inst){
            zv_write_vmcs(VM_GUEST_RIP,funcs[func_index].addr);
            zv_write_cr0_wp_on();
            return;
        }

        inst[len-1] = flag | INT3_RETURN;//to identify which func is and whether in int3 return
        result = copy_to_user((usr_char)addr,inst,len);
        zv_write_vmcs(VM_GUEST_RIP,(u64)addr); 
        zv_write_cr0_wp_on();

    }else{
        if(current->pid != pid_parent){
            pid_clone = current->pid;
            zv_log_write(LOG_NORMAL,"Func_replace" , "Child pid:%d",current->pid);
        }
        /*change rip*/
        zv_write_vmcs(VM_GUEST_RIP,funcs[func_index].addr);
    }
    return;
}

static int zv_find_func_index(u64 inst_addr,unsigned char flag){
    int i;
    for( i = 0 ;i < func_count;i++){
        if(funcs[i].flag == FUNC_INDEX(flag)){
            if(!(flag&INT3_RETURN)) funcs[i].addr = inst_addr;
            zv_log_write(LOG_NORMAL, "Func_replace", "find func,name:%s,flag:%02hhx,addr:%llX",funcs[i].name,funcs[i].flag,funcs[i].addr);
            return i;
        }
    }
    return -1;
}

static ssize_t zv_proc_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
    int i;
    int pos=0;
    int read_setoff;
    if (count > BUFF_SIZE - 1)
        return -EINVAL;

    if (copy_from_user(proc_buffer, ubuf, count))
        return -EFAULT;

    proc_buffer[count] = '\0';
    sscanf(proc_buffer, "count: %d\n%n",&func_count,&read_setoff);
    pos += read_setoff;

    for(i = 0;i < func_count;i++){
        sscanf(proc_buffer + pos, "%s %02hhx%02hhx %02hhx\n%n",funcs[i].name,
            &(funcs[i].byte[0]),&(funcs[i].byte[1]),&(funcs[i].flag),&read_setoff);
        zv_log_write(LOG_NONE, "Func_replace", "func_name:%s func_bytes:%02hhx%02hhx func_flag:%02hhx",funcs[i].name,
            (funcs[i].byte[0]),(funcs[i].byte[1]),(funcs[i].flag));
        pos += read_setoff;
    }

    return count;
}

static const struct proc_ops proc_file_ops = {
    .proc_write = zv_proc_write,
};

int zv_func_replace_init(void)
{
    /*create proc entry for clone*/
    struct proc_dir_entry *entry;
    entry = proc_create("zv_func_replace", 0666, NULL, &proc_file_ops);
    if (!entry) {
        zv_log_write(LOG_NONE,"Func_replace","failed to create zv_func_replace_entry");
        return -ENOMEM;
    }
    zv_log_write(LOG_NORMAL,"Func_replace","/proc/zv_func_replace created");
    return 0;
}

static inline unsigned long zv_read_cr3(void)
{
    unsigned long val;
    asm volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void zv_write_cr3(unsigned long val)
{
    asm volatile("mov %0, %%cr3" :: "r"(val) : "memory");
}

static inline void zv_write_cr0_wp_off(void)
{
    unsigned long cr0;

    asm volatile("mov %%cr0, %0" : "=r" (cr0));
    cr0 &= ~(1UL << 16);
    asm volatile("mov %0, %%cr0" :: "r" (cr0) : "memory");

}

static inline void zv_write_cr0_wp_on(void)
{
    unsigned long cr0;

    asm volatile("mov %%cr0, %0" : "=r" (cr0));
    cr0 |= (1UL << 16);
    asm volatile("mov %0, %%cr0" :: "r" (cr0) : "memory");
}