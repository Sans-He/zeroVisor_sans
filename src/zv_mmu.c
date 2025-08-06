#include <linux/mm.h>
#include <asm/io.h>

#include "../include/zv_mmu.h"
#include "../include/zv_core.h"
#include "../include/zv_log.h"
#include "../include/zv_mem_manager.h"
#include "../include/zv_config.h"

/* Variables */
static u64 g_ram_end;
struct zv_ept_info g_ept_info = {0, };

/* Static functions declarations */
static void zv_setup_ept_system_ram_range(void);
static int zv_callback_set_write_back_to_ram(unsigned long start, unsigned long size, void* arg);
static void zv_set_ept_page_flags(u64 phy_addr, u32 flags);
static void zv_set_ept_page_addr(u64 phy_addr, u64 addr);

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

int zv_alloc_ept_pages(void) {
    int i;

    g_ept_info.pml4_ent_count = CEIL(g_max_ram_size, VAL_512GB);
    g_ept_info.pdpte_pd_ent_count = CEIL(g_max_ram_size, VAL_1GB);
    g_ept_info.pdept_ent_count = CEIL(g_max_ram_size, VAL_2MB);
	g_ept_info.pte_ent_count = CEIL(g_max_ram_size, VAL_4KB);

    g_ept_info.pml4_page_count = CEIL(g_ept_info.pml4_ent_count, EPT_PAGE_ENT_COUNT);
	g_ept_info.pdpte_pd_page_count = CEIL(g_ept_info.pdpte_pd_ent_count, EPT_PAGE_ENT_COUNT);
	g_ept_info.pdept_page_count = CEIL(g_ept_info.pdept_ent_count, EPT_PAGE_ENT_COUNT);
	g_ept_info.pte_page_count = CEIL(g_ept_info.pte_ent_count, EPT_PAGE_ENT_COUNT);

    zv_log_write(LOG_DEBUG, "MMU", "Setup EPT, Max RAM Size %ld", g_max_ram_size);
    zv_log_write(LOG_DEBUG, "MMU", "    [*] EPT Size: %d", 
        (int)sizeof(struct zv_ept_pagetable));
    zv_log_write(LOG_DEBUG, "MMU", "    [*] PML4 Entry Count: %d", 
        (int)g_ept_info.pml4_ent_count);
    zv_log_write(LOG_DEBUG, "MMU", "    [*] PDPTE PD Entry Count: %d", 
        (int)g_ept_info.pdpte_pd_ent_count);
    zv_log_write(LOG_DEBUG, "MMU", "    [*] PDE PT Entry Count: %d", 
        (int)g_ept_info.pdept_ent_count);
    zv_log_write(LOG_DEBUG, "MMU", "    [*] PTE Entry Count: %d", 
        (int)g_ept_info.pte_ent_count);

    zv_log_write(LOG_DEBUG, "MMU", "    [*] PML4 Page Count: %d", 
        (int)g_ept_info.pml4_page_count);
    zv_log_write(LOG_DEBUG, "MMU", "    [*] PDPTE PD Page Count: %d", 
        (int)g_ept_info.pdpte_pd_page_count);
    zv_log_write(LOG_DEBUG, "MMU", "    [*] PDE PT Page Count: %d", 
        (int)g_ept_info.pdept_page_count);
    zv_log_write(LOG_DEBUG, "MMU", "    [*] PTE Page Count: %d", 
        (int)g_ept_info.pte_page_count);

    /* Allocate memory for page table */
    g_ept_info.pml4_page_addr_array = (u64*)zv_vmalloc(g_ept_info.pml4_page_count * sizeof(u64*));
    g_ept_info.pdpte_pd_page_addr_array = (u64*)zv_vmalloc(g_ept_info.pdpte_pd_page_count * sizeof(u64*));
    g_ept_info.pdept_page_addr_array = (u64*)zv_vmalloc(g_ept_info.pdept_page_count * sizeof(u64*));
    g_ept_info.pte_page_addr_array = (u64*)zv_vmalloc(g_ept_info.pte_page_count * sizeof(u64*));

    if (   (! g_ept_info.pml4_page_addr_array)
        || (! g_ept_info.pdpte_pd_page_addr_array)
        || (! g_ept_info.pdept_page_addr_array)
        || (! g_ept_info.pte_page_addr_array)
    ) {
        zv_log_write(LOG_NONE, "MMU", "zv_alloc_ept_pages alloc fail");
		return -1;
	}

    for (i = 0; i < g_ept_info.pml4_page_count; i ++) {
        g_ept_info.pml4_page_addr_array[i] = (u64)zv_kmalloc(0x1000, GFP_KERNEL);
        
        if (! g_ept_info.pml4_page_addr_array[i]) {
            zv_log_write(LOG_NONE, "MMU", "zv_alloc_ept_pages alloc fail");
            return -1;
        }
    }

    for (i = 0; i < g_ept_info.pdpte_pd_page_count; i ++) {
        g_ept_info.pdpte_pd_page_addr_array[i] = (u64)zv_kmalloc(0x1000, GFP_KERNEL);

        if (! g_ept_info.pdpte_pd_page_addr_array[i]) {
            zv_log_write(LOG_NONE, "MMU", "zv_alloc_ept_pages alloc fail");
            return -1;
        }
    }

    for (i = 0; i < g_ept_info.pdept_page_count; i ++) {
        g_ept_info.pdept_page_addr_array[i] = (u64)zv_kmalloc(0x1000, GFP_KERNEL);
        
        if (! g_ept_info.pdept_page_addr_array[i]) {
            zv_log_write(LOG_NONE, "MMU", "zv_alloc_ept_pages alloc fail");
            return -1;
        }
    }
    
    for (i = 0; i < g_ept_info.pte_page_count; i ++) {
        g_ept_info.pte_page_addr_array[i] = (u64)zv_kmalloc(0x1000, GFP_KERNEL);
        
        if (! g_ept_info.pte_page_addr_array[i]) {
            zv_log_write(LOG_NONE, "MMU", "zv_alloc_ept_pages alloc fail");
            return -1;
        }
    }

    zv_log_write(LOG_DETAIL, "MMU", "   [*] Page Table Memory Allocate Success");

    return 0;
}

