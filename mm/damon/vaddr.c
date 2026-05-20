// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON Code for Virtual Address Spaces
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#define pr_fmt(fmt) "damon-va: " fmt

#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mmu_notifier.h>
#include <linux/page_idle.h>
#include <linux/sched/mm.h>

#include "ops-common.h"

/*
 * [4.19 backport] This whole file is rewritten against this kernel's old
 * struct mm_walk (plain function pointers + mm/vma/private embedded
 * directly in the walk struct itself, no separate mm_walk_ops table, no
 * .walk_lock field, no walk->action/ACTION_AGAIN retry protocol - that
 * retry mechanism exists in mainline to handle page tables being freed
 * under RCU, which this kernel doesn't do; this kernel's
 * pte_offset_map_lock() macro always succeeds).
 *
 * struct folio doesn't exist here either, so every folio_* call is
 * replaced with the equivalent struct page API, matching the same
 * approach used in ops-common.c (which itself follows this kernel's own
 * mm/page_idle.c).
 *
 * Hugetlb handling (damon_hugetlb_mkold / damon_mkold_hugetlb_entry /
 * damon_young_hugetlb_entry) is dropped entirely rather than ported: this
 * kernel's X00TD_defconfig has CONFIG_HUGETLBFS unset, and
 * include/linux/hugetlb.h wasn't available to verify whether
 * huge_ptep_get()/huge_pte_lock()'s signatures here match mainline's (they
 * differ across kernel versions). If hugetlb is ever needed on this
 * device, this needs revisiting with that header in hand.
 *
 * damos_madvise()/damon_va_apply_scheme() call do_madvise(mm, ...) with an
 * explicit mm_struct - a mainline addition from ~5.19, added specifically
 * for DAMON's benefit. Whether this 4.19 tree has that backported is
 * UNVERIFIED (mm/madvise.c and linux/mman.h weren't in the provided deps).
 * MADV_COLD and MADV_PAGEOUT specifically are themselves mainline 5.4+
 * additions, so even if do_madvise() exists, whether these two actions are
 * functionally implemented (actual proactive reclaim) rather than just
 * accepted-and-ignored is also unverified. See the comment at
 * damos_madvise() below before relying on DAMOS_COLD/DAMOS_PAGEOUT.
 */

#ifdef CONFIG_DAMON_VADDR_KUNIT_TEST
#undef DAMON_MIN_REGION
#define DAMON_MIN_REGION 1
#endif

/*
 * 't->pid' should be the pointer to the relevant 'struct pid' having reference
 * count.  Caller must put the returned task, unless it is NULL.
 */
static inline struct task_struct *damon_get_task_struct(struct damon_target *t)
{
	return get_pid_task(t->pid, PIDTYPE_PID);
}

/*
 * Get the mm_struct of the given target
 *
 * Caller _must_ put the mm_struct after use, unless it is NULL.
 *
 * Returns the mm_struct of the target on success, NULL on failure
 */
static struct mm_struct *damon_get_mm(struct damon_target *t)
{
	struct task_struct *task;
	struct mm_struct *mm;

	task = damon_get_task_struct(t);
	if (!task)
		return NULL;

	mm = get_task_mm(task);
	put_task_struct(task);
	return mm;
}

/*
 * Functions for the initial monitoring target regions construction
 */

/*
 * Size-evenly split a region into 'nr_pieces' small regions
 *
 * Returns 0 on success, or negative error code otherwise.
 */
static int damon_va_evenly_split_region(struct damon_target *t,
		struct damon_region *r, unsigned int nr_pieces)
{
	unsigned long sz_orig, sz_piece, orig_end;
	struct damon_region *n = NULL, *next;
	unsigned long start;
	unsigned int i;

	if (!r || !nr_pieces)
		return -EINVAL;

	if (nr_pieces == 1)
		return 0;

	orig_end = r->ar.end;
	sz_orig = damon_sz_region(r);
	sz_piece = ALIGN_DOWN(sz_orig / nr_pieces, DAMON_MIN_REGION);

