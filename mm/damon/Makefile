# SPDX-License-Identifier: GPL-2.0
#
# [4.19 backport] Stage 4: paddr.o added (rmap_walk-based physical address
# monitoring, page-based like everything else here). DAMOS_PAGEOUT is
# fully wired up via this kernel's reclaim_pages_from_list()
# (CONFIG_PROCESS_RECLAIM=y, already active in this device's defconfig).
# DAMOS_LRU_DEPRIO is a documented no-op (no deactivate_page() in this
# kernel). DAMOS_MIGRATE_HOT/COLD is fully ported but will always be a
# practical no-op on this single-NUMA-node device. sysfs*.o / reclaim.o /
# lru_sort.o still pending.
#
# core.o / ops-common.o / vaddr.o / paddr.o are temporarily all built
# together under CONFIG_DAMON itself, since the CONFIG_DAMON_VADDR /
# CONFIG_DAMON_PADDR Kconfig entries they'll eventually be split across
# (matching mainline) don't exist yet - added once the module split
# happens in a later cleanup stage.

obj-$(CONFIG_DAMON)		:= core.o ops-common.o vaddr.o paddr.o
