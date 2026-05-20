# SPDX-License-Identifier: GPL-2.0
#
# [4.19 backport] Stage 2: ops-common.o added (folio -> page, no pagewalk
# dependency). vaddr.o / paddr.o / sysfs*.o / reclaim.o / lru_sort.o still
# pending - they depend on the mm_walk_ops pagewalk API that 4.19 doesn't
# have, and will need a manual rewrite to the old struct mm_walk style.
#
# ops-common.o is temporarily built under CONFIG_DAMON itself, since the
# CONFIG_DAMON_VADDR Kconfig entry it'll eventually live under (matching
# mainline) doesn't exist yet - that gets added once vaddr.c is ported and
# actually calls into it. Building it now just so any compile errors in it
# surface at this stage instead of getting bundled into the vaddr.c diff.

obj-$(CONFIG_DAMON)		:= core.o ops-common.o