	if (!sz_piece)
		return -EINVAL;

	r->ar.end = r->ar.start + sz_piece;
	next = damon_next_region(r);
	for (start = r->ar.end, i = 1; i < nr_pieces; start += sz_piece, i++) {
		n = damon_new_region(start, start + sz_piece);
		if (!n)
			return -ENOMEM;
		damon_insert_region(n, r, next, t);
		r = n;
	}
	/* complement last region for possible rounding error */
	if (n)
		n->ar.end = orig_end;

	return 0;
}

static unsigned long sz_range(struct damon_addr_range *r)
{
	return r->end - r->start;
}

/*
 * Find three regions separated by two biggest unmapped regions
 *
 * vma		the head vma of the target address space
 * regions	an array of three address ranges that results will be saved
 *
 * This function receives an address space and finds three regions in it which
 * separated by the two biggest unmapped regions in the space.  Please refer to
 * below comments of '__damon_va_init_regions()' function to know why this is
 * necessary.
 *
 * Returns 0 if success, or negative error code otherwise.
 */
static int __damon_va_three_regions(struct mm_struct *mm,
				       struct damon_addr_range regions[3])
{
	struct damon_addr_range first_gap = {0}, second_gap = {0};
	struct vm_area_struct *vma, *prev = NULL;
	unsigned long start;

	/*
	 * [4.19 backport] Mainline walks VMAs via the maple-tree based
	 * VMA_ITERATOR/for_each_vma under rcu_read_lock(). This kernel
	 * stores VMAs as an rbtree + a 'vm_next' singly-linked list
	 * (mm->mmap is the head), which is what mainline itself used
	 * before the maple tree conversion. The caller
	 * (damon_va_three_regions()) already holds mmap_read_lock(mm) for
	 * the duration of this call, so a plain list walk is safe here
	 * without any extra rcu_read_lock().
	 *
	 * Find the two biggest gaps so that first_gap > second_gap > others.
	 */
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		unsigned long gap;

		if (!prev) {
			start = vma->vm_start;
			goto next;
		}
		gap = vma->vm_start - prev->vm_end;

		if (gap > sz_range(&first_gap)) {
			second_gap = first_gap;
			first_gap.start = prev->vm_end;
			first_gap.end = vma->vm_start;
		} else if (gap > sz_range(&second_gap)) {
			second_gap.start = prev->vm_end;
			second_gap.end = vma->vm_start;
		}
next:
		prev = vma;
	}

	if (!sz_range(&second_gap) || !sz_range(&first_gap))
		return -EINVAL;

	/* Sort the two biggest gaps by address */
	if (first_gap.start > second_gap.start)
		swap(first_gap, second_gap);

	/* Store the result */
	regions[0].start = ALIGN(start, DAMON_MIN_REGION);
	regions[0].end = ALIGN(first_gap.start, DAMON_MIN_REGION);
	regions[1].start = ALIGN(first_gap.end, DAMON_MIN_REGION);
	regions[1].end = ALIGN(second_gap.start, DAMON_MIN_REGION);
	regions[2].start = ALIGN(second_gap.end, DAMON_MIN_REGION);
	regions[2].end = ALIGN(prev->vm_end, DAMON_MIN_REGION);

	return 0;
}

/*
 * Get the three regions in the given target (task)
 *
 * Returns 0 on success, negative error code otherwise.
 */
static int damon_va_three_regions(struct damon_target *t,
				struct damon_addr_range regions[3])
{
	struct mm_struct *mm;
	int rc;

	mm = damon_get_mm(t);
	if (!mm)
		return -EINVAL;

	/*
	 * [4.19 backport] mmap_read_lock()/mmap_read_unlock() - CONFIRMED
	 * to exist in this kernel (this was an assumption in the original
	 * stage-3 port, based only on the mm_struct field already being
	 * named 'mmap_lock'; now directly verified by seeing these exact
	 * wrapper calls used in this kernel's own mm/madvise.c).
	 */
	mmap_read_lock(mm);
	rc = __damon_va_three_regions(mm, regions);
	mmap_read_unlock(mm);

