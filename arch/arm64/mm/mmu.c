/*
 * Based on arch/arm/mm/mmu.c
 *
 * Copyright (C) 1995-2005 Russell King
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/libfdt.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/memblock.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/stop_machine.h>
#include <linux/mm.h>

#include <asm/barrier.h>
#include <asm/cputype.h>
#include <asm/fixmap.h>
#include <asm/kasan.h>
#include <asm/kernel-pgtable.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/sizes.h>
#include <asm/tlb.h>
#include <asm/memblock.h>
#include <asm/mmu_context.h>
#include <asm/map.h>

#include "mm.h"

#include <linux/vmalloc.h>

#ifdef CONFIG_UH
#include <linux/uh.h>
#endif
#ifdef CONFIG_UH_RKP
#include <linux/rkp.h>
#endif

u64 idmap_t0sz = TCR_T0SZ(VA_BITS);
static int iotable_on;

u64 kimage_voffset __read_mostly;
EXPORT_SYMBOL(kimage_voffset);

#ifdef CONFIG_KNOX_KAP
extern int boot_mode_security;
#endif
/*
 * Empty_zero_page is a special page that is used for zero-initialized data
 * and COW.
 */
unsigned long empty_zero_page[PAGE_SIZE / sizeof(unsigned long)] __page_aligned_rkp_bss;
EXPORT_SYMBOL(empty_zero_page);

static pte_t bm_pte[PTRS_PER_PTE] __page_aligned_rkp_bss;
static pmd_t bm_pmd[PTRS_PER_PMD] __page_aligned_rkp_bss __maybe_unused;
static pud_t bm_pud[PTRS_PER_PUD] __page_aligned_rkp_bss __maybe_unused;

pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
			      unsigned long size, pgprot_t vma_prot)
{
	if (!pfn_valid(pfn))
		return pgprot_noncached(vma_prot);
	else if (file->f_flags & O_SYNC)
		return pgprot_writecombine(vma_prot);
	return vma_prot;
}
EXPORT_SYMBOL(phys_mem_access_prot);

static phys_addr_t __init early_pgtable_alloc(void)
{
	phys_addr_t phys;
	void *ptr;

	phys = memblock_alloc(PAGE_SIZE, PAGE_SIZE);
	BUG_ON(!phys);

	/*
	 * The FIX_{PGD,PUD,PMD} slots may be in active use, but the FIX_PTE
	 * slot will be free, so we can (ab)use the FIX_PTE slot to initialise
	 * any level of table.
	 */
	ptr = pte_set_fixmap(phys);

	memset(ptr, 0, PAGE_SIZE);

	/*
	 * Implicit barriers also ensure the zeroed page is visible to the page
	 * table walker
	 */
	pte_clear_fixmap();

	return phys;
}

/*
 * remap a PMD into pages
 */
static void split_pmd(pmd_t *pmd, pte_t *pte)
{
	unsigned long pfn = pmd_pfn(*pmd);
	int i = 0;

	do {
		/*
		 * Need to have the least restrictive permissions available
		 * permissions will be fixed up later
		 */
		set_pte(pte, pfn_pte(pfn, PAGE_KERNEL_EXEC));
		pfn++;
	} while (pte++, i++, i < PTRS_PER_PTE);
}

#ifdef CONFIG_UH_RKP
static phys_addr_t rkp_ro_alloc_phys(void)
{
	phys_addr_t ret = 0;
	ret = uh_call(UH_APP_RKP, RKP_RKP_ROBUFFER_ALLOC, 0, 0, 0, 0);
	return ret;
}
#endif

static phys_addr_t late_pgtable_alloc(void)
{
	void *ptr = (void *)__get_free_page(PGALLOC_GFP);
	BUG_ON(!ptr);

	/* Ensure the zeroed page is visible to the page table walker */
	dsb(ishst);
	return __pa(ptr);
}

