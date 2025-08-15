// Dynamically added from /proc/kallsyms
#define KALLSYMS_LOOKUP_NAME_ADDR 0xffffffff8118c8c0
char* g_kernel_version[] = 
{
	"#1 SMP Tue Aug 5 09:45:46 CST 2025",
};

struct zv_symbol_table g_symbol_table_array[] =
{
	//#1 SMP Tue Aug 5 09:45:46 CST 2025
	{
		{
			{"_text", 0xffffffff81000000},
			{"walk_system_ram_range", 0xffffffff810c2200},
			{"wake_up_new_task", 0xffffffff810f8bd0},
			{"free_module", 0xffffffff811883c0},
			{"ftrace_module_init", 0xffffffff811de0e0},
			{"_etext", 0xffffffff820029f2},
			{"__start_rodata", 0xffffffff82200000},
			{"__start___ex_table", 0xffffffff8278f5c0},
			{"__stop___ex_table", 0xffffffff82790d60},
			{"__end_rodata", 0xffffffff82791000},
			{"tasklist_lock", 0xffffffff82806080},
			{"modules", 0xffffffff829d9a40},
			{"init_mm", 0xffffffff82a26000},
		},
	},
};