	mmput(mm);
	return rc;
}

/*
 * Initialize the monitoring target regions for the given target (task)
 *
 * t	the given target
 *
 * Because only a number of small portions of the entire address space
 * is actually mapped to the memory and accessed, monitoring the unmapped
 * regions is wasteful.  That said, because we can deal with small noises,
 * tracking every mapping is not strictly required but could even incur a high
 * overhead if the mapping frequently changes or the number of mappings is
 * high.  The adaptive regions adjustment mechanism will further help to deal
 * with the noise by simply identifying the unmapped areas as a region that
 * has no access.  Moreover, applying the real mappings that would have many
 * unmapped areas inside will make the adaptive mechanism quite complex.  That
 * said, too huge unmapped areas inside the monitoring target should be removed
 * to not take the time for the adaptive mechanism.
 *
 * For the reason, we convert the complex mappings to three distinct regions
 * that cover every mapped area of the address space.  Also the two gaps
 * between the three regions are the two biggest unmapped areas in the given
 * address space.  In detail, this function first identifies the start and the
 * end of the mappings and the two biggest unmapped areas of the address space.
 * Then, it constructs the three regions as below:
 *
 *     [mappings[0]->start, big_two_unmapped_areas[0]->start)
 *     [big_two_unmapped_areas[0]->end, big_two_unmapped_areas[1]->start)
 *     [big_two_unmapped_areas[1]->end, mappings[nr_mappings - 1]->end)
 *
 * As usual memory map of processes is as below, the gap between the heap and
 * the uppermost mmap()-ed region, and the gap between the lowermost mmap()-ed
 * region and the stack will be two biggest unmapped regions.  Because these
 * gaps are exceptionally huge areas in usual address space, excluding these
 * two biggest unmapped regions will be sufficient to make a trade-off.
 *
 *   <heap>
 *   <BIG UNMAPPED REGION 1>
 *   <uppermost mmap()-ed region>
 *   (other mmap()-ed regions and small unmapped regions)
 *   <lowermost mmap()-ed region>
 *   <BIG UNMAPPED REGION 2>
 *   <stack>
 */
static void __damon_va_init_regions(struct damon_ctx *ctx,
				     struct damon_target *t)
{
	struct damon_target *ti;
	struct damon_region *r;
	struct damon_addr_range regions[3];
	unsigned long sz = 0, nr_pieces;
	int i, tidx = 0;

	if (damon_va_three_regions(t, regions)) {
		damon_for_each_target(ti, ctx) {
			if (ti == t)
				break;
			tidx++;
		}
		pr_debug("Failed to get three regions of %dth target\n", tidx);
		return;
	}

	for (i = 0; i < 3; i++)
		sz += regions[i].end - regions[i].start;
	if (ctx->attrs.min_nr_regions)
		sz /= ctx->attrs.min_nr_regions;
	if (sz < DAMON_MIN_REGION)
		sz = DAMON_MIN_REGION;

	/* Set the initial three regions of the target */
	for (i = 0; i < 3; i++) {
		r = damon_new_region(regions[i].start, regions[i].end);
		if (!r) {
			pr_err("%d'th init region creation failed\n", i);
			return;
		}
		damon_add_region(r, t);

		nr_pieces = (regions[i].end - regions[i].start) / sz;
		damon_va_evenly_split_region(t, r, nr_pieces);
	}
}

/* Initialize '->regions_list' of every target (task) */
static void damon_va_init(struct damon_ctx *ctx)
{
	struct damon_target *t;

	damon_for_each_target(t, ctx) {
		/* the user may set the target regions as they want */
		if (!damon_nr_regions(t))
			__damon_va_init_regions(ctx, t);
	}
}

/*
 * Update regions for current memory mappings
 */
static void damon_va_update(struct damon_ctx *ctx)
{
	struct damon_addr_range three_regions[3];
	struct damon_target *t;

	damon_for_each_target(t, ctx) {
		if (damon_va_three_regions(t, three_regions))
			continue;
		damon_set_regions(t, three_regions, 3);
	}
}