static void alloc_init_pte(pmd_t *pmd, unsigned long addr,
				  unsigned long end, unsigned long pfn,
				  pgprot_t prot,
				  phys_addr_t (*pgtable_alloc)(void))
{
	pte_t *pte;

	if (pmd_none(*pmd) || pmd_sect(*pmd)) {
		phys_addr_t pte_phys = 0;
		BUG_ON(!pgtable_alloc);
		pte_phys = pgtable_alloc();
		pte = pte_set_fixmap(pte_phys);
		if (pmd_sect(*pmd)) {
			split_pmd(pmd, pte);
		}
		__pmd_populate(pmd, pte_phys, PMD_TYPE_TABLE);
		flush_tlb_all();
		pte_clear_fixmap();
	}
	BUG_ON(pmd_bad(*pmd));

	pte = pte_set_fixmap_offset(pmd, addr);
	do {
		if (iotable_on == 1)
			set_pte(pte, pfn_pte(pfn, pgprot_iotable_init(PAGE_KERNEL_EXEC)));
		else
			set_pte(pte, pfn_pte(pfn, prot));
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);

	pte_clear_fixmap();
}

static void split_pud(pud_t *old_pud, pmd_t *pmd)
{
	unsigned long addr = pud_pfn(*old_pud) << PAGE_SHIFT;
	pgprot_t prot = __pgprot(pud_val(*old_pud) ^ addr);
	int i = 0;

	do {
		set_pmd(pmd, __pmd(addr | pgprot_val(prot)));
		addr += PMD_SIZE;
	} while (pmd++, i++, i < PTRS_PER_PMD);
}

#ifdef CONFIG_DEBUG_PAGEALLOC
static bool block_mappings_allowed(phys_addr_t (*pgtable_alloc)(void))
{

	/*
	 * If debug_page_alloc is enabled we must map the linear map
	 * using pages. However, other mappings created by
	 * create_mapping_noalloc must use sections in some cases. Allow
	 * sections to be used in those cases, where no pgtable_alloc
	 * function is provided.
	 */
	return !pgtable_alloc || !debug_pagealloc_enabled();
}
#else
static bool block_mappings_allowed(phys_addr_t (*pgtable_alloc)(void))
{
	return true;
}
#endif

static void alloc_init_pmd(pud_t *pud, unsigned long addr, unsigned long end,
				  phys_addr_t phys, pgprot_t prot,
				  phys_addr_t (*pgtable_alloc)(void))
{
	pmd_t *pmd;
	unsigned long next;
#ifdef CONFIG_UH_RKP
	int rkp_do = 0;
#ifdef CONFIG_KNOX_KAP
		if (boot_mode_security)
#endif
			rkp_do = 1;
#endif

	/*
	 * Check for initial section mappings in the pgd/pud and remove them.
	 */
	if (pud_none(*pud) || pud_sect(*pud)) {
		phys_addr_t pmd_phys = 0;
		BUG_ON(!pgtable_alloc);
#ifdef CONFIG_UH_RKP
		if (rkp_do){
			pmd_phys = rkp_ro_alloc_phys();
			if (!pmd_phys)
				pmd_phys = pgtable_alloc();
		} else {
			pmd_phys = pgtable_alloc();
		}
#else	/* !CONFIG_UH_RKP */
		pmd_phys = pgtable_alloc();
#endif
		pmd = pmd_set_fixmap(pmd_phys);
		if (pud_sect(*pud)) {
			/*
			 * need to have the 1G of mappings continue to be
			 * present
			 */
			split_pud(pud, pmd);
		}
		__pud_populate(pud, pmd_phys, PUD_TYPE_TABLE);
		flush_tlb_all();
		pmd_clear_fixmap();
	}
	BUG_ON(pud_bad(*pud));

	pmd = pmd_set_fixmap_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		/* try section mapping first */
		if (((addr | next | phys) & ~SECTION_MASK) == 0 &&
		      block_mappings_allowed(pgtable_alloc)) {
			pmd_t old_pmd =*pmd;
			pmd_set_huge(pmd, phys, prot);
			/*
			 * Check for previous table entries created during
			 * boot (__create_page_tables) and flush them.
			 */
			if (!pmd_none(old_pmd)) {
				flush_tlb_all();
				if (pmd_table(old_pmd)) {
					phys_addr_t table = pmd_page_paddr(old_pmd);
					if (!WARN_ON_ONCE(slab_is_available()))
						memblock_free(table, PAGE_SIZE);
				}
			}
		} else {
			alloc_init_pte(pmd, addr, next, __phys_to_pfn(phys),
				       prot, pgtable_alloc);
		}
		phys += next - addr;
	} while (pmd++, addr = next, addr != end);

	pmd_clear_fixmap();
}

