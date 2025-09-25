#include <linux/kernel.h>
#include<../include/zv_func_search.h>
#include <../include/zv_log.h>

static struct zv_inst_table_header *inst_header;
static struct zv_inst_tb_entry *zv_inst_tb;

static bool is_init = false;

static void zv_func_search_init(void){
    inst_header = (void*)zv_inst_buf;
    zv_log_write(LOG_NORMAL,"Func_find","find func to replace : %lld",inst_header->num_inst_to_replace);
    zv_inst_tb = (void*)(inst_header->inst_tb_offset + (unsigned char *)inst_header);
}

extern int zv_func_search(unsigned char flag , struct zv_replace_inst *ret){
    int i;
    if(!is_init){
        zv_func_search_init();
        is_init = true;
    }
    for(i = 0 ; i < inst_header->num_inst_to_replace ; i++){
        if(zv_inst_tb[i].flag == flag){
            zv_log_write(LOG_DEBUG,"Func_find","find func to replace : %llu size : %llu",zv_inst_tb[i].flag,zv_inst_tb[i].inst_size);
            ret->inst_addr =  (void*)((char *)inst_header + zv_inst_tb[i].inst_offset);
            ret->inst_size = zv_inst_tb[i].inst_size;
            ret->flag = zv_inst_tb[i].flag;
            return 0;
        }
    }
    return -1;
}

