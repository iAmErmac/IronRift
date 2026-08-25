#include <jni.h>
#include <android/log.h>
#include <stdint.h>
#include <time.h>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <SDL_system.h>

#include "android_lifecycle.h"


#define IW_TAG "IronRift"
#define IW_LOG(...) __android_log_print(ANDROID_LOG_INFO, IW_TAG, __VA_ARGS__)
extern "C" void Modlist_AndroidDownloadProgress(int bytes);
extern "C" qboolean Modlist_AndroidDownloadCancelled(void);

static JNIEnv *IW_GetJNIEnv(void)
{
    return static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
}

extern "C" char *IW_Android_DownloadText(const char *url)
{
    JNIEnv *env = IW_GetJNIEnv();
    jobject activity = env ? static_cast<jobject>(SDL_AndroidGetActivity()) : nullptr;
    char *result = nullptr;
    if (activity)
    {
        jclass type = env->GetObjectClass(activity);
        jmethodID method = type ? env->GetMethodID(type, "downloadAddonText", "(Ljava/lang/String;)Ljava/lang/String;") : nullptr;
        jstring request = method ? env->NewStringUTF(url) : nullptr;
        jstring text = request ? static_cast<jstring>(env->CallObjectMethod(activity, method, request)) : nullptr;
        if (!env->ExceptionCheck() && text)
        {
            const char *utf8 = env->GetStringUTFChars(text, nullptr);
            if (utf8)
            {
                size_t length = strlen(utf8);
                result = static_cast<char *>(malloc(length + 1));
                if (result) memcpy(result, utf8, length + 1);
                env->ReleaseStringUTFChars(text, utf8);
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (text) env->DeleteLocalRef(text);
        if (request) env->DeleteLocalRef(request);
        if (type) env->DeleteLocalRef(type);
        env->DeleteLocalRef(activity);
    }
    return result;
}

extern "C" qboolean IW_Android_DownloadFile(const char *url, const char *destination)
{
    JNIEnv *env = IW_GetJNIEnv();
    jobject activity = env ? static_cast<jobject>(SDL_AndroidGetActivity()) : nullptr;
    qboolean ok = false;
    if (activity)
    {
        jclass type = env->GetObjectClass(activity);
        jmethodID method = type ? env->GetMethodID(type, "downloadAddonFile", "(Ljava/lang/String;Ljava/lang/String;)Z") : nullptr;
        jstring request = method ? env->NewStringUTF(url) : nullptr;
        jstring target = method ? env->NewStringUTF(destination) : nullptr;
        if (request && target)
            ok = env->CallBooleanMethod(activity, method, request, target) == JNI_TRUE;
        if (env->ExceptionCheck()) { env->ExceptionClear(); ok = false; }
        if (target) env->DeleteLocalRef(target);
        if (request) env->DeleteLocalRef(request);
        if (type) env->DeleteLocalRef(type);
        env->DeleteLocalRef(activity);
    }
    return ok;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeAddonDownloadProgress(JNIEnv *, jclass, jlong bytes)
{
    Modlist_AndroidDownloadProgress(bytes > INT_MAX ? INT_MAX : static_cast<int>(bytes));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeAddonDownloadCancelled(JNIEnv *, jclass)
{
    return Modlist_AndroidDownloadCancelled() ? JNI_TRUE : JNI_FALSE;
}

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
Java_com_ermac_ironwail_GLES3JNIActivity_nativeCommand(JNIEnv *env, jclass, jstring command)
{
    if (!command) return;
    const char *text = env->GetStringUTFChars(command, nullptr);
    if (text) { IW_Android_Command(text); env->ReleaseStringUTFChars(command, text); }
}

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeAction(JNIEnv *, jclass, jint action, jboolean down)
{
    IW_Android_Action(action, down != 0);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeKey(JNIEnv *, jclass, jint keycode, jboolean down)
{
    IW_Android_Key(keycode, down != 0);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeText(JNIEnv *env, jclass, jstring text)
{
    if (!text) return;
    const char *utf8 = env->GetStringUTFChars(text, nullptr);
    if (utf8) { IW_Android_Text(utf8); env->ReleaseStringUTFChars(text, utf8); }
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
Java_com_ermac_ironwail_GLES3JNIActivity_nativeTouchPointer(JNIEnv *, jclass, jint action, jint pointer_id, jfloat x, jfloat y)
{ IW_Android_TouchPointer(action, pointer_id, x, y); }

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeContextRestored(JNIEnv *, jclass)
{ IW_Android_ContextRestored(); }

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeSurfaceDestroyed(JNIEnv *, jclass)
{ IW_Android_SurfaceDestroyed(); }

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeLook(JNIEnv *, jclass, jint delta_x, jint delta_y)
{ IW_Android_Look(delta_x, delta_y); }

extern "C" JNIEXPORT jint JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeScreenMode(JNIEnv *, jclass)
{ return IW_Android_ScreenMode(); }

extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3JNIActivity_nativeAudioFocus(JNIEnv *, jclass, jboolean focused)
{
    IW_Android_AudioFocus(focused != 0);
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