static inline bool use_1G_block(unsigned long addr, unsigned long next,
			unsigned long phys)
{
	if (PAGE_SHIFT != 12)
		return false;

	if (((addr | next | phys) & ~PUD_MASK) != 0)
		return false;

#ifdef CONFIG_UH_RKP
	return false;
#else
	return true;
#endif
}

static void alloc_init_pud(pgd_t *pgd, unsigned long addr, unsigned long end,
				  phys_addr_t phys, pgprot_t prot,
				  phys_addr_t (*pgtable_alloc)(void))
{
	pud_t *pud;
	unsigned long next;

	if (pgd_none(*pgd)) {
		phys_addr_t pud_phys;
		BUG_ON(!pgtable_alloc);
		pud_phys = pgtable_alloc();
		__pgd_populate(pgd, pud_phys, PUD_TYPE_TABLE);
	}
	BUG_ON(pgd_bad(*pgd));

	pud = pud_set_fixmap_offset(pgd, addr);
	do {
		next = pud_addr_end(addr, end);

		/*
		 * For 4K granule only, attempt to put down a 1GB block
		 */
		if (use_1G_block(addr, next, phys) &&
		    block_mappings_allowed(pgtable_alloc)) {
			pud_t old_pud = *pud;
			pud_set_huge(pud, phys, prot);

			/*
			 * If we have an old value for a pud, it will
			 * be pointing to a pmd table that we no longer
			 * need (from swapper_pg_dir).
			 *
			 * Look up the old pmd table and free it.
			 */
			if (!pud_none(old_pud)) {
				flush_tlb_all();
				if (pud_table(old_pud)) {
					phys_addr_t table = pud_page_paddr(old_pud);
					if (!WARN_ON_ONCE(slab_is_available())) {
						memblock_free(table, PAGE_SIZE);
					}
				}
			}
		} else {
			alloc_init_pmd(pud, addr, next, phys, prot,
				       pgtable_alloc);
		}
		phys += next - addr;
	} while (pud++, addr = next, addr != end);

	pud_clear_fixmap();
}

/*
 * Create the page directory entries and any necessary page tables for the
 * mapping specified by 'md'.
 */
static void init_pgd(pgd_t *pgd, phys_addr_t phys, unsigned long virt,
				    phys_addr_t size, pgprot_t prot,
				    phys_addr_t (*pgtable_alloc)(void))
{
	unsigned long addr, length, end, next;

	/*
	 * If the virtual and physical address don't have the same offset
	 * within a page, we cannot map the region as the caller expects.
	 */
	if (WARN_ON((phys ^ virt) & ~PAGE_MASK))
		return;

	phys &= PAGE_MASK;
	addr = virt & PAGE_MASK;
	length = PAGE_ALIGN(size + (virt & ~PAGE_MASK));

	end = addr + length;
	do {
		next = pgd_addr_end(addr, end);
		alloc_init_pud(pgd, addr, next, phys, prot, pgtable_alloc);
		phys += next - addr;
	} while (pgd++, addr = next, addr != end);
}

static void __create_pgd_mapping(pgd_t *pgdir, phys_addr_t phys,
				 unsigned long virt, phys_addr_t size,
				 pgprot_t prot,
				 phys_addr_t (*alloc)(void))
{
	init_pgd(pgd_offset_raw(pgdir, virt), phys, virt, size, prot, alloc);
}

/*
 * This function can only be used to modify existing table entries,
 * without allocating new levels of table. Note that this permits the
 * creation of new section or page entries.
 */
