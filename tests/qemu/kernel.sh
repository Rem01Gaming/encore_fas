#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Builds a small arm64 kernel for the QEMU test. The kernel has lockdep, the
# debug objects checks, and atomic sleep checks, so it finds locking and timer
# mistakes in the module.
#
# usage: kernel.sh <mainline tag, for example v6.12>
# needs: git, make, aarch64-linux-gnu-gcc, flex, bison, bc, libssl-dev
set -e
tag=${1:?usage: kernel.sh <tag>}
work=${WORK:-$(pwd)/.qemu-work}
dir=$work/linux-$tag

mkdir -p "$work"
[ -d "$dir" ] || git clone -q --depth 1 -b "$tag" https://github.com/torvalds/linux.git "$dir"
cd "$dir"
export ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}

make -s tinyconfig
scripts/config -e 64BIT -e MODULES -e MODULE_UNLOAD -e MODULE_FORCE_UNLOAD \
	-e BLK_DEV_INITRD -e RD_GZIP -e BINFMT_ELF -e PRINTK -e PRINTK_TIME \
	-e TTY -e SERIAL_AMBA_PL011 -e SERIAL_AMBA_PL011_CONSOLE -e SERIAL_EARLYCON \
	-e DEVTMPFS -e DEVTMPFS_MOUNT -e PROC_FS -e SYSFS -e TMPFS -e SHMEM \
	-e MULTIUSER -e ARCH_VEXPRESS -e ARM_GIC -e ARM_GIC_V3 -e ARM_ARCH_TIMER \
	-e HIGH_RES_TIMERS -e NO_HZ_IDLE -e HZ_250 -e SMP -e MMU -e PSCI -e OF \
	-e PERF_EVENTS -e FTRACE -e FUNCTION_TRACER -e TRACING -e UPROBE_EVENTS \
	-e KALLSYMS -e EXPERT -e FUTEX -e EPOLL -e SIGNALFD -e TIMERFD -e EVENTFD \
	-e AIO -e POSIX_TIMERS -e ADVISE_SYSCALLS -e UNIX -e PROC_SYSCTL \
	-e PREEMPT -e DEBUG_KERNEL -e DEBUG_ATOMIC_SLEEP -e DEBUG_SPINLOCK \
	-e PROVE_LOCKING -e LOCKDEP -e DEBUG_OBJECTS -e DEBUG_OBJECTS_TIMERS \
	-e DEBUG_OBJECTS_FREE -e DEBUG_OBJECTS_WORK -e SLUB_DEBUG \
	-d MODULE_SIG -d DEBUG_INFO_BTF -d WERROR
make -s olddefconfig
make -j"$(nproc)" Image modules
echo "kernel ready: $dir"
