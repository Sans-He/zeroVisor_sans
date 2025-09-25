#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/pagewalk.h>

#include "../include/zv_types.h"
#include "../include/zv_func_search.h"

#define MAP_ANONYMOUS	0x20		/* don't use a file */
#define MAP_PRIVATE	0x02		/* Changes are private */
#define INT3_RETURN (0x01<<7)
#define FUNC_INDEX(flag) (flag&(~INT3_RETURN))
#define BUFF_SIZE 128

typedef unsigned char __user* usr_char;
                
extern int zv_func_replace_init(void);
void zv_func_replace_exit(void);
extern void zv_handle_function_hijack(u64 inst_addr,unsigned long guest_cr3);

/*
 * Function hijack target structure
 * Defines a function that will be intercepted for code injection.
 * 
 * Flag encoding:
 *   bit 7: INT3_RETURN flag (1 = returning from int3, 0 = first int3 hit)  
 *   bits 0-6: function identifier (0-127, used to distinguish functions)
 */
struct zv_hijack_target_func {
    char func_name[100];           /* Name of function to intercept */
    unsigned char saved_bytes[2];   /* Saved original bytes */
    unsigned char hijack_flag;        /* hijack identifier + flags */
    u64 origin_addr;             /* Function address */
};

