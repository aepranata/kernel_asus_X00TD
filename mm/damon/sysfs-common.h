/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common Code for DAMON Sysfs Interface
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

/*
 * [4.19 backport - applies to every ktype in sysfs.c/sysfs-common.c/
 * sysfs-schemes.c, not just this file]
 *
 * Every 'const struct kobj_type' in mainline is declared here as plain
 * 'struct kobj_type' (const dropped). Confirmed directly in this
 * kernel's include/linux/kobject.h: kobject_init() and
 * kobject_init_and_add() both take a non-const 'struct kobj_type *ktype'
 * parameter. Mainline's const-qualified kobj_type is a hardening change
 * from a later kernel version that this tree doesn't have - passing our
 * ktype structs as const here would fail to compile (assigning a
 * 'const struct kobj_type *' where a non-const one is expected).
 *
 * SECOND correction (found via build error, not caught by static review -
 * the deps snapshot for kobject.h claimed 'default_groups' existed in
 * struct kobj_type, but the real header used at build time doesn't have
 * that field at all): every '.default_groups = X_groups' in a kobj_type
 * literal is changed to '.default_attrs = X_attrs' - the older, more
 * fundamental flat-array field, using the underlying attribute array that
 * was already being built via ATTRIBUTE_GROUPS() anyway. The now-unused
 * ATTRIBUTE_GROUPS(X) macro invocations were removed entirely (left in,
 * they'd generate an unused static array and risk -Wunused-variable
 * under -Werror).
 *
 * Everything else in the sysfs files (sysfs_emit()/sysfs_emit_at(),
 * kobject_init_and_add(), kobject_create_and_add(), sysfs_streq(),
 * kstrtoul()/kstrtoint()/kstrtouint()/kstrtobool()) is confirmed present
 * and unchanged from mainline usage in this kernel's own
 * include/linux/sysfs.h and include/linux/kobject.h.
 */

/*
 * [4.19 backport] __ATTR_RW_MODE(name, mode) doesn't exist in this
 * kernel's include/linux/sysfs.h (only __ATTR / __ATTR_RO / __ATTR_RO_MODE
 * / __ATTR_RW are defined there). Every one of the ~48 uses across the
 * sysfs files passes an explicit 0600 mode already (never relying on
 * __ATTR_RW's baked-in 0644 default), so this shim is defined in terms of
 * the always-present generic __ATTR() exactly the way mainline's own
 * __ATTR_RW_MODE does it (forcing S_IWUSR into the mode is a no-op for
 * every actual call site here, since 0600 already includes it - kept for
 * fidelity to mainline's definition regardless).
 */
#ifndef __ATTR_RW_MODE
#define __ATTR_RW_MODE(_name, _mode) \
	__ATTR(_name, (S_IWUSR | (_mode)), _name##_show, _name##_store)
#endif

#include <linux/damon.h>
#include <linux/kobject.h>

extern struct mutex damon_sysfs_lock;

struct damon_sysfs_ul_range {
	struct kobject kobj;
	unsigned long min;
	unsigned long max;
};

struct damon_sysfs_ul_range *damon_sysfs_ul_range_alloc(
		unsigned long min,
		unsigned long max);
void damon_sysfs_ul_range_release(struct kobject *kobj);

extern struct kobj_type damon_sysfs_ul_range_ktype;

/*
 * schemes directory
 */

struct damon_sysfs_schemes {
	struct kobject kobj;
	struct damon_sysfs_scheme **schemes_arr;
	int nr;
};

struct damon_sysfs_schemes *damon_sysfs_schemes_alloc(void);
void damon_sysfs_schemes_rm_dirs(struct damon_sysfs_schemes *schemes);

extern struct kobj_type damon_sysfs_schemes_ktype;

int damon_sysfs_add_schemes(struct damon_ctx *ctx,
		struct damon_sysfs_schemes *sysfs_schemes);

void damon_sysfs_schemes_update_stats(
		struct damon_sysfs_schemes *sysfs_schemes,
		struct damon_ctx *ctx);

void damos_sysfs_populate_region_dir(struct damon_sysfs_schemes *sysfs_schemes,
		struct damon_ctx *ctx, struct damon_target *t,
		struct damon_region *r, struct damos *s,
		bool total_bytes_only, unsigned long sz_filter_passed);

int damon_sysfs_schemes_clear_regions(
		struct damon_sysfs_schemes *sysfs_schemes);

int damos_sysfs_set_quota_scores(struct damon_sysfs_schemes *sysfs_schemes,
		struct damon_ctx *ctx);

void damos_sysfs_update_effective_quotas(
		struct damon_sysfs_schemes *sysfs_schemes,
		struct damon_ctx *ctx);
