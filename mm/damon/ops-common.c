// SPDX-License-Identifier: GPL-2.0
/*
 * Common Code for Data Access Monitoring
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#include <linux/mmu_notifier.h>
#include <linux/page_idle.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/swap.h>

#include "ops-common.h"

/*
 * [4.19 backport] Mainline's damon_get_folio() is built on folio_try_get()/
 * folio_test_lru(), which don't exist here. This kernel's own
 * mm/page_idle.c (page_idle_get_page()) already solves the exact same
 * problem - "get an online page for a pfn if it's on the LRU" - using the
 * page + zone_lru_lock API this kernel actually has, so this is that same
 * logic, just renamed and with pfn_valid()+pfn_to_page() in place of
 * pfn_to_online_page() (a newer helper that's also not in this kernel;
 * page_idle.c already avoids it for the same reason).
 *
 * Get an online page for a pfn if it's in the LRU list.  Otherwise, returns
 * NULL.
 */
struct page *damon_get_page(unsigned long pfn)
{
	struct page *page;
	struct zone *zone;

	if (!pfn_valid(pfn))
		return NULL;

	page = pfn_to_page(pfn);
	if (!page || !PageLRU(page) || !get_page_unless_zero(page))
		return NULL;

	zone = page_zone(page);
	spin_lock_irq(zone_lru_lock(zone));
	if (unlikely(!PageLRU(page))) {
		put_page(page);
		page = NULL;
	}
	spin_unlock_irq(zone_lru_lock(zone));
	return page;
}

void damon_ptep_mkold(pte_t *pte, struct vm_area_struct *vma, unsigned long addr)
{
	pte_t pteval = *pte;
	struct page *page;

	/*
	 * [4.19 backport] Mainline also handles non-present "PFN swap PTEs"
	 * here (device-exclusive / migration entries, via
	 * swp_offset_pfn(pte_to_swp_entry(pteval))). That concept doesn't
	 * exist in this kernel's swapops.h, so non-present PTEs are simply
	 * skipped, matching DAMON's behavior from before that mainline
	 * refactor was added.
	 */
	if (!pte_present(pteval))
		return;

	page = damon_get_page(pte_pfn(pteval));
	if (!page)
		return;

	/*
	 * ptep_clear_young_notify() resolves (via linux/mmu_notifier.h) to
	 * ptep_test_and_clear_young() combined with a secondary-MMU notify
	 * when CONFIG_MMU_NOTIFIER is enabled, or to a plain
	 * ptep_test_and_clear_young() otherwise - same call this kernel's
	 * own mm/page_idle.c uses for the same purpose.
	 */
	if (ptep_clear_young_notify(vma, addr, pte))
		set_page_young(page);

	set_page_idle(page);
	put_page(page);
}

void damon_pmdp_mkold(pmd_t *pmd, struct vm_area_struct *vma, unsigned long addr)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	struct page *page = damon_get_page(pmd_pfn(*pmd));

	if (!page)
		return;

	if (pmdp_clear_young_notify(vma, addr, pmd))
		set_page_young(page);

	set_page_idle(page);
	put_page(page);
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
}

#define DAMON_MAX_SUBSCORE	(100)
#define DAMON_MAX_AGE_IN_LOG	(32)

int damon_hot_score(struct damon_ctx *c, struct damon_region *r,
			struct damos *s)
{
	int freq_subscore;
	unsigned int age_in_sec;
	int age_in_log, age_subscore;
	unsigned int freq_weight = s->quota.weight_nr_accesses;
	unsigned int age_weight = s->quota.weight_age;
	int hotness;

	freq_subscore = r->nr_accesses * DAMON_MAX_SUBSCORE /
		damon_max_nr_accesses(&c->attrs);

	age_in_sec = (unsigned long)r->age * c->attrs.aggr_interval / 1000000;
	for (age_in_log = 0; age_in_log < DAMON_MAX_AGE_IN_LOG && age_in_sec;
			age_in_log++, age_in_sec >>= 1)
		;

	/* If frequency is 0, higher age means it's colder */
	if (freq_subscore == 0)
		age_in_log *= -1;

	/*
	 * Now age_in_log is in [-DAMON_MAX_AGE_IN_LOG, DAMON_MAX_AGE_IN_LOG].
	 * Scale it to be in [0, 100] and set it as age subscore.
	 */
	age_in_log += DAMON_MAX_AGE_IN_LOG;
	age_subscore = age_in_log * DAMON_MAX_SUBSCORE /
		DAMON_MAX_AGE_IN_LOG / 2;

	hotness = (freq_weight * freq_subscore + age_weight * age_subscore);
	if (freq_weight + age_weight)
		hotness /= freq_weight + age_weight;
	/*
	 * Transform it to fit in [0, DAMOS_MAX_SCORE]
	 */
	hotness = hotness * DAMOS_MAX_SCORE / DAMON_MAX_SUBSCORE;

	return hotness;
}

int damon_cold_score(struct damon_ctx *c, struct damon_region *r,
			struct damos *s)
{
	int hotness = damon_hot_score(c, r, s);

	/* Return coldness of the region */
	return DAMOS_MAX_SCORE - hotness;
}