static int damon_mkold_pmd_entry(pmd_t *pmd, unsigned long addr,
		unsigned long next, struct mm_walk *walk)
{
	pte_t *pte;
	pmd_t pmde;
	spinlock_t *ptl;

	if (pmd_trans_huge(*pmd)) {
		ptl = pmd_lock(walk->mm, pmd);
		pmde = *pmd;

		if (!pmd_present(pmde)) {
			spin_unlock(ptl);
			return 0;
		}

		if (pmd_trans_huge(pmde)) {
			damon_pmdp_mkold(pmd, walk->vma, addr);
			spin_unlock(ptl);
			return 0;
		}
		spin_unlock(ptl);
	}

	pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	/*
	 * [4.19 backport] Mainline checks `if (!pte) { walk->action =
	 * ACTION_AGAIN; return 0; }` here for a page-table-freed-under-RCU
	 * race. This kernel's pte_offset_map_lock() macro (checked in
	 * mm.h) always returns a valid pointer, and struct mm_walk here
	 * has no 'action' field at all, so that retry path is dropped.
	 */
	if (!pte_present(*pte))
		goto out;
	damon_ptep_mkold(pte, walk->vma, addr);
out:
	pte_unmap_unlock(pte, ptl);
	return 0;
}

/*
 * [4.19 backport] Hugetlb handling dropped for this backport stage - see
 * the file-level comment at the top for why.
 */
#define damon_mkold_hugetlb_entry NULL

static void damon_va_mkold(struct mm_struct *mm, unsigned long addr)
{
	struct mm_walk walk = {
		.pmd_entry = damon_mkold_pmd_entry,
		.hugetlb_entry = damon_mkold_hugetlb_entry,
		.mm = mm,
	};

	mmap_read_lock(mm);
	walk_page_range(addr, addr + 1, &walk);
	mmap_read_unlock(mm);
}

/*
 * Functions for the access checking of the regions
 */

static void __damon_va_prepare_access_check(struct mm_struct *mm,
					struct damon_region *r)
{
	r->sampling_addr = damon_rand(r->ar.start, r->ar.end);

	damon_va_mkold(mm, r->sampling_addr);
}

static void damon_va_prepare_access_checks(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct mm_struct *mm;
	struct damon_region *r;

	damon_for_each_target(t, ctx) {
		mm = damon_get_mm(t);
		if (!mm)
			continue;
		damon_for_each_region(r, t)
			__damon_va_prepare_access_check(mm, r);
		mmput(mm);
	}
}

struct damon_young_walk_private {
	/* [4.19 backport] size of the page (not folio) for the access
	 * checked virtual memory address
	 */
	unsigned long *page_sz;
	bool young;
};

static int damon_young_pmd_entry(pmd_t *pmd, unsigned long addr,
		unsigned long next, struct mm_walk *walk)
{
	pte_t *pte;
	pte_t ptent;
	spinlock_t *ptl;
	struct page *page;
	struct damon_young_walk_private *priv = walk->private;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (pmd_trans_huge(*pmd)) {
		pmd_t pmde;

		ptl = pmd_lock(walk->mm, pmd);
		pmde = *pmd;

		if (!pmd_present(pmde)) {
			spin_unlock(ptl);
			return 0;
		}

		if (!pmd_trans_huge(pmde)) {
			spin_unlock(ptl);
			goto regular_page;
		}
		page = damon_get_page(pmd_pfn(pmde));
		if (!page)
			goto huge_out;
		if (pmd_young(pmde) || !page_is_idle(page) ||
				mmu_notifier_test_young(walk->mm, addr))
			priv->young = true;
		*priv->page_sz = HPAGE_PMD_SIZE;
		put_page(page);
huge_out:
		spin_unlock(ptl);
		return 0;
	}

regular_page:
#endif	/* CONFIG_TRANSPARENT_HUGEPAGE */

	pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	/* [4.19 backport] see damon_mkold_pmd_entry() - no ACTION_AGAIN here */
	ptent = *pte;
	if (!pte_present(ptent))
		goto out;
	page = damon_get_page(pte_pfn(ptent));
	if (!page)
		goto out;
	if (pte_young(ptent) || !page_is_idle(page) ||
			mmu_notifier_test_young(walk->mm, addr))
		priv->young = true;
	/*
	 * [4.19 backport fix] page_size() turned out to not actually be
	 * declared in the mm.h used at build time here (despite appearing
	 * in the deps originally provided - possibly a different revision
	 * than what's really in this tree). Sidestepped entirely: with
	 * CONFIG_TRANSPARENT_HUGEPAGE and CONFIG_HUGETLBFS both off in
	 * this device's defconfig, a page reached via the regular
	 * (non-huge) PTE walk below can never be a compound page, so its
	 * size is always exactly PAGE_SIZE.
	 */
	*priv->page_sz = PAGE_SIZE;
	put_page(page);
out:
	pte_unmap_unlock(pte, ptl);
	return 0;
}

