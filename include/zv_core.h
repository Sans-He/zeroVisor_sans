#include <linux/types.h>

/* Variables */
extern u64 g_max_ram_size;
extern struct desc_ptr g_gdtr_array[];
extern struct zv_share_context* g_share_context;
extern void* g_guest_vmcs_log_addr[];

/* Functions */
u64 zv_get_symbol_address(char* symbol);
void zv_hide_range(u64 start_addr, u64 end_addr, int alloc_type);
u64 zv_calc_vm_pre_timer_value(void);