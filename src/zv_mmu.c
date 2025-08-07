#include <linux/mm.h>
#include <asm/io.h>


#include "../include/zv_mmu.h"
#include "../include/zv_core.h"
#include "../include/zv_log.h"
#include "../include/zv_mem_manager.h"
#include "../include/zv_config.h"

/* Variables */
static u64 g_ram_end;

DEFINE_XARRAY(zv_pml4_table);
DEFINE_XARRAY(zv_pdpte_pd_table);
DEFINE_XARRAY(zv_pdept_table);
DEFINE_XARRAY(zv_pte_table);

/* Static functions declarations */
static void zv_setup_ept_system_ram_range(void);
static int zv_callback_set_write_back_to_ram(unsigned long start, unsigned long size, void* arg);

static void zv_set_ept_page(struct zv_ept_info* ept_info_high , int type , u64 start_align);
static void zv_set_ept_page_flags(u64 phy_addr, u32 flags);
static void zv_set_ept_page_addr(u64 phy_addr, u64 addr);

/* Static functions related to resource*/
static void zv_setup_ept_iomem_ram_range(u64 start_addr);
static struct resource *zv_get_next_resource(struct resource *p, bool sibling_only);
static int zv_callback_set_ept_to_iomem(struct resource* res,void * arg);

static int zv_callback_walk_ram(unsigned long start, unsigned long size, void* arg) {
    zv_log_write(LOG_DEBUG, "MMU", "System RAM start %016lX, end %016lX, "
        "size %016lX", start * PAGE_SIZE, start * PAGE_SIZE + size * PAGE_SIZE,
		size * PAGE_SIZE);

    if (g_ram_end < ((start + size) * PAGE_SIZE)) {
        g_ram_end = (start + size) * PAGE_SIZE;
    }

    return 0;
}

/* Calculate System RAM size */
u64 zv_get_max_ram_size(void) {
    my_walk_system_ram_range func = NULL;
    unsigned long *p_max_pfn = NULL;
    unsigned long range_end = 0;

    g_ram_end = 0;

    func = (my_walk_system_ram_range)zv_get_symbol_address("walk_system_ram_range");
    if (func == NULL) {
        zv_log_write(LOG_NONE, "MMU", "walk_system_ram_range address get fail");
        return totalram_pages() * 2 * PAGE_SIZE;
    }

    // get max_pfn
    p_max_pfn = (unsigned long *)zv_get_symbol_address("max_pfn");
    if (p_max_pfn && *p_max_pfn > 0) {
        range_end = *p_max_pfn;
        zv_log_write(LOG_DEBUG, "MMU", "Using max_pfn = %lu", range_end);
    } else {
        // fallback
        range_end = totalram_pages() * 2;
        zv_log_write(LOG_DEBUG, "MMU", "Fallback: max_pfn not found, using totalram_pages * 2 = %lu", range_end);
    }

    func(0, range_end, NULL, zv_callback_walk_ram);
    
    return g_ram_end;
}

void zv_protect_ept_pages(void){
    //to do
}

/* Set write-back permission to System RAM area */
static void zv_setup_ept_system_ram_range(void) {
    my_walk_system_ram_range func = NULL;

    func = (my_walk_system_ram_range)zv_get_symbol_address("walk_system_ram_range");
    if (! func) {
        zv_log_write(LOG_NONE, "MMU", "walk_system_ram_range address get fail");
        return;
    }

    func(0, g_max_ram_size / PAGE_SIZE, NULL, zv_callback_set_write_back_to_ram);
}

/*Set ept for iomem*/
static void zv_setup_ept_iomem_ram_range(u64 start_addr) {

    my_walk_iomem_ram_range func = NULL;

    func = (my_walk_iomem_ram_range)zv_get_symbol_address("walk_iomem_res_desc");
    if (! func) {
        zv_log_write(LOG_NONE, "MMU", "zwalk_iomem_res_desc get fail");
        return;
    }

    func(IORES_DESC_NONE , 0 , start_addr , -1ULL , NULL ,zv_callback_set_ept_to_iomem);
}

/*
 * Process callback of walk_system_ram_range().
 *
 * This function sets write-back cache type to EPT page.
 */
static int zv_callback_set_write_back_to_ram(
    unsigned long start,
    unsigned long size,
    void* arg
) {
    struct zv_ept_pagetable* ept_info;
    unsigned long i;

    zv_log_write(LOG_DEBUG, "MMU", "System RAM start %016lX, end %016lX, size %016lX",
        start * PAGE_SIZE, (start + size) * PAGE_SIZE, size * PAGE_SIZE);

    for (i = start; i < start + size; i ++) {
        ept_info = (struct zv_ept_pagetable*)zv_get_pagetable_log_addr(EPT_TYPE_PTE, i / EPT_PAGE_ENT_COUNT);
        ept_info->entry[i % EPT_PAGE_ENT_COUNT] |= EPT_BIT_MEM_TYPE_WB;
    }

    return 0;
}



