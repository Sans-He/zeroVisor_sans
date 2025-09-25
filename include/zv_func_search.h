#ifndef ZV_FUNC_SEARCH
#define ZV_FUNC_SEARCH
#include<linux/types.h>
#include "../include/zv_dev_memory.h"

struct zv_replace_inst{
    void * inst_addr;
    int inst_size;
    int flag;
};

struct zv_inst_table_header{
    u64 num_inst_to_replace;
    u64 inst_tb_size;
    u64 inst_tb_offset;
};

struct zv_inst_tb_entry{
    unsigned char flag;
    u64 inst_offset;
    u64 inst_size;
};

extern int zv_func_search(unsigned char flag , struct zv_replace_inst *answer);

#endif