/* Setup EPT */
void zv_setup_ept_pagetables(void) {
    struct zv_ept_pagetable* ept_info;
    u64 next_page_table_addr;
    u64 i, j;
    u64 loop_cnt;
    u64 base_addr;

    /* Setup PML4 */
    zv_log_write(LOG_DETAIL, "MMU", "Setup PML4");
    ept_info = (struct zv_ept_pagetable*)zv_get_pagetable_log_addr(EPT_TYPE_PML4, 0);
    zv_log_write(LOG_DETAIL, "MMU", "   [*] Setup PML4 %016lX", (u64)ept_info);
    memset(ept_info, 0, sizeof(struct zv_ept_pagetable));

    base_addr = 0;
    for (i = 0; i < EPT_PAGE_ENT_COUNT; i ++) {
        if (i < g_ept_info.pml4_ent_count) {
            next_page_table_addr = (u64)zv_get_pagetable_phy_addr(EPT_TYPE_PDPTEPD, i);
            ept_info->entry[i] = next_page_table_addr | EPT_ALL_ACCESS;

            if (i == 0) {
                zv_log_write(LOG_DETAIL, "MMU", "   [*] %016lX", (u64)next_page_table_addr);
            }
        } else {
            ept_info->entry[i] = base_addr | EPT_ALL_ACCESS;
        }

        base_addr += VAL_512GB;
    }

    /* Setup PDPTE PD */
    zv_log_write(LOG_DETAIL, "MMU", "Setup PDPTE PD");
    base_addr = 0;
    for (j = 0; j < g_ept_info.pdpte_pd_page_count; j ++) {
        ept_info = (struct zv_ept_pagetable*)zv_get_pagetable_log_addr(EPT_TYPE_PDPTEPD, j);
        zv_log_write(LOG_DETAIL, "MMU", "   [*] Setup PDPTEPD [%d] %016lX", j, (u64)ept_info);
        memset(ept_info, 0, sizeof(struct zv_ept_pagetable));

        loop_cnt = g_ept_info.pdpte_pd_ent_count - (j * EPT_PAGE_ENT_COUNT);
        loop_cnt = loop_cnt > EPT_PAGE_ENT_COUNT ? EPT_PAGE_ENT_COUNT : loop_cnt;

        for (i = 0; i < EPT_PAGE_ENT_COUNT; i ++) {
            if (i < loop_cnt) {
                next_page_table_addr = (u64)zv_get_pagetable_phy_addr(EPT_TYPE_PDEPT,
                    (j * EPT_PAGE_ENT_COUNT) + i);
                ept_info->entry[i] = next_page_table_addr | EPT_ALL_ACCESS;

                if (i == 0) {
                    zv_log_write(LOG_DETAIL, "MMU", "   [*] %016lX", (u64)next_page_table_addr);
                }
            } else {
                ept_info->entry[i] = base_addr | EPT_ALL_ACCESS;
            }

            base_addr += VAL_1GB;
        }
    }

    /* Setup PDEPT */
    zv_log_write(LOG_DETAIL, "MMU", "Setup PDEPT");
    base_addr = 0;
    for (j = 0; j < g_ept_info.pdept_page_count; j ++) {
        ept_info = (struct zv_ept_pagetable*)zv_get_pagetable_log_addr(EPT_TYPE_PDEPT, j);
        zv_log_write(LOG_DETAIL, "MMU", "   [*] Setup PDEPT [%d] %016lX", j, (u64)ept_info);
        memset(ept_info, 0, sizeof(struct zv_ept_pagetable));

        loop_cnt = g_ept_info.pdept_ent_count - (j * EPT_PAGE_ENT_COUNT);
        loop_cnt = loop_cnt > EPT_PAGE_ENT_COUNT ? EPT_PAGE_ENT_COUNT : loop_cnt;


        for (i = 0; i < EPT_PAGE_ENT_COUNT; i ++) {
            if (i < loop_cnt) {
                next_page_table_addr = (u64)zv_get_pagetable_phy_addr(EPT_TYPE_PTE, 
                    (j * EPT_PAGE_ENT_COUNT) + i);
                ept_info->entry[i] = next_page_table_addr | EPT_ALL_ACCESS;

                if (i == 0) {
                    zv_log_write(LOG_DETAIL, "MMU", "   [*] %016lX", (u64)next_page_table_addr);
                }
            } else {
                ept_info->entry[i] = base_addr | EPT_ALL_ACCESS;
            }

            base_addr += VAL_2MB;
        }
    }

    /* Setup PTE */
    zv_log_write(LOG_DETAIL, "MMU", "Setup PTE");
    for (j = 0; j < g_ept_info.pte_page_count; j ++) {
        ept_info = (struct zv_ept_pagetable*)zv_get_pagetable_log_addr(EPT_TYPE_PTE, j);
        memset(ept_info, 0, sizeof(struct zv_ept_pagetable));

        loop_cnt = g_ept_info.pte_ent_count - (j * EPT_PAGE_ENT_COUNT);
        loop_cnt = loop_cnt > EPT_PAGE_ENT_COUNT ? EPT_PAGE_ENT_COUNT : loop_cnt;


        for (i = 0; i < EPT_PAGE_ENT_COUNT; i ++) {
            if (i < loop_cnt) {
                next_page_table_addr = ((u64)j * EPT_PAGE_ENT_COUNT + i) * EPT_PAGE_SIZE;

                /*
				 * Set uncacheable type by default.
				 * Set write-back type to "System RAM" areas at the end of this
				 * function.
				 */
				ept_info->entry[i] = next_page_table_addr | EPT_ALL_ACCESS;
            } else {
                ept_info->entry[i] = base_addr | EPT_ALL_ACCESS;
            }

            base_addr += VAL_4KB;
        }
    }

    /* Set write-back type to "System RAM" areas */
    zv_setup_ept_system_ram_range();
}