/*
 * Hide a physical page to protect it from the guest.
 *
 * When zeroVisor sets no permission to the page, error is occured in some system.
 * So, for hiding a physical page, zeroVisor sets read-only permission to the page
 * and maps guest physical page to page number 0.
 */
void zv_set_ept_hide_page(u64 phy_addr) {
    zv_set_ept_page_flags(phy_addr, EPT_READ | EPT_BIT_MEM_TYPE_WB);
    zv_set_ept_page_addr(phy_addr, 0); // guest access -> #PF
}

/*
 * Lock a physical page to protect it from the guest.
 */
void zv_set_ept_lock_page(u64 phy_addr) {
	zv_set_ept_page_flags(phy_addr, EPT_READ | EPT_EXECUTE | EPT_BIT_MEM_TYPE_WB);
	zv_set_ept_page_addr(phy_addr, phy_addr);
}

/* Set all permissions to a physical page */
void zv_set_ept_all_access_page(u64 phy_addr) {
    zv_set_ept_page_flags(phy_addr, EPT_ALL_ACCESS | EPT_BIT_MEM_TYPE_WB);
    zv_set_ept_page_addr(phy_addr, phy_addr);
}

/* Set permissions to a physical page in EPT */
static void zv_set_ept_page_flags(u64 phy_addr, u32 flags) {
    u64 page_offset;
    u64* page_table_addr;
    u64 page_index;

    page_offset = phy_addr / EPT_PAGE_SIZE;
    page_index = page_offset % EPT_PAGE_ENT_COUNT;
    page_table_addr = zv_get_pagetable_log_addr(EPT_TYPE_PTE,
        page_offset / EPT_PAGE_ENT_COUNT);
    page_table_addr[page_index] =
        (page_table_addr[page_index] & MASK_PAGEADDR) | flags;
}

/* Change physical address in EPT */
static void zv_set_ept_page_addr(u64 phy_addr, u64 addr) {
    u64 page_offset;
    u64* page_table_addr;
    u64 page_index;

    page_offset = phy_addr / EPT_PAGE_SIZE;
    page_index = page_offset % EPT_PAGE_ENT_COUNT;
    page_table_addr = zv_get_pagetable_log_addr(EPT_TYPE_PTE,
        page_offset / EPT_PAGE_ENT_COUNT);
    page_table_addr[page_index] = (addr & MASK_PAGEADDR)
        | (page_table_addr[page_index] & ~MASK_PAGEADDR); // extra flags 
}

u64 guest_to_host(u64 x){
	struct zv_ept_pagetable * ept_ptr;
	struct zv_ept_pagetable * PDPE_addr;
	struct zv_ept_pagetable * PDE_addr;
	struct zv_ept_pagetable * PTE_addr;
	struct zv_ept_pagetable * phy_addr;

	u64 PML4E_offset = (x>>39) & MASK_EPT_OFFSET;
	u64 PDPE_offset = (x>>30) & MASK_EPT_OFFSET;
	u64 PDE_offset = (x>>21) & MASK_EPT_OFFSET;
	u64 PTE_offset = (x>>12) & MASK_EPT_OFFSET;
	u64 phy_offset = x & ~MASK_PAGEADDR;
	

	ept_ptr = (void*)xa_load(&zv_pml4_table,0);
	PDPE_addr = CHANGE_ADDR(ept_ptr->entry[PML4E_offset])
	PDE_addr = CHANGE_ADDR(PDPE_addr->entry[PDPE_offset])
	PTE_addr = CHANGE_ADDR(PDE_addr->entry[PDE_offset])
	phy_addr = (void*)((PTE_addr->entry[PTE_offset])&(~((u64)0xfff)));


	return (u64)phy_addr +  phy_offset;
} 

void * zv_get_pagetable_log_addr(unsigned long type, int index){
    void* entry;
    int result;
    struct xarray * xa;
    switch (type) {
        case EPT_TYPE_PML4: 
            entry = xa_load(&zv_pml4_table,index);
            xa = &zv_pml4_table;
            break;
        
        case EPT_TYPE_PDPTEPD:
            entry = xa_load(&zv_pdpte_pd_table,index);
            xa = &zv_pdpte_pd_table;
            break;
        
        case EPT_TYPE_PDEPT:
            entry = xa_load(&zv_pdept_table,index);
            xa = &zv_pdept_table;
            break;

        case EPT_TYPE_PTE:
            entry = xa_load(&zv_pte_table,index);
            xa = &zv_pte_table;
            break;
        
        default:
            break;
    }
    if(!entry){
        entry = zv_kmalloc(EPT_PAGE_SIZE,GFP_ATOMIC);
        zv_log_write(LOG_DETAIL,"MMU", "addr not found in xarray , allocate new page:%px",entry);
        result = xa_insert(xa,index,entry,GFP_ATOMIC);
    }
    return entry;
}


void * zv_get_pagetable_phy_addr(unsigned long type, int index){
    return (void*)virt_to_phys(zv_get_pagetable_log_addr(type,index));

}

