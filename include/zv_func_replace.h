#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include "../include/zv_types.h"

#define MAP_ANONYMOUS	0x20		/* don't use a file */
#define MAP_PRIVATE	0x02		/* Changes are private */
#define INT3_RETURN (0x01<<7)
#define FUNC_INDEX(flag) (flag&(~INT3_RETURN))
#define BUFF_SIZE 128

typedef unsigned char __user* usr_char;

extern int zv_func_replace_init(void);
extern void zv_vm_exit_callback_int3(u64 inst_addr,unsigned long cr3);


struct func{
    char name[100];
    unsigned char byte[2];
    unsigned char flag;
    u64 addr;
};
