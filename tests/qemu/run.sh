#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Runs the module in QEMU on a kernel that kernel.sh built. The exit code is 0
# when every selftest passes and the kernel log has no warning.
#
# usage: run.sh <mainline tag>
# needs: qemu-system-aarch64, aarch64-linux-gnu-gcc, python3
set -e
tag=${1:?usage: run.sh <tag>}
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../..
work=${WORK:-$here/.qemu-work}
kdir=${KTREE:-$work/linux-$tag}
export ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}
out=$work/run-$tag
mkdir -p "$out"

echo "== build the module against $kdir"
rm -rf "$out/src" && mkdir "$out/src" && cp -r "$repo/kernel/." "$out/src/"
make -C "$kdir" M="$out/src" CONFIG_ENCORE_FAS=m modules >"$out/build.log" 2>&1 ||
	{ tail -20 "$out/build.log"; exit 1; }

echo "== build fas_ctl and init"
${CROSS_COMPILE}gcc -std=gnu11 -O2 -static -pthread -I"$repo/kernel/include" \
	-o "$out/fas_ctl" "$repo"/userspace_test/jni/fas_*.c
${CROSS_COMPILE}gcc -O2 -static -o "$out/init" "$here/init.c"
python3 "$here/mkcpio.py" "$out/initramfs.cpio" "$out/init" "$out/fas_ctl" "$out/src/encore_fas.ko"
gzip -f "$out/initramfs.cpio"

echo "== boot"
timeout "${QEMU_TIMEOUT:-900}" qemu-system-aarch64 -machine virt -cpu cortex-a72 -smp 2 \
	-m 1024 -nographic -no-reboot -kernel "$kdir/arch/arm64/boot/Image" \
	-initrd "$out/initramfs.cpio.gz" \
	-append "console=ttyAMA0 rdinit=/init panic=-1 loglevel=4" 2>&1 |
	tr -d '\r' >"$out/qemu.log" || true

sed -n '/== load module/,$p' "$out/qemu.log" | grep -E "^\[(PASS|FAIL|SKIP)\]|SELFTEST|^==|cycles" || true

passed=$(grep -c "^SELFTEST PASSED" "$out/qemu.log" || true)
# Kernel v5.10 with FUNCTION_TRACER prints a warning at every module load. Two
# object files of one module both have a __patchable_function_entries section.
# The warning is a kernel issue. It is not a problem of the module.
problems=$(awk '/sysfs: cannot create duplicate filename .\/module\/encore_fas\/sections\// { skip = 1 }
	skip && /el0_sync\+/ { skip = 0; next }
	!skip' "$out/qemu.log" | grep -v '^<' |
	grep -cE "WARNING|BUG:|possible (circular|recursive)|inconsistent|Oops|debugobjects|sleeping function|Call [Tt]race" || true)
echo "selftests passed: $passed of 2, kernel log problems: $problems"
[ "$passed" = 2 ] && [ "$problems" = 0 ]