static void zv_set_ept_page(struct zv_ept_info* ept_info_high , int type , u64 start_align){
    struct zv_pagetable* pagetable;
    u64 offset_ept;
    u64 offset_start;
    u64 offset_real;//offset in page
    u64 ent_count;
    u64 index;//index of page
    u64 i;
    unsigned long index_base;
    unsigned long index_base_next;
    switch (type){
        case EPT_TYPE_PML4:
            offset_ept = 39;
            ent_count = ept_info_high->pml4_ent_count;
            index_base = start_align/VAL_256TB;
            index_base_next = start_align/VAL_512GB;
            break;
        case EPT_TYPE_PDPTEPD:
            offset_ept = 30;
            ent_count = ept_info_high->pdpte_pd_ent_count;
            index_base = start_align/VAL_512GB;
            index_base_next = start_align/VAL_1GB;
            break;
        case EPT_TYPE_PDEPT:
            offset_ept = 21;
            ent_count = ept_info_high->pdept_ent_count;
            index_base = start_align/VAL_1GB;
            index_base_next = start_align/VAL_2MB;
            break;
        case EPT_TYPE_PTE:
            offset_ept = 12;
            ent_count = ept_info_high->pte_ent_count;
            index_base = start_align/VAL_2MB;
            break;
        default:
            break;
    }

    offset_start = (start_align>>offset_ept) & MASK_EPT_OFFSET;
    pagetable = zv_get_pagetable_log_addr(type,index_base);
    for(i = 0;i < ent_count ; i++){
        offset_real = (i + offset_start) % 512;
        if(offset_real == 0){
            index = (i + offset_start)/512;
            pagetable = zv_get_pagetable_log_addr(type,index + index_base);
        }
        if(type != EPT_TYPE_PTE){
            pagetable->entry[offset_real] = (u64)zv_get_pagetable_phy_addr(type + 1,index_base_next + i) | EPT_ALL_ACCESS;
        }else{
            pagetable->entry[offset_real] = (start_align + i * EPT_PAGE_SIZE) | EPT_ALL_ACCESS;
        }
        
    }
}

void zv_add_mem_range(u64 start,u64 end){
    struct zv_ept_info ept_info_high;
    u64 start_align =  start & MASK_PAGEADDR;
    u64 end_align = (end + 0xfff) & MASK_PAGEADDR;
    u64 size = (end_align - start_align);
    /*calculate entry count*/
    ept_info_high.pml4_ent_count     = CEIL(size , VAL_512GB);
    ept_info_high.pdpte_pd_ent_count = CEIL(size , VAL_1GB);
    ept_info_high.pdept_ent_count    = CEIL(size , VAL_2MB);
    ept_info_high.pte_ent_count      = CEIL(size , VAL_4KB);
    /*calculate page count*/
    ept_info_high.pml4_page_count     = CEIL(size , VAL_256TB);
    ept_info_high.pdpte_pd_page_count = CEIL(size , VAL_512GB);
    ept_info_high.pdept_page_count    = CEIL(size , VAL_1GB);
    ept_info_high.pte_page_count      = CEIL(size , VAL_2MB);
    /*delete page_array in ept_info so we needn't to alloct space,we now just use it to store basic ept info*/
    zv_set_ept_page(&ept_info_high,EPT_TYPE_PML4,start_align);
    zv_set_ept_page(&ept_info_high,EPT_TYPE_PDPTEPD,start_align);
    zv_set_ept_page(&ept_info_high,EPT_TYPE_PDEPT,start_align);
    zv_set_ept_page(&ept_info_high,EPT_TYPE_PTE,start_align);
    /*when start addr = 0 means initial of the moudule,so we initialize iomem*/
    if(start == 0){
        zv_setup_ept_system_ram_range();
        zv_setup_ept_iomem_ram_range(end_align);
    }
}

static struct resource *zv_get_next_resource(struct resource *p, bool sibling_only)
{
	/* Caller wants to traverse through siblings only */
	if (sibling_only)
		return p->sibling;
	if (p->child)
		return p->child;
	while (!p->sibling && p->parent)
		p = p->parent;
	return p->sibling;
}

static int zv_callback_set_ept_to_iomem(struct resource* res,void * arg){
    struct resource *p;
    struct resource * (*func)(struct resource * , resource_size_t) = (void*)zv_get_symbol_address("lookup_resource");
    /*
        function walk_iomem_res_desc doesn't return a complete resource struct(without child),
    so we use lookup_resource to get the address.
    */
    res = func(&iomem_resource,res->start);
    p = res;
    zv_log_write(LOG_DEBUG,"MMU","find resource: %s, start_addr: %16llX end_addr: %16llX child:%p sibling:%p ",
        res->name,res->start,res->end,res->child,res->sibling);
    p = zv_get_next_resource(p,0);
    while(zv_get_next_resource(p,0) != NULL){
        p = zv_get_next_resource(p,0);
        if(!(p->child)){
            zv_add_mem_range(p->start,p->end);
            zv_log_write(LOG_DEBUG,"MMU","add resource: %s, start_addr: %16llX end_addr: %16llX",p->name,p->start,p->end);
        }
    }
    return 0;
}