static void __init create_mapping_noalloc(phys_addr_t phys, unsigned long virt,
				  phys_addr_t size, pgprot_t prot)
{
	if (virt < VMALLOC_START) {
		pr_warn("BUG: not creating mapping for %pa at 0x%016lx - outside kernel range\n",
			&phys, virt);
		return;
	}
	__create_pgd_mapping(init_mm.pgd, phys, virt, size, prot,
			     NULL);
}

void __init create_pgd_mapping(struct mm_struct *mm, phys_addr_t phys,
			       unsigned long virt, phys_addr_t size,
			       pgprot_t prot)
{
	__create_pgd_mapping(mm->pgd, phys, virt, size, prot,
			     late_pgtable_alloc);
}

static void create_mapping_late(phys_addr_t phys, unsigned long virt,
				  phys_addr_t size, pgprot_t prot)
{
	if (virt < VMALLOC_START) {
		pr_warn("BUG: not creating mapping for %pa at 0x%016lx - outside kernel range\n",
			&phys, virt);
		return;
	}

	__create_pgd_mapping(init_mm.pgd, phys, virt, size, prot,
			     late_pgtable_alloc);
}

static void __init __map_memblock(pgd_t *pgd, phys_addr_t start, phys_addr_t end)
{
	unsigned long kernel_start = __pa_symbol(_stext);
	unsigned long kernel_end = __pa_symbol(__init_begin);

	/*
	 * Take care not to create a writable alias for the
	 * read-only text and rodata sections of the kernel image.
	 */

	/* No overlap with the kernel text/rodata */
	if (end < kernel_start || start >= kernel_end) {
		__create_pgd_mapping(pgd, start, __phys_to_virt(start),
				     end - start, PAGE_KERNEL,
				     early_pgtable_alloc);
		return;
	}

	/*
	 * This block overlaps the kernel text/rodata mapping.
	 * Map the portion(s) which don't overlap.
	 */
	if (start < kernel_start)
		__create_pgd_mapping(pgd, start,
				     __phys_to_virt(start),
				     kernel_start - start, PAGE_KERNEL,
				     early_pgtable_alloc);
	if (kernel_end < end)
		__create_pgd_mapping(pgd, kernel_end,
				     __phys_to_virt(kernel_end),
				     end - kernel_end, PAGE_KERNEL,
				     early_pgtable_alloc);

	/*
	 * Map the linear alias of the [_stext, __init_begin) interval as
	 * read-only/non-executable. This makes the contents of the
	 * region accessible to subsystems such as hibernate, but
	 * protects it from inadvertent modification or execution.
	 */
	__create_pgd_mapping(pgd, kernel_start, __phys_to_virt(kernel_start),
			     kernel_end - kernel_start, PAGE_KERNEL_RO,
			     early_pgtable_alloc);
}

static void __init map_mem(pgd_t *pgd)
{
	struct memblock_region *reg;

	/* map all the memory banks */
	for_each_memblock(memory, reg) {
		phys_addr_t start = reg->base;
		phys_addr_t end = start + reg->size;

		if (start >= end)
			break;
		if (memblock_is_nomap(reg))
			continue;

		__map_memblock(pgd, start, end);
	}
}

void mark_rodata_ro(void)
{
	unsigned long section_size;


	section_size = (unsigned long)_etext - (unsigned long)_stext;
	create_mapping_late(__pa_symbol(_stext), (unsigned long)_stext,
			    section_size, PAGE_KERNEL_ROX);
	/*
	 * mark .rodata as read only. Use __init_begin rather than __end_rodata
	 * to cover NOTES and EXCEPTION_TABLE.
	 */
	section_size = (unsigned long)__init_begin - (unsigned long)__start_rodata;
	create_mapping_late(__pa_symbol(__start_rodata),
			    (unsigned long)__start_rodata,
			    section_size, PAGE_KERNEL_RO);
}