/*
 * [4.19 backport] Hugetlb handling dropped for this backport stage - see
 * the file-level comment at the top for why.
 */
#define damon_young_hugetlb_entry NULL

static bool damon_va_young(struct mm_struct *mm, unsigned long addr,
		unsigned long *page_sz)
{
	struct damon_young_walk_private arg = {
		.page_sz = page_sz,
		.young = false,
	};
	struct mm_walk walk = {
		.pmd_entry = damon_young_pmd_entry,
		.hugetlb_entry = damon_young_hugetlb_entry,
		.mm = mm,
		.private = &arg,
	};

	mmap_read_lock(mm);
	walk_page_range(addr, addr + 1, &walk);
	mmap_read_unlock(mm);
	return arg.young;
}

/*
 * Check whether the region was accessed after the last preparation
 *
 * mm	'mm_struct' for the given virtual address space
 * r	the region to be checked
 */
static void __damon_va_check_access(struct mm_struct *mm,
				struct damon_region *r, bool same_target,
				struct damon_attrs *attrs)
{
	static unsigned long last_addr;
	static unsigned long last_page_sz = PAGE_SIZE;
	static bool last_accessed;

	if (!mm) {
		damon_update_region_access_rate(r, false, attrs);
		return;
	}

	/* If the region is in the last checked page, reuse the result */
	if (same_target && (ALIGN_DOWN(last_addr, last_page_sz) ==
				ALIGN_DOWN(r->sampling_addr, last_page_sz))) {
		damon_update_region_access_rate(r, last_accessed, attrs);
		return;
	}

	last_accessed = damon_va_young(mm, r->sampling_addr, &last_page_sz);
	damon_update_region_access_rate(r, last_accessed, attrs);

	last_addr = r->sampling_addr;
}

static unsigned int damon_va_check_accesses(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct mm_struct *mm;
	struct damon_region *r;
	unsigned int max_nr_accesses = 0;
	bool same_target;

	damon_for_each_target(t, ctx) {
		mm = damon_get_mm(t);
		same_target = false;
		damon_for_each_region(r, t) {
			__damon_va_check_access(mm, r, same_target,
					&ctx->attrs);
			max_nr_accesses = max(r->nr_accesses, max_nr_accesses);
			same_target = true;
		}
		if (mm)
			mmput(mm);
	}

	return max_nr_accesses;
}

/*
 * Functions for the target validity check and cleanup
 */

static bool damon_va_target_valid(struct damon_target *t)
{
	struct task_struct *task;

	task = damon_get_task_struct(t);
	if (task) {
		put_task_struct(task);
		return true;
	}

	return false;
}

