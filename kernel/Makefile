obj-$(CONFIG_ENCORE_FAS) += encore_fas.o
encore_fas-y := encore_fas_main.o \
                encore_fas_ioctl.o \
                encore_fas_context.o \
                encore_fas_jank.o \
                encore_fas_eventq.o

ifeq ($(KERNELRELEASE),)

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
HOSTCC ?= cc

all: check_symbol
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	@if [ -f $(KDIR)/vmlinux ]; then \
		./scripts/tools/check_symbol encore_fas.ko $(KDIR)/vmlinux; \
	else \
		echo "-- no vmlinux in KDIR, skipping symbol check"; \
	fi

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f scripts/tools/check_symbol

check_symbol: scripts/tools/check_symbol.c
	$(HOSTCC) scripts/tools/check_symbol.c -o scripts/tools/check_symbol

.PHONY: all clean

else

fas_srcdir := $(if $(filter /%,$(src)),$(src),$(srctree)/$(src))
fas_git := git -C $(fas_srcdir)

$(shell if [ "$$($(fas_git) rev-parse --is-shallow-repository 2>/dev/null)" = true ]; then $(fas_git) fetch --unshallow >/dev/null 2>&1; fi)

fas_git_version := $(shell $(fas_git) rev-list --count HEAD 2>/dev/null)
fas_git_branch := $(shell $(fas_git) rev-parse --abbrev-ref HEAD 2>/dev/null)

ifneq ($(fas_git_version),)
FAS_VERSION := $(fas_git_version)
ccflags-y += -DFAS_VERSION=$(FAS_VERSION)
ccflags-y += -DFAS_BRANCH=\"$(fas_git_branch)\"
$(info -- Encore FAS version: $(FAS_VERSION))
$(info -- Encore FAS branch: $(fas_git_branch))
else
$(warning "FAS_GIT_VERSION not defined! It is better to make Encore FAS a git repository!")
ccflags-y += -DFAS_VERSION=0
ccflags-y += -DFAS_BRANCH=\"-\"
endif

ccflags-y += -I$(fas_srcdir)/include

endif
