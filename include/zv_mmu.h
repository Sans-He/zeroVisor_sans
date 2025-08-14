#include <linux/types.h>
#include <linux/xarray.h>
#include <linux/resource.h>
/*
 * Macros.
 */
/* Page size macro. */
#define VAL_256TB				((u64)256 * 1024 * 1024 * 1024 * 1024)
#define VAL_512GB				((u64)512 * 1024 * 1024 * 1024)
#define VAL_4GB					((u64)4* 1024 * 1024 * 1024)
#define VAL_1GB					((u64)1024 * 1024 * 1024)
#define VAL_2MB					((u64)2 * 1024 * 1024)
#define VAL_4KB					((u64)4 * 1024)

/* Page table flags. */
#define MASK_PAGEFLAG			((u64) 0xFFF0000000000FFF)
#define MASK_PAGEFLAG_WO_DA		(((u64) 0xFFF0000000000FFF) ^ (0x01 << 5) ^ (0x01 << 6))
#define MASK_INVALIDPAGEFLAG	((u64) 0x07FF000000000000)
#define MASK_PAGE_SIZE_FLAG		(0x01 << 7)
#define MASK_PAGEFLAG_WO_SIZE	(MASK_PAGEFLAG ^ MASK_PAGE_SIZE_FLAG)
#define MASK_PRESENT_FLAG		(0x01 << 0)
#define MASK_XD_FLAG			((u64)0x01 << 63)
#define MASK_PAGEADDR			((u64) 0xFFFFFFFFFFFFF000)
#define MASK_EPT_OFFSET         ((u64) 0x00000000000001FF)

/* EPT page type. */
#define EPT_TYPE_PML4			0
#define EPT_TYPE_PDPTEPD		1
#define EPT_TYPE_PDEPT			2   
#define EPT_TYPE_PTE			3   
#define EPT_TYPE_PHY            4

/* EPT flags */
#define EPT_READ				(0x01 << 0)
#define EPT_WRITE				(0x01 << 1)
#define EPT_EXECUTE				(0x01 << 2)
#define EPT_ALL_ACCESS			(EPT_READ | EPT_WRITE | EPT_EXECUTE)
#define EPT_BIT_PDE_2MB			(0x01 << 7)
#define EPT_BIT_MEM_TYPE_WB		(0x06 << 3)
#define EPT_PAGE_ENT_COUNT		512
#define EPT_PAGE_SIZE			4096

/* Macro for GPA to HPA*/
#define CHANGE_ADDR(x) phys_to_virt(((u64)x)&(~MASK_PAGEFLAG));

/* Structures */

/* EPT information structure */
struct zv_ept_info {
	/* Entry counts for each level */
	u64 pml4_ent_count;
	u64 pdpte_pd_ent_count;
    u64 pdept_ent_count;
    u64 pte_ent_count;

	/* Page counts for each level */
    u64 pml4_page_count;
    u64 pdpte_pd_page_count;
    u64 pdept_page_count;
    u64 pte_page_count;
};

/* Page table structure. */
struct zv_pagetable
{
    u64 entry[512];
};

/* EPT table structure. */
struct zv_ept_pagetable
{
    u64 entry[512];
};


/* Variables */
#ifndef XARRAY
#define XARRAY
    extern struct xarray zv_pml4_table;
    extern struct xarray zv_pdpte_pd_table;
    extern struct xarray zv_pdept_table;
    extern struct xarray zv_pte_table;
#endif // MACRO


/* The function protocol for walk_system_ram_range. */
typedef int (*my_walk_system_ram_range) (unsigned long start_pfn, unsigned long nr_pages, 
	void *arg, int (*func)(unsigned long, unsigned long, void*));
typedef int (*my_walk_iomem_ram_range)  (unsigned long desc, unsigned long flags, u64 start, 
    u64 end, void * arg, int (*func) (struct resource *, void *));


/* Function declarations */
u64 zv_get_max_ram_size(void);
int zv_alloc_ept_pages(void);
void zv_setup_ept_pagetables(void);
void* zv_get_pagetable_log_addr(unsigned long type, int index);
void* zv_get_pagetable_phy_addr(unsigned long type, int index);
void zv_set_ept_hide_page(u64 phy_addr);
void zv_set_ept_lock_page(u64 phy_addr);
void zv_set_ept_all_access_page(u64 phy_addr);
void zv_protect_ept_pages(void);

/* Change GPA to HPA with EPT*/
void* check_addr_page(u64 x,int type,u64 page_addr,u64 pre_page_addr,u64 offset);

/* Function to add mem range to ept*/
void zv_add_ept_map_range(u64 start, u64 end);

u64 guest_to_host(u64 x);