void fixup_init(void)
{
	/*
	 * Unmap the __init region but leave the VM area in place. This
	 * prevents the region from being reused for kernel modules, which
	 * is not supported by kallsyms.
	 */
	unmap_kernel_range((u64)__init_begin, (u64)(__init_end - __init_begin));
}
#ifdef CONFIG_UH
void *__init _uh_map(phys_addr_t uh_phys)
{
	const u64 uh_virt = __fix_to_virt(FIX_UH);
	phys_addr_t uh_base = round_down(uh_phys, SWAPPER_BLOCK_SIZE);
	BUILD_BUG_ON(uh_virt % SZ_2M);

	/* map the first chunk so we can read the size from the header */
	create_mapping_noalloc(uh_base, uh_virt, SWAPPER_BLOCK_SIZE, PAGE_KERNEL);
	return (void *)(uh_virt + (u64)uh_phys - (u64)uh_base);
}
#endif
#ifdef CONFIG_UH_RKP
static void __init map_kernel_text_chunk(pgd_t *pgd, void *va_start, void *va_end,
				    pgprot_t prot, struct vm_struct *vma)
{
	phys_addr_t pa_start = __pa_symbol(va_start);
	unsigned long size = va_end - va_start;

	BUG_ON(!PAGE_ALIGNED(pa_start));
	BUG_ON(!PAGE_ALIGNED(size));

	__create_pgd_mapping(pgd, pa_start, (unsigned long)va_start, size, prot,
			     rkp_ro_alloc_phys);

	vma->addr	= (void *)((unsigned long)va_start & PMD_MASK);
	vma->phys_addr	= (phys_addr_t)((unsigned long)pa_start & PMD_MASK);
	vma->size	= size + (unsigned long)va_start - (unsigned long)vma->addr;
	vma->flags	= VM_MAP;
	vma->caller	= __builtin_return_address(0);


	vm_area_add_early(vma);
}
#endif
static void __init map_kernel_chunk(pgd_t *pgd, void *va_start, void *va_end,
				    pgprot_t prot, struct vm_struct *vma)
{
	phys_addr_t pa_start = __pa_symbol(va_start);
	unsigned long size = va_end - va_start;

	BUG_ON(!PAGE_ALIGNED(pa_start));
	BUG_ON(!PAGE_ALIGNED(size));

	__create_pgd_mapping(pgd, pa_start, (unsigned long)va_start, size, prot,
			     early_pgtable_alloc);

	vma->addr	= va_start;
	vma->phys_addr	= pa_start;
	vma->size	= size;
	vma->flags	= VM_MAP;
	vma->caller	= __builtin_return_address(0);

	vm_area_add_early(vma);
}

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
static int __init map_entry_trampoline(void)
{
	extern char __entry_tramp_text_start[];

	pgprot_t prot = PAGE_KERNEL_EXEC;
	phys_addr_t pa_start = __pa_symbol(__entry_tramp_text_start);

	/* The trampoline is always mapped and can therefore be global */
	pgprot_val(prot) &= ~PTE_NG;

	/* Map only the text into the trampoline page table */
	memset(tramp_pg_dir, 0, PGD_SIZE);
#ifdef CONFIG_UH_RKP
	__create_pgd_mapping(tramp_pg_dir, pa_start, TRAMP_VALIAS, PAGE_SIZE,
			     prot, rkp_ro_alloc_phys);
#else
	__create_pgd_mapping(tramp_pg_dir, pa_start, TRAMP_VALIAS, PAGE_SIZE,
			     prot, late_pgtable_alloc);
#endif

	/* Map both the text and data into the kernel page table */
	__set_fixmap(FIX_ENTRY_TRAMP_TEXT, pa_start, prot);
	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
		extern char __entry_tramp_data_start[];

		__set_fixmap(FIX_ENTRY_TRAMP_DATA,
			     __pa_symbol(__entry_tramp_data_start),
			     PAGE_KERNEL_RO);
	}

	return 0;
}
core_initcall(map_entry_trampoline);
#endif

/*
 * Create fine-grained mappings for the kernel.
 */
