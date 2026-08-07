LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := ironrift
LOCAL_SRC_FILES := IronRiftSrc/ironrift_stub.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/ironwail/Quake
LOCAL_CFLAGS := -DIRO_RIFT_ANDROID=1 -DANDROID_GLES3=1 -Wno-unused-parameter
LOCAL_LDLIBS := -landroid -llog -lEGL -lGLESv3 -ldl
include $(BUILD_SHARED_LIBRARY)
