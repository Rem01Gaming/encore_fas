LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := encore_fas

LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../kernel/include

LOCAL_SRC_FILES := fas_ctl.c fas_client.c fas_elf.c

include $(BUILD_EXECUTABLE)