static void __init map_kernel(pgd_t *pgd)
{
	static struct vm_struct vmlinux_text, vmlinux_rodata, vmlinux_init, vmlinux_data;

#ifdef CONFIG_UH_RKP
#ifdef CONFIG_KNOX_KAP
	if (boot_mode_security)
#endif
	map_kernel_text_chunk(pgd, _text, _etext, PAGE_KERNEL_EXEC, &vmlinux_text);
#ifdef CONFIG_KNOX_KAP
	else
#else
	if(0)
#endif
#endif
	map_kernel_chunk(pgd, _stext, _etext, PAGE_KERNEL_EXEC, &vmlinux_text);
	map_kernel_chunk(pgd, __start_rodata, __init_begin, PAGE_KERNEL, &vmlinux_rodata);
	map_kernel_chunk(pgd, __init_begin, __init_end, PAGE_KERNEL_EXEC,
			 &vmlinux_init);
	map_kernel_chunk(pgd, _data, _end, PAGE_KERNEL, &vmlinux_data);

	if (!pgd_val(*pgd_offset_raw(pgd, FIXADDR_START))) {
		/*
		 * The fixmap falls in a separate pgd to the kernel, and doesn't
		 * live in the carveout for the swapper_pg_dir. We can simply
		 * re-use the existing dir for the fixmap.
		 */
		set_pgd(pgd_offset_raw(pgd, FIXADDR_START),
			*pgd_offset_k(FIXADDR_START));
	} else if (CONFIG_PGTABLE_LEVELS > 3) {
		/*
		 * The fixmap shares its top level pgd entry with the kernel
		 * mapping. This can really only occur when we are running
		 * with 16k/4 levels, so we can simply reuse the pud level
		 * entry instead.
		 */
		BUG_ON(!IS_ENABLED(CONFIG_ARM64_16K_PAGES));
		set_pud(pud_set_fixmap_offset(pgd, FIXADDR_START),
			__pud(__pa_symbol(bm_pmd) | PUD_TYPE_TABLE));
		pud_clear_fixmap();
	} else {
		BUG();
	}

	kasan_copy_shadow(pgd);
}

/*
 * paging_init() sets up the page tables, initialises the zone memory
 * maps and sets up the zero page.
 */
void __init paging_init(void)
{
	phys_addr_t pgd_phys = early_pgtable_alloc();
	pgd_t *pgd = pgd_set_fixmap(pgd_phys);

	map_kernel(pgd);
	map_mem(pgd);

	/*
	 * We want to reuse the original swapper_pg_dir so we don't have to
	 * communicate the new address to non-coherent secondaries in
	 * secondary_entry, and so cpu_switch_mm can generate the address with
	 * adrp+add rather than a load from some global variable.
	 *
	 * To do this we need to go via a temporary pgd.
	 */
	cpu_replace_ttbr1(__va(pgd_phys));
	memcpy(swapper_pg_dir, pgd, PAGE_SIZE);
	cpu_replace_ttbr1(lm_alias(swapper_pg_dir));

	pgd_clear_fixmap();
	memblock_free(pgd_phys, PAGE_SIZE);

	/* Ensure the zero page is visible to the page table walker */
	dsb(ishst);

	/*
	 * We only reuse the PGD from the swapper_pg_dir, not the pud + pmd
	 * allocated with it.
	 */
#ifndef CONFIG_UH_RKP
	memblock_free(__pa_symbol(swapper_pg_dir) + PAGE_SIZE,
		      SWAPPER_DIR_SIZE - PAGE_SIZE);
#endif

	bootmem_init();
	set_memsize_kernel_type(MEMSIZE_KERNEL_OTHERS);
}

/*
 * Check whether a kernel address is valid (derived from arch/x86/).
 */
