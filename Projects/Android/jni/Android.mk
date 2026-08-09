IRONRIFT_PATH := $(call my-dir)

# Pinned native audio/runtime dependency: SDL2 release-2.30.12.
SDL_ROOT := $(IRONRIFT_PATH)/SupportLibs/SDL2
TOUCH_CONTROLS_NO_SDL := 1
TOUCH_ROOT := $(IRONRIFT_PATH)/SupportLibs/MobileTouchControls
include $(TOUCH_ROOT)/Android.mk
include $(SDL_ROOT)/Android.mk

# Ironwail is the active renderer/engine repository.  Keep the wrapper as a
# host and compile the Android-selected engine sources directly from it.
LOCAL_PATH := $(IRONRIFT_PATH)
IW_ROOT := $(LOCAL_PATH)/../../../../ironwail
IW_QUAKE := $(IW_ROOT)/Quake
IW_SRC := $(wildcard $(IW_QUAKE)/*.c)
IW_SRC := $(filter-out \
    %/main_sdl.c \
    %/sys_sdl_win.c %/pl_win.c %/pl_linux.c \
    %/net_win.c %/net_wins.c %/net_wipx.c \
    %/snd_flac.c %/snd_mikmod.c %/snd_modplug.c %/snd_mp3.c \
    %/snd_mpg123.c %/snd_opus.c %/snd_umx.c %/snd_vorbis.c %/snd_xmp.c %/lodepng.c, \
    $(IW_SRC))
IW_SRC := $(subst $(LOCAL_PATH)/,,$(IW_SRC))

include $(CLEAR_VARS)
LOCAL_MODULE := ironrift
LOCAL_SRC_FILES := $(IW_SRC) IronRiftSrc/ironrift_jni.cpp IronRiftSrc/touch/ironrift_touch_adapter.cpp
LOCAL_C_INCLUDES := $(IW_QUAKE) $(SDL_ROOT)/include $(TOUCH_ROOT)
LOCAL_CFLAGS := -DIRO_RIFT_ANDROID=1 -DANDROID_GLES3=1 -DNO_SDL_CONFIG -DUSE_SDL2 -DUSE_CODEC_WAVE -DWITHOUT_CURL -Wno-unused-parameter
LOCAL_CPPFLAGS := -std=c++17
LOCAL_STATIC_LIBRARIES := SDL2_static
LOCAL_SHARED_LIBRARIES := touchcontrols
LOCAL_LDLIBS := -landroid -llog -lEGL -lGLESv3 -ldl -lOpenSLES -lm
include $(BUILD_SHARED_LIBRARY)
