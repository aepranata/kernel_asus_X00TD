# SPDX-License-Identifier: GPL-2.0
#
# [4.19 backport - FINAL wiring]
#
# Now matches mainline's structure exactly. 'obj-y := core.o'
# (unconditional within this sub-Makefile, not obj-$(CONFIG_DAMON)) is
# correct and matches mainline: this mm/damon/ subdirectory is only ever
# entered in the first place via the parent mm/Makefile's own
# 'obj-$(CONFIG_DAMON) += damon/' line, so core.o only actually gets built
# when CONFIG_DAMON=y regardless of using obj-y here.
#
# ops-common.o is listed under both DAMON_VADDR and DAMON_PADDR, and
# modules-common.o under both DAMON_RECLAIM and DAMON_LRU_SORT - this is
# mainline's own pattern (not a mistake): if either config needs the
# object, list it there; Kbuild builds each object once regardless of how
# many obj-$(CONFIG_X) lines reference it.

obj-y				:= core.o
obj-$(CONFIG_DAMON_VADDR)	+= ops-common.o vaddr.o
obj-$(CONFIG_DAMON_PADDR)	+= ops-common.o paddr.o
obj-$(CONFIG_DAMON_SYSFS)	+= sysfs-common.o sysfs-schemes.o sysfs.o
obj-$(CONFIG_DAMON_RECLAIM)	+= modules-common.o reclaim.o
obj-$(CONFIG_DAMON_LRU_SORT)	+= modules-common.o lru_sort.o
