# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_ENCORE_FAS) += encore_fas.o
encore_fas-y := encore_fas_main.o \
                encore_fas_ctx.o \
                encore_fas_evq.o \
                encore_fas_time.o

ifeq ($(KERNELRELEASE),)

# Out-of-tree build: make KDIR=<kernel build dir> ARCH=arm64 CROSS_COMPILE=...
KDIR ?= /lib/modules/$(shell uname -r)/build
HOSTCC ?= cc

all: check_symbol
	$(MAKE) -C $(KDIR) M=$(CURDIR) CONFIG_ENCORE_FAS=m modules
	@if [ -f $(KDIR)/vmlinux ]; then \
		./scripts/tools/check_symbol encore_fas.ko $(KDIR)/vmlinux; \
	else \
		echo "-- no vmlinux in KDIR, skipping symbol check"; \
	fi

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f scripts/tools/check_symbol

check_symbol: scripts/tools/check_symbol.c
	$(HOSTCC) scripts/tools/check_symbol.c -o scripts/tools/check_symbol

.PHONY: all clean

else

# Kbuild changed the meaning of $(src) for external modules in v6.13.
# The path of this Makefile works on every version.
fas_srcdir := $(patsubst %/,%,$(dir $(realpath $(lastword $(MAKEFILE_LIST)))))
fas_git := git -C $(fas_srcdir)

# The version is the commit count. setup.sh unshallows the repository.
fas_git_version := $(shell $(fas_git) rev-list --count HEAD 2>/dev/null)
fas_git_branch := $(shell $(fas_git) rev-parse --abbrev-ref HEAD 2>/dev/null)

ifneq ($(fas_git_version),)
ccflags-y += -DFAS_VERSION=$(fas_git_version)
ccflags-y += -DFAS_BRANCH=\"$(fas_git_branch)\"
else
$(warning encore_fas is not a git repository. The version is 0.)
endif

ccflags-y += -I$(fas_srcdir)/include

# Probe the kernel headers. A vendor kernel can backport an API, so the
# headers are more exact than LINUX_VERSION_CODE.
fas_uprobes_h := $(srctree)/include/linux/uprobes.h
fas_hrtimer_h := $(srctree)/include/linux/hrtimer.h

ifneq ($(shell grep -qs uprobe_unregister_nosync $(fas_uprobes_h) && echo y),)
ifneq ($(shell grep -qs '__u64 \*' $(fas_uprobes_h) && echo y),)
ccflags-y += -DFAS_UPROBE_API=3
else
ccflags-y += -DFAS_UPROBE_API=2
endif
else
ccflags-y += -DFAS_UPROBE_API=1
endif

ifneq ($(shell grep -qs hrtimer_setup $(fas_hrtimer_h) && echo y),)
ccflags-y += -DFAS_HAVE_HRTIMER_SETUP
endif

# The timer modes moved to hrtimer_types.h in v6.10.
ifneq ($(shell cat $(srctree)/include/linux/hrtimer*.h 2>/dev/null | grep -qs HRTIMER_MODE_REL_PINNED_SOFT && echo y),)
ccflags-y += -DFAS_HAVE_HRTIMER_SOFT
endif

endif
