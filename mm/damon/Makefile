# SPDX-License-Identifier: GPL-2.0
#
# [4.19 backport] Stage 3: vaddr.o added (rewritten against the old
# struct mm_walk API - no mm_walk_ops table in this kernel). Hugetlb
# entries are compiled out (CONFIG_HUGETLBFS is off in this device's
# defconfig). paddr.o / sysfs*.o / reclaim.o / lru_sort.o still pending.
#
# core.o / ops-common.o / vaddr.o are temporarily all built together
# under CONFIG_DAMON itself, since the CONFIG_DAMON_VADDR Kconfig entry
# they'll eventually be split across (matching mainline) doesn't exist
# yet - added once the module split happens in a later cleanup stage.

obj-$(CONFIG_DAMON)		:= core.o ops-common.o vaddr.o
