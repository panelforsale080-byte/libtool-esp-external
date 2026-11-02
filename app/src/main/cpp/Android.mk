LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := LibToolEsp
LOCAL_SRC_FILES := main.cpp
LOCAL_CFLAGS := -fPIC -O2 -w -DANDROID
LOCAL_CPPFLAGS := -std=c++17 -fexceptions -frtti -w
LOCAL_LDLIBS := -llog -landroid
include $(BUILD_SHARED_LIBRARY)