/*
 * [4.19 backport - RESOLVED after review of mm/madvise.c]
 *
 * do_madvise(mm, ...) itself has been added via a small patch to this
 * kernel's mm/madvise.c (extracted from the madvise(2) syscall body,
 * parameterized on an explicit mm instead of current->mm - see that
 * file's comment above do_madvise() for the exact diff). This actually
 * enables DAMOS_WILLNEED, DAMOS_HUGEPAGE and DAMOS_NOHUGEPAGE.
 *
 * DAMOS_COLD and DAMOS_PAGEOUT remain no-ops: confirmed by directly
 * inspecting this kernel's mm/madvise.c that neither the MADV_COLD/
 * MADV_PAGEOUT behavior constants nor any proactive-reclaim pte-walk
 * function (mainline's madvise_cold_or_pageout_pte_range() and its
 * vmscan.c reclaim helpers) exist here at all. That's a separate,
 * materially larger backport than DAMON itself and hasn't been done.
 */
static unsigned long damos_madvise(struct damon_target *target,
		struct damon_region *r, int behavior)
{
	struct mm_struct *mm;
	unsigned long start = PAGE_ALIGN(r->ar.start);
	unsigned long len = PAGE_ALIGN(damon_sz_region(r));
	unsigned long applied;

	mm = damon_get_mm(target);
	if (!mm)
		return 0;

	applied = do_madvise(mm, start, len, behavior) ? 0 : len;
	mmput(mm);

	return applied;
}

static unsigned long damon_va_apply_scheme(struct damon_ctx *ctx,
		struct damon_target *t, struct damon_region *r,
		struct damos *scheme, unsigned long *sz_filter_passed)
{
	int madv_action;

	switch (scheme->action) {
	case DAMOS_WILLNEED:
		madv_action = MADV_WILLNEED;
		break;
	/*
	 * [4.19 backport] MADV_COLD and MADV_PAGEOUT are undeclared in
	 * this kernel's uapi headers (confirmed by build error), so these
	 * two cases are guarded out entirely rather than referencing
	 * macros that don't exist. Both mainline actions are mainline
	 * 5.4+ additions. Falls through to 'default: return 0' below for
	 * these two, same as any other unsupported action.
	 */
#ifdef MADV_COLD
	case DAMOS_COLD:
		madv_action = MADV_COLD;
		break;
#endif
#ifdef MADV_PAGEOUT
	case DAMOS_PAGEOUT:
		madv_action = MADV_PAGEOUT;
		break;
#endif
	case DAMOS_HUGEPAGE:
		madv_action = MADV_HUGEPAGE;
		break;
	case DAMOS_NOHUGEPAGE:
		madv_action = MADV_NOHUGEPAGE;
		break;
	case DAMOS_STAT:
		return 0;
	default:
		/*
		 * DAMOS actions that are not yet supported by 'vaddr'.
		 */
		return 0;
	}

	return damos_madvise(t, r, madv_action);
}

static int damon_va_scheme_score(struct damon_ctx *context,
		struct damon_target *t, struct damon_region *r,
		struct damos *scheme)
{

	switch (scheme->action) {
	case DAMOS_PAGEOUT:
		return damon_cold_score(context, r, scheme);
	default:
		break;
	}

	return DAMOS_MAX_SCORE;
}

static int __init damon_va_initcall(void)
{
	struct damon_operations ops = {
		.id = DAMON_OPS_VADDR,
		.init = damon_va_init,
		.update = damon_va_update,
		.prepare_access_checks = damon_va_prepare_access_checks,
		.check_accesses = damon_va_check_accesses,
		.target_valid = damon_va_target_valid,
		.cleanup = NULL,
		.apply_scheme = damon_va_apply_scheme,
		.get_scheme_score = damon_va_scheme_score,
	};
	/* ops for fixed virtual address ranges */
	struct damon_operations ops_fvaddr = ops;
	int err;

	/* Don't set the monitoring target regions for the entire mapping */
	ops_fvaddr.id = DAMON_OPS_FVADDR;
	ops_fvaddr.init = NULL;
	ops_fvaddr.update = NULL;

	err = damon_register_ops(&ops);
	if (err)
		return err;
	return damon_register_ops(&ops_fvaddr);
};

subsys_initcall(damon_va_initcall);

#include "tests/vaddr-kunit.h"
