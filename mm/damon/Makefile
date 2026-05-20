# SPDX-License-Identifier: GPL-2.0
#
# [4.19 backport] Stage 5: sysfs.o / sysfs-common.o / sysfs-schemes.o added
# (userspace control interface). Only one systemic API difference found
# across all three files: this kernel's kobject_init()/kobject_init_and_add()
# take a non-const 'struct kobj_type *' (mainline's const-qualified
# kobj_type is a later hardening change this tree doesn't have) - every
# 'const struct kobj_type' became plain 'struct kobj_type'. Everything
# else (sysfs_emit(), .default_groups, kobject_create_and_add(),
# sysfs_streq(), kstrto*()) is confirmed present and unchanged from
# mainline usage. __ATTR_RW_MODE() (used ~48 times) doesn't exist in this
# kernel's sysfs.h either - a compat shim is defined once in
# sysfs-common.h rather than rewriting every call site.
# reclaim.o / lru_sort.o still pending.
#
# core.o / ops-common.o / vaddr.o / paddr.o / sysfs*.o are temporarily all
# built together under CONFIG_DAMON itself, since the CONFIG_DAMON_VADDR /
# CONFIG_DAMON_PADDR / CONFIG_DAMON_SYSFS Kconfig entries they'll
# eventually be split across (matching mainline) don't exist yet - added
# once the module split happens in a later cleanup stage.

obj-$(CONFIG_DAMON)		:= core.o ops-common.o vaddr.o paddr.o \
				   sysfs.o sysfs-common.o sysfs-schemes.o