/* Get logical address of page table pointer of index and type */
void* zv_get_pagetable_log_addr(int type, int index) {
    u64* table_array_addr;
    
    switch (type) {
        case EPT_TYPE_PML4: 
            table_array_addr = g_ept_info.pml4_page_addr_array;
            break;
        
        case EPT_TYPE_PDPTEPD:
            table_array_addr = g_ept_info.pdpte_pd_page_addr_array;
            break;
        
        case EPT_TYPE_PDEPT:
            table_array_addr = g_ept_info.pdept_page_addr_array;
            break;

        case EPT_TYPE_PTE:
            table_array_addr = g_ept_info.pte_page_addr_array;
            break;
        
        default:
            table_array_addr = g_ept_info.pte_page_addr_array;
            break;
    }

    return (void*)table_array_addr[index];
}

/* Get physical address of page table pointer of index and type */
void* zv_get_pagetable_phy_addr(int type, int index) {
    void* table_log_addr;
    
    table_log_addr = zv_get_pagetable_log_addr(type, index);
    return (void*)virt_to_phys(table_log_addr);
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

/* Protect page table memory for EPT */
void zv_protect_ept_pages(void) {
    int i;
    u64 end;

    zv_log_write(LOG_DEBUG, "MMU", "Protect EPT");

    /* Hide the EPT page table */
    end = (u64)g_ept_info.pml4_page_addr_array + 
        g_ept_info.pml4_page_count * sizeof(u64*);
	zv_hide_range((u64)g_ept_info.pml4_page_addr_array, end, ALLOC_VMALLOC);

	end = (u64)g_ept_info.pdpte_pd_page_addr_array +
		g_ept_info.pdpte_pd_page_count * sizeof(u64*);
	zv_hide_range((u64)g_ept_info.pdpte_pd_page_addr_array, end, ALLOC_VMALLOC);

	end = (u64)g_ept_info.pdept_page_addr_array +
		g_ept_info.pdept_page_count * sizeof(u64*);
	zv_hide_range((u64)g_ept_info.pdept_page_addr_array, end, ALLOC_VMALLOC);

	end = (u64)g_ept_info.pte_page_addr_array +
		g_ept_info.pte_page_count * sizeof(u64*);
	zv_hide_range((u64)g_ept_info.pte_page_addr_array, end, ALLOC_VMALLOC);

    for (i = 0 ; i < g_ept_info.pml4_page_count; i ++)
	{
		end = (u64)g_ept_info.pml4_page_addr_array[i] + EPT_PAGE_SIZE;
		zv_hide_range((u64)g_ept_info.pml4_page_addr_array[i], end,
			ALLOC_KMALLOC);
	}

	for (i = 0 ; i < g_ept_info.pdpte_pd_page_count; i ++)
	{
		end = (u64)g_ept_info.pdpte_pd_page_addr_array[i] + EPT_PAGE_SIZE;
		zv_hide_range((u64)g_ept_info.pdpte_pd_page_addr_array[i], end,
			ALLOC_KMALLOC);
	}

	for (i = 0 ; i < g_ept_info.pdept_page_count; i ++)
	{
		end = (u64)g_ept_info.pdept_page_addr_array[i] + EPT_PAGE_SIZE;
		zv_hide_range((u64)g_ept_info.pdept_page_addr_array[i], end,
			ALLOC_KMALLOC);
	}

	for (i = 0 ; i < g_ept_info.pte_page_count; i ++)
	{
		end = (u64)g_ept_info.pte_page_addr_array[i] + EPT_PAGE_SIZE;
		zv_hide_range((u64)g_ept_info.pte_page_addr_array[i], end,
			ALLOC_KMALLOC);
	}

    zv_log_write(LOG_DEBUG, "MMU", "    [*] Complete");
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
	

	ept_ptr = (void*)g_ept_info.pml4_page_addr_array[0];

	PDPE_addr = CHANGE_ADDR(ept_ptr->entry[PML4E_offset])
	PDE_addr = CHANGE_ADDR(PDPE_addr->entry[PDPE_offset])
	PTE_addr = CHANGE_ADDR(PDE_addr->entry[PDE_offset])

	phy_addr = (void*)((PTE_addr->entry[PTE_offset])&(~((u64)0xfff)));


	return (u64)phy_addr +  phy_offset;
} 

void * zv_get_pagetable_log_addr_high(struct zv_ept_info* ept_info_high,u64 index, int type){
    u64* table_array_addr;
    
    switch (type) {
        case EPT_TYPE_PML4: 
            return zv_get_pagetable_log_addr(index,type);
            break;
        
        case EPT_TYPE_PDPTEPD:
            table_array_addr = ept_info_high->pdpte_pd_page_addr_array;
            break;
        
        case EPT_TYPE_PDEPT:
            table_array_addr = ept_info_high->pdept_page_addr_array;
            break;

        case EPT_TYPE_PTE:
            table_array_addr = ept_info_high->pte_page_addr_array;
            break;
        
        default:
            table_array_addr = ept_info_high->pte_page_addr_array;
            break;
    }

    return (void*)table_array_addr[index];
}


void * zv_get_pagetable_phy_addr_high(struct zv_ept_info* ept_info_high,u64 index, int type){

    return (void*)virt_to_phys(zv_get_pagetable_log_addr_high(ept_info_high,index,type));

}

void zv_set_ept_high(struct zv_ept_info* ept_info_high , int type , u64 start_align){
    struct zv_pagetable* pagetable = zv_get_pagetable_log_addr_high(ept_info_high,type,0);
    u64 offset_ept;
    u64 offset_start;
    u64 offset_real;
    u64 ent_count;
    u64 index;
    u64 i;
    switch (type){
        case EPT_TYPE_PML4:
            offset_ept = 39;
            ent_count = ept_info_high->pml4_ent_count;
            break;
        case EPT_TYPE_PDPTEPD:
            offset_ept = 30;
            ent_count = ept_info_high->pdpte_pd_ent_count;
            break;
        case EPT_TYPE_PDEPT:
            offset_ept = 21;
            ent_count = ept_info_high->pdept_ent_count;
            break;
        case EPT_TYPE_PTE:
            offset_ept = 12;
            ent_count = ept_info_high->pte_ent_count;
            break;
    }

    offset_start = (start_align>>offset_ept) & MASK_EPT_OFFSET;

    for(i = 0;i < ent_count ; i++){
        offset_real = (i + offset_start) % 512;
        if(offset_real == 0){
            index = (i + offset_start)/512;
            pagetable = zv_get_pagetable_phy_addr_high(ept_info_high,type,index);
        }
        if(type != EPT_TYPE_PTE){
            pagetable->entry[offset_real] = (u64)zv_get_pagetable_phy_addr_high(ept_info_high,type + 1,i) | EPT_ALL_ACCESS;
        }else{
            pagetable->entry[offset_real] = (start_align + i * 512) | EPT_ALL_ACCESS;
        }
        
    }
}

void zv_add_mem_range(u64 start,u64 end){
    struct zv_ept_info ept_info_high;
    u64 start_align =  start & MASK_PAGEADDR;
    u64 end_align = (end + 0xfff) & MASK_PAGEADDR;
    u64 size = (end_align - start_align) >> 12 ;
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
    /*allocate page*/
    /*
    ept_info_high.pdpte_pd_page_addr_array = (u64*)zv_vmalloc(ept_info_high.pdpte_pd_page_count * EPT_PAGE_SIZE);
    ept_info_high.pdept_page_addr_array    = (u64*)zv_vmalloc(ept_info_high.pdept_page_count * EPT_PAGE_SIZE);
    ept_info_high.pte_page_addr_array      = (u64*)zv_vmalloc(ept_info_high.pte_page_count * EPT_PAGE_SIZE);
    */
    /*setup ept*/
    zv_set_ept_high(&ept_info_high,EPT_TYPE_PML4,start_align);
    zv_set_ept_high(&ept_info_high,EPT_TYPE_PDPTEPD,start_align);
    zv_set_ept_high(&ept_info_high,EPT_TYPE_PDEPT,start_align);
    zv_set_ept_high(&ept_info_high,EPT_TYPE_PTE,start_align);
}