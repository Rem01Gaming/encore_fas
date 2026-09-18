LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := fas_ctl

LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../kernel/include
LOCAL_SRC_FILES := fas_ctl.c fas_client.c fas_elf.c fas_selftest.c
LOCAL_CFLAGS := -std=gnu11 -O2 -Wall -Wextra

# The selftest probes a function of this executable. The function must stay in
# the dynamic symbol table, because ndk-build strips the file.
LOCAL_LDFLAGS := -Wl,--export-dynamic

include $(BUILD_EXECUTABLE)
