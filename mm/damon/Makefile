# SPDX-License-Identifier: GPL-2.0
#
# [4.19 backport] Stage 6: reclaim.o / lru_sort.o / modules-common.o added
# (ready-made DAMOS modules). Only fix needed: <linux/kstrtox.h> doesn't
# exist in this kernel, swapped for <linux/kernel.h> (where kstrtobool()/
# kstrtoul() etc. are still declared here). Both modules build on APIs
# already proven working in earlier stages (damon_new_scheme(),
# damos_new_quota_goal(), damon_set_region_biggest_system_ram_default()
# from core.c; DAMOS_PAGEOUT/DAMOS_LRU_PRIO from paddr.c).
#
# DAMON_LRU_SORT's cold-side action (DAMOS_LRU_DEPRIO) is a no-op on this
# device - deactivate_page() isn't available here (see paddr.c). Its
# hot-side (DAMOS_LRU_PRIO) does work.
#
# All DAMON object files are still temporarily built together under
# CONFIG_DAMON itself rather than mainline's separate DAMON_VADDR /
# DAMON_PADDR / DAMON_SYSFS Kconfig entries - see the earlier stage
# comments in this file. reclaim.o/lru_sort.o are correctly gated on
# their own DAMON_RECLAIM/DAMON_LRU_SORT options though, since those were
# added properly in this stage.

obj-$(CONFIG_DAMON)		:= core.o ops-common.o vaddr.o paddr.o \
				   sysfs.o sysfs-common.o sysfs-schemes.o
obj-$(CONFIG_DAMON_RECLAIM)	+= reclaim.o modules-common.o
obj-$(CONFIG_DAMON_LRU_SORT)	+= lru_sort.o modules-common.o
