#include <jni.h>
#include <android/log.h>
#include <stdint.h>
#include <time.h>
#include <string>
#include <vector>

#include "android_lifecycle.h"

#define IW_TAG "IronRift"
#define IW_LOG(...) __android_log_print(ANDROID_LOG_INFO, IW_TAG, __VA_ARGS__)

static uint64_t IW_NowNS(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeInit(JNIEnv *env, jclass, jstring data_dir, jobjectArray launch_args)
{
    const char *path = data_dir ? env->GetStringUTFChars(data_dir, nullptr) : ".";
    std::vector<std::string> args;
    args.emplace_back("ironwail");
    args.emplace_back("-basedir");
    args.emplace_back(path);
    args.emplace_back("-nojoy");
    if (launch_args)
    {
        jsize count = env->GetArrayLength(launch_args);
        for (jsize i = 0; i < count; ++i)
        {
            jstring value = (jstring)env->GetObjectArrayElement(launch_args, i);
            if (value)
            {
                const char *text = env->GetStringUTFChars(value, nullptr);
                args.emplace_back(text ? text : "");
                if (text)
                    env->ReleaseStringUTFChars(value, text);
                env->DeleteLocalRef(value);
            }
        }
    }
    std::vector<const char *> argv;
    argv.reserve(args.size());
    for (const std::string &arg : args)
        argv.push_back(arg.c_str());
    IW_Android_SurfaceCreated();
    if (!IW_Android_Init(path, (int)argv.size(), argv.data()))
        IW_LOG("Ironwail native initialization failed");
    if (data_dir)
        env->ReleaseStringUTFChars(data_dir, path);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeResize(JNIEnv *, jclass, jint width, jint height)
{
    IW_Android_Resize(width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeRender(JNIEnv *, jclass)
{
    IW_Android_Frame(IW_NowNS());
}

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeKey(JNIEnv *, jclass, jint keycode, jboolean down)
{
    IW_Android_Key(keycode, down != 0);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeAxis(JNIEnv *, jclass, jint device_id, jint axis, jfloat value)
{
    IW_Android_Axis(device_id, axis, value);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeTouch(JNIEnv *, jclass, jint action, jfloat x, jfloat y)
{
    IW_Android_Touch(action, x, y);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativePause(JNIEnv *, jclass, jboolean paused)
{
    IW_Android_Pause(paused != 0);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeShutdown(JNIEnv *, jclass)
{
    IW_Android_Shutdown();
}