int kern_addr_valid(unsigned long addr)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	if ((((long)addr) >> VA_BITS) != -1UL)
		return 0;

	pgd = pgd_offset_k(addr);
	if (pgd_none(*pgd))
		return 0;

	pud = pud_offset(pgd, addr);
	if (pud_none(*pud))
		return 0;

	if (pud_sect(*pud))
		return pfn_valid(pud_pfn(*pud));

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return 0;

	if (pmd_sect(*pmd))
		return pfn_valid(pmd_pfn(*pmd));

	pte = pte_offset_kernel(pmd, addr);
	if (pte_none(*pte))
		return 0;

	return pfn_valid(pte_pfn(*pte));
}
#ifdef CONFIG_SPARSEMEM_VMEMMAP
#if !ARM64_SWAPPER_USES_SECTION_MAPS
int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node)
{
	return vmemmap_populate_basepages(start, end, node);
}
#else	/* !ARM64_SWAPPER_USES_SECTION_MAPS */
int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node)
{
	unsigned long addr = start;
	unsigned long next;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	do {
		next = pmd_addr_end(addr, end);

		pgd = vmemmap_pgd_populate(addr, node);
		if (!pgd)
			return -ENOMEM;

		pud = vmemmap_pud_populate(pgd, addr, node);
		if (!pud)
			return -ENOMEM;

		pmd = pmd_offset(pud, addr);
		if (pmd_none(*pmd)) {
			void *p = NULL;

			p = vmemmap_alloc_block_buf(PMD_SIZE, node);
			if (!p)
				return -ENOMEM;

			set_pmd(pmd, __pmd(__pa(p) | PROT_SECT_NORMAL));
		} else
			vmemmap_verify((pte_t *)pmd, node, addr, next);
	} while (addr = next, addr != end);

	return 0;
}
#endif	/* CONFIG_ARM64_64K_PAGES */
void vmemmap_free(unsigned long start, unsigned long end)
{
}
#endif	/* CONFIG_SPARSEMEM_VMEMMAP */

static inline pud_t * fixmap_pud(unsigned long addr)
{
	pgd_t *pgd = pgd_offset_k(addr);

	BUG_ON(pgd_none(*pgd) || pgd_bad(*pgd));

	return pud_offset_kimg(pgd, addr);
}

static inline pmd_t * fixmap_pmd(unsigned long addr)
{
	pud_t *pud = fixmap_pud(addr);

	BUG_ON(pud_none(*pud) || pud_bad(*pud));

	return pmd_offset_kimg(pud, addr);
}

static inline pte_t * fixmap_pte(unsigned long addr)
{
	return &bm_pte[pte_index(addr)];
}

/*
 * The p*d_populate functions call virt_to_phys implicitly so they can't be used
 * directly on kernel symbols (bm_p*d). This function is called too early to use
 * lm_alias so __p*d_populate functions must be used to populate with the
 * physical address from __pa_symbol.
 */
void __init early_fixmap_init(void)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	unsigned long addr = FIXADDR_START;

	pgd = pgd_offset_k(addr);
	if (CONFIG_PGTABLE_LEVELS > 3 &&
	    !(pgd_none(*pgd) || pgd_page_paddr(*pgd) == __pa_symbol(bm_pud))) {
		/*
		 * We only end up here if the kernel mapping and the fixmap
		 * share the top level pgd entry, which should only happen on
		 * 16k/4 levels configurations.
		 */
		BUG_ON(!IS_ENABLED(CONFIG_ARM64_16K_PAGES));
		pud = pud_offset_kimg(pgd, addr);
	} else {
		if (pgd_none(*pgd))
			__pgd_populate(pgd, __pa_symbol(bm_pud),
				       PUD_TYPE_TABLE);
		pud = fixmap_pud(addr);
	}
	if (pud_none(*pud))
		__pud_populate(pud, __pa_symbol(bm_pmd), PMD_TYPE_TABLE);
	pmd = fixmap_pmd(addr);
	__pmd_populate(pmd, __pa_symbol(bm_pte), PMD_TYPE_TABLE);

	/*
	 * The boot-ioremap range spans multiple pmds, for which
	 * we are not prepared:
	 */
	BUILD_BUG_ON((__fix_to_virt(FIX_BTMAP_BEGIN) >> PMD_SHIFT)
		     != (__fix_to_virt(FIX_BTMAP_END) >> PMD_SHIFT));

	if ((pmd != fixmap_pmd(fix_to_virt(FIX_BTMAP_BEGIN)))
	     || pmd != fixmap_pmd(fix_to_virt(FIX_BTMAP_END))) {
		WARN_ON(1);
		pr_warn("pmd %p != %p, %p\n",
			pmd, fixmap_pmd(fix_to_virt(FIX_BTMAP_BEGIN)),
			fixmap_pmd(fix_to_virt(FIX_BTMAP_END)));
		pr_warn("fix_to_virt(FIX_BTMAP_BEGIN): %08lx\n",
			fix_to_virt(FIX_BTMAP_BEGIN));
		pr_warn("fix_to_virt(FIX_BTMAP_END):   %08lx\n",
			fix_to_virt(FIX_BTMAP_END));

		pr_warn("FIX_BTMAP_END:       %d\n", FIX_BTMAP_END);
		pr_warn("FIX_BTMAP_BEGIN:     %d\n", FIX_BTMAP_BEGIN);
	}
}

