# SPDX-License-Identifier: GPL-2.0
#
# [4.19 backport] Trimmed to core.o only for this stage of the port.
# vaddr.o / paddr.o / sysfs*.o / reclaim.o / lru_sort.o will be added back
# here once they're individually ported (they all depend on folio/pagewalk
# APIs that 4.19 doesn't have yet).

obj-$(CONFIG_DAMON)		:= core.o
