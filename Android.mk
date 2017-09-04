LOCAL_PATH:= $(call my-dir)

common_src_files := \
	VolumeManager.cpp \
	CommandListener.cpp \
	VoldCommand.cpp \
	NetlinkManager.cpp \
	NetlinkHandler.cpp \
	Process.cpp \
	fs/Ext4.cpp \
	fs/Vfat.cpp \
	fs/Ntfs.cpp \
	fs/Exfat.cpp \
	fs/Hfsplus.cpp \
	fs/Iso9660.cpp \
	Disk.cpp \
	VolumeBase.cpp \
	PublicVolume.cpp \
	ResponseCode.cpp \
	Utils.cpp \
	secontext.cpp \

common_shared_libraries := \
	libsysutils \
	libcutils \
	liblog \
	liblogwrap \
	libext4_utils \
	libselinux \
	libutils \
	libbase

common_static_libraries := \
	libfs_mgr \

vold_conlyflags := -std=c11
vold_cflags := -Werror -Wall -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-parameter

include $(CLEAR_VARS)

LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk
LOCAL_MODULE := droidvold
LOCAL_CLANG := true
LOCAL_SRC_FILES := \
	main.cpp \
	$(common_src_files)

LOCAL_INIT_RC := droidvold.rc

LOCAL_C_INCLUDES := $(common_c_includes)
LOCAL_CFLAGS := $(vold_cflags)
LOCAL_CONLYFLAGS := $(vold_conlyflags)

LOCAL_SHARED_LIBRARIES := $(common_shared_libraries)
LOCAL_STATIC_LIBRARIES := $(common_static_libraries)
LOCAL_REQUIRED_MODULES := $(required_modules)
LOCAL_PROPRIETARY_MODULE := true

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk
LOCAL_CLANG := true
LOCAL_SRC_FILES := dvdc.cpp
LOCAL_MODULE := dvdc
LOCAL_SHARED_LIBRARIES := libcutils libbase
LOCAL_CFLAGS := $(vold_cflags)
LOCAL_CONLYFLAGS := $(vold_conlyflags)
LOCAL_PROPRIETARY_MODULE := true

include $(BUILD_EXECUTABLE)