void __set_fixmap(enum fixed_addresses idx,
			       phys_addr_t phys, pgprot_t flags)
{
	unsigned long addr = __fix_to_virt(idx);
	pte_t *pte;

	BUG_ON(idx <= FIX_HOLE || idx >= __end_of_fixed_addresses);

	pte = fixmap_pte(addr);

	if (pgprot_val(flags)) {
		set_pte(pte, pfn_pte(phys >> PAGE_SHIFT, flags));
	} else {
		pte_clear(&init_mm, addr, pte);
		flush_tlb_kernel_range(addr, addr+PAGE_SIZE);
	}
}

void *__init __fixmap_remap_fdt(phys_addr_t dt_phys, int *size, pgprot_t prot)
{
	const u64 dt_virt_base = __fix_to_virt(FIX_FDT);
	int offset;
	void *dt_virt;

	/*
	 * Check whether the physical FDT address is set and meets the minimum
	 * alignment requirement. Since we are relying on MIN_FDT_ALIGN to be
	 * at least 8 bytes so that we can always access the magic and size
	 * fields of the FDT header after mapping the first chunk, double check
	 * here if that is indeed the case.
	 */
	BUILD_BUG_ON(MIN_FDT_ALIGN < 8);
	if (!dt_phys || dt_phys % MIN_FDT_ALIGN)
		return NULL;

	/*
	 * Make sure that the FDT region can be mapped without the need to
	 * allocate additional translation table pages, so that it is safe
	 * to call create_mapping_noalloc() this early.
	 *
	 * On 64k pages, the FDT will be mapped using PTEs, so we need to
	 * be in the same PMD as the rest of the fixmap.
	 * On 4k pages, we'll use section mappings for the FDT so we only
	 * have to be in the same PUD.
	 */
	BUILD_BUG_ON(dt_virt_base % SZ_2M);

	BUILD_BUG_ON(__fix_to_virt(FIX_FDT_END) >> SWAPPER_TABLE_SHIFT !=
		     __fix_to_virt(FIX_BTMAP_BEGIN) >> SWAPPER_TABLE_SHIFT);

	offset = dt_phys % SWAPPER_BLOCK_SIZE;
	dt_virt = (void *)dt_virt_base + offset;

	/* map the first chunk so we can read the size from the header */
	create_mapping_noalloc(round_down(dt_phys, SWAPPER_BLOCK_SIZE),
			dt_virt_base, SWAPPER_BLOCK_SIZE, prot);

	if (fdt_magic(dt_virt) != FDT_MAGIC)
		return NULL;

	*size = fdt_totalsize(dt_virt);
	if (*size > MAX_FDT_SIZE)
		return NULL;

	if (offset + *size > SWAPPER_BLOCK_SIZE)
		create_mapping_noalloc(round_down(dt_phys, SWAPPER_BLOCK_SIZE), dt_virt_base,
			       round_up(offset + *size, SWAPPER_BLOCK_SIZE), prot);

	return dt_virt;
}

void *__init fixmap_remap_fdt(phys_addr_t dt_phys)
{
	void *dt_virt;
	int size;

	dt_virt = __fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL_RO);
	if (!dt_virt)
		return NULL;

	memblock_reserve(dt_phys, size);
	return dt_virt;
}

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP
int pud_free_pmd_page(pud_t *pud, unsigned long addr)
{
	return pud_none(*pud);
}

int pmd_free_pte_page(pmd_t *pmd, unsigned long addr)
{
	return pmd_none(*pmd);
}
#endif
