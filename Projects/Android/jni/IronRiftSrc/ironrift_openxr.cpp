#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "xr_virtual_screen.h"
#include "xr_virtual_environment.h"
#include <pthread.h>
#include <unistd.h>
#include <atomic>
#include <vector>
#include <cmath>
#include <cctype>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cctype>

#include "android_lifecycle.h"
#include "xr_action_schema.h"
#ifndef XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME
#define XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME "XR_FB_display_refresh_rate"
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateDisplayRefreshRatesFB)(XrSession session, uint32_t displayRefreshRateCapacityInput, uint32_t *displayRefreshRateCountOutput, float *displayRefreshRates);
typedef XrResult (XRAPI_PTR *PFN_xrGetDisplayRefreshRateFB)(XrSession session, float *displayRefreshRate);
typedef XrResult (XRAPI_PTR *PFN_xrRequestDisplayRefreshRateFB)(XrSession session, float displayRefreshRate);
#endif

#ifndef GL_FRAMEBUFFER_TEXTURE_MULTIVIEW_OVR
typedef void (GL_APIENTRYP PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC)(GLenum, GLenum, GLuint, GLint, GLint, GLsizei);
#endif

#define XR_TAG "IronRiftXR"
#define XR_LOG(...) __android_log_print(ANDROID_LOG_INFO, XR_TAG, __VA_ARGS__)
#define XR_ERR(...) __android_log_print(ANDROID_LOG_ERROR, XR_TAG, __VA_ARGS__)

struct IronRiftXRHost {
    JavaVM *vm = nullptr;
    jobject activity = nullptr;
    pthread_t thread{};
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    ANativeWindow *window = nullptr;
    bool surface_changed = false;
    bool paused = true;
    bool focused = false;
    bool activity_paused = true;
    bool audio_focused = false;
    bool surface_ready = false;
    bool stopping = false;
    bool started = false;
    bool initialized = false;
    bool engine_paused = false;
};

struct AndroidXRRuntime;
static IronRiftXRHost g_host;
static AndroidXRRuntime *g_haptic_runtime = nullptr;
static XrSession g_active_session = XR_NULL_HANDLE;
static std::string g_base_dir = ".";

extern "C" void Modlist_AndroidDownloadProgress(int bytes);
extern "C" qboolean Modlist_AndroidDownloadCancelled(void);

static JNIEnv *IW_GetJNIEnv(bool *detach)
{
    JNIEnv *env = nullptr;
    *detach = false;
    if (!g_host.vm)
        return nullptr;
    if (g_host.vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
    {
        if (g_host.vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return nullptr;
        *detach = true;
    }
    return env;
}

static jobject IW_GetActivity(JNIEnv *env)
{
    jobject activity;
    pthread_mutex_lock(&g_host.mutex);
    activity = g_host.activity ? env->NewLocalRef(g_host.activity) : nullptr;
    pthread_mutex_unlock(&g_host.mutex);
    return activity;
}

extern "C" char *IW_Android_DownloadText(const char *url)
{
    bool detach;
    JNIEnv *env = IW_GetJNIEnv(&detach);
    jobject activity = env ? IW_GetActivity(env) : nullptr;
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
    if (detach) g_host.vm->DetachCurrentThread();
    return result;
}

extern "C" qboolean IW_Android_DownloadFile(const char *url, const char *destination)
{
    bool detach;
    JNIEnv *env = IW_GetJNIEnv(&detach);
    jobject activity = env ? IW_GetActivity(env) : nullptr;
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
    if (detach) g_host.vm->DetachCurrentThread();
    return ok;
}
extern "C" JNIEXPORT void JNICALL
Java_com_ermac_ironwail_GLES3OpenXRActivity_nativeAddonDownloadProgress(JNIEnv *, jobject, jlong bytes)
{
    Modlist_AndroidDownloadProgress(bytes > INT_MAX ? INT_MAX : static_cast<int>(bytes));
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_ermac_ironwail_GLES3OpenXRActivity_nativeAddonDownloadCancelled(JNIEnv *, jobject)
{
    return Modlist_AndroidDownloadCancelled() ? JNI_TRUE : JNI_FALSE;
}
static bool XR_Ok(XrResult result, const char *what) {
    if (result == XR_SUCCESS) return true;
    XR_ERR("%s failed: %d", what, (int)result);
    return false;
}

struct AndroidXREyeTarget {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    GLuint fbo = 0;
    uint32_t image_index = 0;
    bool acquired = false;
    int width = 0;
    int height = 0;
};

struct AndroidXRMultiviewTarget {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> fbos;
    std::vector<GLuint> overlay_fbos;
    std::vector<GLuint> depth_textures;
    uint32_t image_index = 0;
    bool acquired = false;
    bool capable = false;
    int width = 0;
    int height = 0;
};
struct AndroidXRRuntime {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace view_space = XR_NULL_HANDLE;
    XrSpace screen_space = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    XrTime last_predicted = 0;
    bool running = false;
    bool cylinder_supported = false;
    bool color_space_supported = false;
    bool refresh_rate_supported = false;
    float requested_refresh_rate = -1.f;
    float applied_refresh_rate = 0.f;
    PFN_xrEnumerateDisplayRefreshRatesFB enumerate_display_refresh_rates = nullptr;
    PFN_xrGetDisplayRefreshRateFB get_display_refresh_rate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB request_display_refresh_rate = nullptr;
    XrTime last_pacing_predicted = 0;
    uint32_t pacing_frame_count = 0;
    iw_xr_virtual_screen_follow_t screen_follow{};
    iw_xr_virtual_screen_follow_t hud_follow{};
    bool ready = false;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    GLuint fbo = 0;
    bool swapchain_is_srgb = false;
    bool virtual_environment_rendered = false;
    AndroidXREyeTarget eyes[2];
    AndroidXRMultiviewTarget multiview;
    PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC framebuffer_texture_multiview_ovr = nullptr;
    bool multiview_rendered = false;
    int multiview_last_log_state = -1;
    AndroidXREyeTarget pointer;
    bool pointer_active = false;
    float pointer_start[3] = {}, pointer_hit[3] = {};
    unsigned pointer_color = 0xffffff;
    float pointer_alpha = 0.4f, pointer_width = 2.f;
    XrView located_views[2]{};
    uint32_t located_view_count = 0;
    int width = 0;
    int height = 0;
    float eye_scale = 1.f;
    int eye_base_width[2] = {};
    int eye_base_height[2] = {};
    int64_t eye_format = 0;
    XrActionSet action_set = XR_NULL_HANDLE;
    XrAction trigger = XR_NULL_HANDLE;
    XrAction jump = XR_NULL_HANDLE;
    XrAction back = XR_NULL_HANDLE;
    XrAction haptic = XR_NULL_HANDLE;
    XrAction trigger_click = XR_NULL_HANDLE, grip = XR_NULL_HANDLE, grip_click = XR_NULL_HANDLE, stick = XR_NULL_HANDLE, stick_click = XR_NULL_HANDLE, primary = XR_NULL_HANDLE, secondary = XR_NULL_HANDLE, menu = XR_NULL_HANDLE;
    XrAction aim_pose = XR_NULL_HANDLE, grip_pose = XR_NULL_HANDLE;
    XrSpace aim_spaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace grip_spaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrPath left_hand = XR_NULL_PATH;
    XrPath right_hand = XR_NULL_PATH;
    XrPath trigger_left = XR_NULL_PATH;
    XrPath trigger_right = XR_NULL_PATH;
    XrPath a_path = XR_NULL_PATH;
    XrPath b_path = XR_NULL_PATH;
    XrPath menu_path = XR_NULL_PATH;
    bool trigger_down = false;
    bool jump_down = false;
};

static std::vector<std::string> BuildEngineArgs()
{
    std::vector<std::string> args = { "ironwail", "-basedir", g_base_dir, "-condebug", "-nojoy" };
    std::ifstream file(g_base_dir + "/commandline.txt");
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    bool first = true;
    for (size_t cursor = 0; cursor < text.size();) {
        while (cursor < text.size() && std::isspace((unsigned char)text[cursor])) ++cursor;
        if (cursor == text.size()) break;
        const char quote = text[cursor] == '"' ? text[cursor++] : 0;
        std::string token;
        while (cursor < text.size() && (quote ? text[cursor] != quote : !std::isspace((unsigned char)text[cursor]))) token += text[cursor++];
        if (quote && cursor < text.size() && text[cursor] == quote) ++cursor;
        if (first && token == "ironwail") { first = false; continue; }
        first = false;
        if (!token.empty()) args.push_back(token);
    }
    if (!text.empty()) XR_LOG("Android command line loaded tokens=%u", (unsigned)args.size());
    return args;
}
extern "C" void IW_Android_NativeHaptic(int hand, float amplitude, float duration_seconds) { if (!g_haptic_runtime || !g_haptic_runtime->session || !g_haptic_runtime->haptic || hand < 0 || hand > 1) return; XrHapticActionInfo info{}; info.type = XR_TYPE_HAPTIC_ACTION_INFO; info.action = g_haptic_runtime->haptic; info.subactionPath = hand == 0 ? g_haptic_runtime->left_hand : g_haptic_runtime->right_hand; if (amplitude <= 0.f || duration_seconds <= 0.f) { xrStopHapticFeedback(g_haptic_runtime->session, &info); return; } XrHapticVibration vibration{}; vibration.type = XR_TYPE_HAPTIC_VIBRATION; vibration.amplitude = amplitude > 1.f ? 1.f : amplitude; vibration.duration = (XrDuration)(duration_seconds * 1000000000.0f); vibration.frequency = XR_FREQUENCY_UNSPECIFIED; xrApplyHapticFeedback(g_haptic_runtime->session, &info, reinterpret_cast<const XrHapticBaseHeader *>(&vibration)); }
static bool CreateEGL(AndroidXRRuntime &xr) {
    xr.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (xr.display == EGL_NO_DISPLAY || !eglInitialize(xr.display, nullptr, nullptr)) return false;
    const EGLint attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE };
    EGLint count = 0;
    if (!eglChooseConfig(xr.display, attrs, &xr.config, 1, &count) || count != 1) return false;
    const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return false;
    xr.context = eglCreateContext(xr.display, xr.config, EGL_NO_CONTEXT, context_attrs);
    const EGLint surface_attrs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    xr.surface = eglCreatePbufferSurface(xr.display, xr.config, surface_attrs);
    if (xr.context == EGL_NO_CONTEXT || xr.surface == EGL_NO_SURFACE) return false;
    if (!eglMakeCurrent(xr.display, xr.surface, xr.surface, xr.context)) return false;
    XR_LOG("EGL GLES context ready vendor=%s version=%s", glGetString(GL_VENDOR), glGetString(GL_VERSION));
    return true;
}
static bool CreateInstance(AndroidXRRuntime &xr) {
    PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
    if (xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction *>(&initialize_loader)) == XR_SUCCESS && initialize_loader) {
        XrLoaderInitInfoAndroidKHR loader_info{};
        loader_info.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
        loader_info.applicationVM = g_host.vm;
        loader_info.applicationContext = g_host.activity;
        XR_Ok(initialize_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loader_info)), "xrInitializeLoaderKHR");
    }
    uint32_t extension_count = 0;
    if (!XR_Ok(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr), "xrEnumerateInstanceExtensionProperties")) return false;
    std::vector<XrExtensionProperties> available(extension_count);
    for (auto &extension : available) extension.type = XR_TYPE_EXTENSION_PROPERTIES;
    if (!XR_Ok(xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, available.data()), "xrEnumerateInstanceExtensionProperties data")) return false;
    auto has_extension = [&](const char *name) { for (const auto &extension : available) if (!std::strcmp(extension.extensionName, name)) return true; return false; };
    if (!has_extension(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) { XR_ERR("Runtime lacks XR_KHR_opengl_es_enable"); return false; }
    std::vector<const char *> extensions;
    extensions.push_back(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
    bool has_android_instance = has_extension(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
    if (!has_android_instance) { XR_ERR("Runtime lacks XR_KHR_android_create_instance"); return false; }
    xr.cylinder_supported = has_extension(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    xr.color_space_supported = has_extension(XR_FB_COLOR_SPACE_EXTENSION_NAME);
    xr.refresh_rate_supported = has_extension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
    if (xr.cylinder_supported) extensions.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    if (xr.color_space_supported) extensions.push_back(XR_FB_COLOR_SPACE_EXTENSION_NAME);
    if (xr.refresh_rate_supported) extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    XR_LOG("OpenXR extensions: GLES=%d AndroidInstance=%d Cylinder=%d ColorSpace=%d RefreshRate=%d", 1, has_android_instance ? 1 : 0, xr.cylinder_supported ? 1 : 0, xr.color_space_supported ? 1 : 0, xr.refresh_rate_supported ? 1 : 0);
    XrInstanceCreateInfoAndroidKHR android_info{};
    android_info.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    android_info.applicationVM = g_host.vm;
    android_info.applicationActivity = g_host.activity;
    XrInstanceCreateInfo info{};
    info.type = XR_TYPE_INSTANCE_CREATE_INFO;
    info.next = &android_info;
    std::strncpy(info.applicationInfo.applicationName, "Ironwail", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(info.applicationInfo.engineName, "Ironwail", XR_MAX_ENGINE_NAME_SIZE - 1);
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.enabledExtensionNames = extensions.data();
    return XR_Ok(xrCreateInstance(&info, &xr.instance), "xrCreateInstance");
}
static void ConfigureRefreshRate(AndroidXRRuntime &xr)
{
    float rates[16] = {}, current = 0.f, requested = IW_Android_GetXRRefreshRate();
    uint32_t count = 0, i;
    if (!xr.refresh_rate_supported || xr.session == XR_NULL_HANDLE) return;
    if (!xr.enumerate_display_refresh_rates) xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRefreshRatesFB", reinterpret_cast<PFN_xrVoidFunction *>(&xr.enumerate_display_refresh_rates));
    if (!xr.get_display_refresh_rate) xrGetInstanceProcAddr(xr.instance, "xrGetDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction *>(&xr.get_display_refresh_rate));
    if (!xr.request_display_refresh_rate) xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction *>(&xr.request_display_refresh_rate));
    if (!xr.enumerate_display_refresh_rates || !xr.request_display_refresh_rate) { xr.refresh_rate_supported = false; XR_LOG("refresh-rate extension procedures unavailable; runtime default retained"); return; }
    if (requested <= 0.f) return;
    if (xr.requested_refresh_rate == requested) return;
    xr.requested_refresh_rate = requested;
    if (xr.enumerate_display_refresh_rates(xr.session, (uint32_t)(sizeof(rates) / sizeof(rates[0])), &count, rates) != XR_SUCCESS || !count) { XR_LOG("refresh-rate query failed; runtime default retained"); return; }
    count = std::min<uint32_t>(count, (uint32_t)(sizeof(rates) / sizeof(rates[0])));
    std::string supported;
    for (i = 0; i < count; ++i) { char value[16]; std::snprintf(value, sizeof(value), "%s%.0f", i ? "," : "", rates[i]); supported += value; }
    XR_LOG("OpenXR refresh rates supported=%s requested=%.0f", supported.c_str(), requested);
    for (i = 0; i < count; ++i) if (std::fabs(rates[i] - requested) < 0.01f) break;
    if (i == count) { XR_LOG("OpenXR refresh rate %.0f unsupported; runtime default retained", requested); return; }
    if (xr.request_display_refresh_rate(xr.session, rates[i]) == XR_SUCCESS) { xr.applied_refresh_rate = rates[i]; XR_LOG("OpenXR requested refresh rate %.0f Hz", rates[i]); }
    else XR_LOG("OpenXR refresh-rate request %.0f Hz failed", rates[i]);
    if (xr.get_display_refresh_rate && xr.get_display_refresh_rate(xr.session, &current) == XR_SUCCESS) XR_LOG("OpenXR current refresh rate %.0f Hz", current);
}
static bool CreateMultiviewTarget(AndroidXRRuntime &xr);
static bool CreateSession(AndroidXRRuntime &xr) {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR get_requirements = nullptr;
    if (!XR_Ok(xrGetInstanceProcAddr(xr.instance, "xrGetOpenGLESGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction *>(&get_requirements)), "xrGetOpenGLESGraphicsRequirementsKHR") || !get_requirements) return false;
    XrSystemGetInfo requirement_system{}; requirement_system.type = XR_TYPE_SYSTEM_GET_INFO; requirement_system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId requirement_id = XR_NULL_SYSTEM_ID; if (!XR_Ok(xrGetSystem(xr.instance, &requirement_system, &requirement_id), "xrGetSystem requirements")) return false;
    XrGraphicsRequirementsOpenGLESKHR requirements{}; requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR; if (!XR_Ok(get_requirements(xr.instance, requirement_id, &requirements), "xrGetOpenGLESGraphicsRequirementsKHR call")) return false;
    XR_LOG("OpenXR GLES requirements min=%u.%u max=%u.%u", XR_VERSION_MAJOR(requirements.minApiVersionSupported), XR_VERSION_MINOR(requirements.minApiVersionSupported), XR_VERSION_MAJOR(requirements.maxApiVersionSupported), XR_VERSION_MINOR(requirements.maxApiVersionSupported));
    XrSystemGetInfo system_info{};
    system_info.type = XR_TYPE_SYSTEM_GET_INFO;
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!XR_Ok(xrGetSystem(xr.instance, &system_info, &xr.system), "xrGetSystem")) return false;
    XrGraphicsBindingOpenGLESAndroidKHR binding{};
    binding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
    binding.display = xr.display;
    binding.config = xr.config;
    binding.context = xr.context;
    XrSessionCreateInfo session_info{};
    session_info.type = XR_TYPE_SESSION_CREATE_INFO;
    session_info.next = &binding;
    session_info.systemId = xr.system;
    if (!XR_Ok(xrCreateSession(xr.instance, &session_info, &xr.session), "xrCreateSession")) return false; pthread_mutex_lock(&g_host.mutex); g_active_session = xr.session; pthread_mutex_unlock(&g_host.mutex);
    ConfigureRefreshRate(xr);
    if (xr.color_space_supported) {
        PFN_xrEnumerateColorSpacesFB enumerate_colorspaces = nullptr;
        PFN_xrSetColorSpaceFB set_colorspace = nullptr;
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateColorSpacesFB", reinterpret_cast<PFN_xrVoidFunction *>(&enumerate_colorspaces));
        xrGetInstanceProcAddr(xr.instance, "xrSetColorSpaceFB", reinterpret_cast<PFN_xrVoidFunction *>(&set_colorspace));
        if (enumerate_colorspaces && set_colorspace) {
        uint32_t count = 0;
        if (XR_Ok(enumerate_colorspaces(xr.session, 0, &count, nullptr), "xrEnumerateColorSpacesFB count") && count) {
            std::vector<XrColorSpaceFB> spaces(count);
            if (XR_Ok(enumerate_colorspaces(xr.session, count, &count, spaces.data()), "xrEnumerateColorSpacesFB data")) {
                bool rec2020 = false;
                for (XrColorSpaceFB space : spaces) if (space == XR_COLOR_SPACE_REC2020_FB) rec2020 = true;
                if (rec2020) { XR_Ok(set_colorspace(xr.session, XR_COLOR_SPACE_REC2020_FB), "xrSetColorSpaceFB REC2020"); XR_LOG("OpenXR color space: REC2020"); }
            }
        }
        }
    }
    XrReferenceSpaceCreateInfo space_info{};
    space_info.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    space_info.poseInReferenceSpace.orientation.w = 1.f;
    if (!XR_Ok(xrCreateReferenceSpace(xr.session, &space_info, &xr.view_space), "xrCreateReferenceSpace VIEW")) return false;
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!XR_Ok(xrCreateReferenceSpace(xr.session, &space_info, &xr.screen_space), "xrCreateReferenceSpace LOCAL")) return false;

    uint32_t count = 0;
    if (!XR_Ok(xrEnumerateViewConfigurationViews(xr.instance, xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr), "xrEnumerateViewConfigurationViews")) return false;
    std::vector<XrViewConfigurationView> views(count);
    for (auto &v : views) { v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW; }
    if (!XR_Ok(xrEnumerateViewConfigurationViews(xr.instance, xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, views.data()), "xrEnumerateViewConfigurationViews data")) return false;
    xr.width = 1024; xr.height = 768;
    if (xr.width <= 0 || xr.height <= 0) return false;
    uint32_t formats = 0;
    xrEnumerateSwapchainFormats(xr.session, 0, &formats, nullptr);
    std::vector<int64_t> available(formats);
    xrEnumerateSwapchainFormats(xr.session, formats, &formats, available.data());
    int64_t chosen = GL_SRGB8_ALPHA8;
    bool found = false;
    for (int64_t f : available) if (f == chosen) found = true;
    if (!found) { chosen = GL_RGBA8; for (int64_t f : available) if (f == chosen) found = true; }
    if (!found) return false;
    xr.swapchain_is_srgb = chosen == GL_SRGB8_ALPHA8;
    xr.eye_format = chosen;
    xr.eye_scale = IW_Android_GetXRRenderScale();
    XrSwapchainCreateInfo swap_info{};
    swap_info.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    swap_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swap_info.format = chosen;
    swap_info.sampleCount = 1;
    swap_info.width = xr.width;
    swap_info.height = xr.height;
    swap_info.faceCount = 1;
    swap_info.arraySize = 1;
    swap_info.mipCount = 1;
    if (!XR_Ok(xrCreateSwapchain(xr.session, &swap_info, &xr.swapchain), "xrCreateSwapchain")) return false;
    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(xr.swapchain, 0, &image_count, nullptr);
    xr.images.resize(image_count);
    for (auto &image : xr.images) image.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    if (!XR_Ok(xrEnumerateSwapchainImages(xr.swapchain, image_count, &image_count, reinterpret_cast<XrSwapchainImageBaseHeader *>(xr.images.data())), "xrEnumerateSwapchainImages")) return false;
    glGenFramebuffers(1, &xr.fbo);
    if (count < 2) return false;
    for (unsigned eye = 0; eye < 2; ++eye)
    {
        AndroidXREyeTarget &target = xr.eyes[eye];
        xr.eye_base_width[eye] = (int)views[eye].recommendedImageRectWidth;
        xr.eye_base_height[eye] = (int)views[eye].recommendedImageRectHeight;
        target.width = std::max(1, (int)(xr.eye_base_width[eye] * IW_Android_GetXRRenderScale() + 0.5f));
        target.height = std::max(1, (int)(xr.eye_base_height[eye] * IW_Android_GetXRRenderScale() + 0.5f));
        if (target.width <= 0 || target.height <= 0) return false;
        XrSwapchainCreateInfo eye_info = swap_info;
        eye_info.width = target.width;
        eye_info.height = target.height;
        if (!XR_Ok(xrCreateSwapchain(xr.session, &eye_info, &target.swapchain), "xrCreateSwapchain eye")) return false;
        uint32_t eye_count = 0;
        if (!XR_Ok(xrEnumerateSwapchainImages(target.swapchain, 0, &eye_count, nullptr), "xrEnumerateSwapchainImages eye count")) return false;
        target.images.resize(eye_count);
        for (auto &image : target.images) image.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        if (!XR_Ok(xrEnumerateSwapchainImages(target.swapchain, eye_count, &eye_count, reinterpret_cast<XrSwapchainImageBaseHeader *>(target.images.data())), "xrEnumerateSwapchainImages eye")) return false;
        glGenFramebuffers(1, &target.fbo);
    }
    CreateMultiviewTarget(xr);
    xr.pointer.width = 256; xr.pointer.height = 64;
    XrSwapchainCreateInfo pointer_info = swap_info; pointer_info.width = xr.pointer.width; pointer_info.height = xr.pointer.height;
    if (!XR_Ok(xrCreateSwapchain(xr.session, &pointer_info, &xr.pointer.swapchain), "xrCreateSwapchain pointer")) return false;
    uint32_t pointer_count = 0; if (!XR_Ok(xrEnumerateSwapchainImages(xr.pointer.swapchain, 0, &pointer_count, nullptr), "xrEnumerateSwapchainImages pointer count")) return false;
    xr.pointer.images.resize(pointer_count); for (auto &image : xr.pointer.images) image.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    if (!XR_Ok(xrEnumerateSwapchainImages(xr.pointer.swapchain, pointer_count, &pointer_count, reinterpret_cast<XrSwapchainImageBaseHeader *>(xr.pointer.images.data())), "xrEnumerateSwapchainImages pointer")) return false;
    glGenFramebuffers(1, &xr.pointer.fbo);
    XR_LOG("stereo eye swapchains %dx%d,%dx%d format=%lld srgb=%d", xr.eyes[0].width, xr.eyes[0].height, xr.eyes[1].width, xr.eyes[1].height, (long long)chosen, chosen == GL_SRGB8_ALPHA8 ? 1 : 0);
    XR_LOG("mono virtual-screen swapchain %dx%d (desktop reference resolution) format=%lld srgb=%d images=%u", xr.width, xr.height, (long long)chosen, chosen == GL_SRGB8_ALPHA8 ? 1 : 0, image_count);
    return true;
}

static void DestroyMultiviewTarget(AndroidXRRuntime &xr)
{
    auto &target = xr.multiview;
    if (!target.fbos.empty()) glDeleteFramebuffers((GLsizei)target.fbos.size(), target.fbos.data());
    if (!target.overlay_fbos.empty()) glDeleteFramebuffers((GLsizei)target.overlay_fbos.size(), target.overlay_fbos.data());
    if (!target.depth_textures.empty()) glDeleteTextures((GLsizei)target.depth_textures.size(), target.depth_textures.data());
    if (target.swapchain) xrDestroySwapchain(target.swapchain);
    target = AndroidXRMultiviewTarget{};
    xr.multiview_rendered = false;
}

static bool CreateMultiviewTarget(AndroidXRRuntime &xr)
{
    DestroyMultiviewTarget(xr);
    xr.framebuffer_texture_multiview_ovr = reinterpret_cast<PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC>(eglGetProcAddress("glFramebufferTextureMultiviewOVR"));
    if (!xr.framebuffer_texture_multiview_ovr) { XR_LOG("multiview disabled: glFramebufferTextureMultiviewOVR unavailable"); return false; }
    if (xr.eyes[0].width != xr.eyes[1].width || xr.eyes[0].height != xr.eyes[1].height) { XR_LOG("multiview disabled: asymmetric eye target sizes"); return false; }
    auto &target = xr.multiview;
    target.width = xr.eyes[0].width;
    target.height = xr.eyes[0].height;
    XrSwapchainCreateInfo info{};
    info.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = xr.eye_format;
    info.sampleCount = 1;
    info.width = target.width;
    info.height = target.height;
    info.faceCount = 1;
    info.arraySize = 2;
    info.mipCount = 1;
    if (!XR_Ok(xrCreateSwapchain(xr.session, &info, &target.swapchain), "xrCreateSwapchain multiview")) { DestroyMultiviewTarget(xr); return false; }
    uint32_t count = 0;
    if (!XR_Ok(xrEnumerateSwapchainImages(target.swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages multiview count")) { DestroyMultiviewTarget(xr); return false; }
    target.images.resize(count);
    for (auto &image : target.images) image.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    if (!XR_Ok(xrEnumerateSwapchainImages(target.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(target.images.data())), "xrEnumerateSwapchainImages multiview")) { DestroyMultiviewTarget(xr); return false; }
    target.fbos.resize(count);
    target.overlay_fbos.resize(count * 2);
    target.depth_textures.resize(count);
    glGenFramebuffers((GLsizei)count, target.fbos.data());
    glGenFramebuffers((GLsizei)target.overlay_fbos.size(), target.overlay_fbos.data());
    glGenTextures((GLsizei)count, target.depth_textures.data());
    for (uint32_t i = 0; i < count; ++i) {
        glBindTexture(GL_TEXTURE_2D_ARRAY, target.depth_textures[i]);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH24_STENCIL8, target.width, target.height, 2);
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbos[i]);
        xr.framebuffer_texture_multiview_ovr(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target.images[i].image, 0, 0, 2);
        xr.framebuffer_texture_multiview_ovr(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, target.depth_textures[i], 0, 0, 2);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) { XR_LOG("multiview disabled: FBO status=0x%x", status); glBindFramebuffer(GL_FRAMEBUFFER, 0); DestroyMultiviewTarget(xr); return false; }        for (uint32_t eye = 0; eye < 2; ++eye) {
            glBindFramebuffer(GL_FRAMEBUFFER, target.overlay_fbos[i * 2 + eye]);
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target.images[i].image, 0, eye);
            status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) { XR_LOG("multiview overlay disabled: FBO status=0x%x", status); glBindFramebuffer(GL_FRAMEBUFFER, 0); DestroyMultiviewTarget(xr); return false; }
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    target.capable = true;
    XR_LOG("multiview target ready size=%dx%d images=%u", target.width, target.height, count);
    return true;
}
static bool ResizeEyeTargets(AndroidXRRuntime &xr)
{
    float scale = IW_Android_GetXRRenderScale();
    if (std::fabs(scale - xr.eye_scale) < 0.001f) return true;
    for (auto &target : xr.eyes) { if (target.fbo) { glDeleteFramebuffers(1, &target.fbo); target.fbo = 0; } if (target.swapchain) { xrDestroySwapchain(target.swapchain); target.swapchain = XR_NULL_HANDLE; } target.images.clear(); }
    for (unsigned eye = 0; eye < 2; ++eye) {
        AndroidXREyeTarget &target = xr.eyes[eye];
        target.width = std::max(1, (int)(xr.eye_base_width[eye] * scale + 0.5f));
        target.height = std::max(1, (int)(xr.eye_base_height[eye] * scale + 0.5f));
        XrSwapchainCreateInfo info{}; info.type = XR_TYPE_SWAPCHAIN_CREATE_INFO; info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT; info.format = xr.eye_format; info.sampleCount = 1; info.width = target.width; info.height = target.height; info.faceCount = 1; info.arraySize = 1; info.mipCount = 1;
        if (!XR_Ok(xrCreateSwapchain(xr.session, &info, &target.swapchain), "xrCreateSwapchain resized eye")) return false;
        uint32_t count = 0; if (!XR_Ok(xrEnumerateSwapchainImages(target.swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages resized eye count")) return false;
        target.images.resize(count); for (auto &image : target.images) image.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        if (!XR_Ok(xrEnumerateSwapchainImages(target.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(target.images.data())), "xrEnumerateSwapchainImages resized eye")) return false;
        glGenFramebuffers(1, &target.fbo);
    }
    CreateMultiviewTarget(xr);
    xr.eye_scale = scale;
    XR_LOG("stereo eye swapchains resized scale=%.2f %dx%d,%dx%d", scale, xr.eyes[0].width, xr.eyes[0].height, xr.eyes[1].width, xr.eyes[1].height);
    return true;
}
static void DestroyRuntime(AndroidXRRuntime &xr) {
    if (g_haptic_runtime == &xr) g_haptic_runtime = nullptr;
    pthread_mutex_lock(&g_host.mutex); if (g_active_session == xr.session) g_active_session = XR_NULL_HANDLE; pthread_mutex_unlock(&g_host.mutex);
    DestroyMultiviewTarget(xr);
    for (auto &eye : xr.eyes) { if (eye.fbo) glDeleteFramebuffers(1, &eye.fbo); if (eye.swapchain) xrDestroySwapchain(eye.swapchain); }
    if (xr.pointer.fbo) glDeleteFramebuffers(1, &xr.pointer.fbo); if (xr.pointer.swapchain) xrDestroySwapchain(xr.pointer.swapchain);
    if (xr.fbo) glDeleteFramebuffers(1, &xr.fbo);
    if (xr.swapchain) xrDestroySwapchain(xr.swapchain);
    if (xr.view_space) xrDestroySpace(xr.view_space);
    for (int i = 0; i < 2; ++i) { if (xr.aim_spaces[i]) xrDestroySpace(xr.aim_spaces[i]); if (xr.grip_spaces[i]) xrDestroySpace(xr.grip_spaces[i]); }
    if (xr.screen_space) xrDestroySpace(xr.screen_space);
    if (xr.session) xrDestroySession(xr.session);
    if (xr.instance) xrDestroyInstance(xr.instance);
    if (xr.display != EGL_NO_DISPLAY) { eglMakeCurrent(xr.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); if (xr.surface != EGL_NO_SURFACE) eglDestroySurface(xr.display, xr.surface); if (xr.context != EGL_NO_CONTEXT) eglDestroyContext(xr.display, xr.context); eglTerminate(xr.display); }
    xr = AndroidXRRuntime{};
}

static bool CreateAction(AndroidXRRuntime &xr, XrActionType type, const char *name, const char *localized, XrPath *hands, XrAction *out) { XrActionCreateInfo info{}; info.type = XR_TYPE_ACTION_CREATE_INFO; info.actionType = type; std::strcpy(info.actionName, name); std::strcpy(info.localizedActionName, localized); info.countSubactionPaths = 2; info.subactionPaths = hands; return XR_Ok(xrCreateAction(xr.action_set, &info, out), name); }
static bool SetupActions(AndroidXRRuntime &xr) {
    XrActionSetCreateInfo set{}; set.type = XR_TYPE_ACTION_SET_CREATE_INFO; std::strcpy(set.actionSetName, "gameplay"); std::strcpy(set.localizedActionSetName, "Gameplay"); if (!XR_Ok(xrCreateActionSet(xr.instance, &set, &xr.action_set), "xrCreateActionSet")) return false;
    XrPath hands[2]; xrStringToPath(xr.instance, "/user/hand/left", &hands[0]); xrStringToPath(xr.instance, "/user/hand/right", &hands[1]); xr.left_hand = hands[0]; xr.right_hand = hands[1];
    if (!CreateAction(xr, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim Pose", hands, &xr.aim_pose) || !CreateAction(xr, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip Pose", hands, &xr.grip_pose) || !CreateAction(xr, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", hands, &xr.trigger) || !CreateAction(xr, XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger_click", "Trigger Click", hands, &xr.trigger_click) || !CreateAction(xr, XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip", hands, &xr.grip) || !CreateAction(xr, XR_ACTION_TYPE_BOOLEAN_INPUT, "grip_click", "Grip Click", hands, &xr.grip_click) || !CreateAction(xr, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbstick", hands, &xr.stick) || !CreateAction(xr, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click", "Thumbstick Click", hands, &xr.stick_click) || !CreateAction(xr, XR_ACTION_TYPE_BOOLEAN_INPUT, "primary", "Primary", hands, &xr.primary) || !CreateAction(xr, XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary", "Secondary", hands, &xr.secondary) || !CreateAction(xr, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", hands, &xr.menu) || !CreateAction(xr, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptic", hands, &xr.haptic)) return false;
    XrPath oculus, pico_legacy, pico_neo3, pico4; xrStringToPath(xr.instance, "/interaction_profiles/oculus/touch_controller", &oculus); xrStringToPath(xr.instance, "/interaction_profiles/pico/neo3_controller", &pico_legacy); xrStringToPath(xr.instance, "/interaction_profiles/bytedance/pico_neo3_controller_bd", &pico_neo3); xrStringToPath(xr.instance, "/interaction_profiles/bytedance/pico4_controller", &pico4);
    XrPath paths[20]; unsigned count = 0; auto add = [&](XrAction action, const char *path) { XrPath p; if (xrStringToPath(xr.instance, path, &p) == XR_SUCCESS) paths[count++] = p; };
    XrActionSuggestedBinding bindings[20]; unsigned b = 0; auto bind = [&](XrAction action, const char *path) { XrPath p; if (xrStringToPath(xr.instance, path, &p) == XR_SUCCESS) bindings[b++] = {action, p}; };
    bind(xr.aim_pose, "/user/hand/left/input/aim/pose"); bind(xr.aim_pose, "/user/hand/right/input/aim/pose"); bind(xr.grip_pose, "/user/hand/left/input/grip/pose"); bind(xr.grip_pose, "/user/hand/right/input/grip/pose"); bind(xr.trigger, "/user/hand/left/input/trigger/value"); bind(xr.trigger, "/user/hand/right/input/trigger/value"); bind(xr.grip, "/user/hand/left/input/squeeze/value"); bind(xr.grip, "/user/hand/right/input/squeeze/value"); bind(xr.stick, "/user/hand/left/input/thumbstick"); bind(xr.stick, "/user/hand/right/input/thumbstick"); bind(xr.stick_click, "/user/hand/left/input/thumbstick/click"); bind(xr.stick_click, "/user/hand/right/input/thumbstick/click"); bind(xr.primary, "/user/hand/left/input/x/click"); bind(xr.primary, "/user/hand/right/input/a/click"); bind(xr.secondary, "/user/hand/left/input/y/click"); bind(xr.secondary, "/user/hand/right/input/b/click"); bind(xr.menu, "/user/hand/left/input/menu/click"); bind(xr.haptic, "/user/hand/left/output/haptic"); bind(xr.haptic, "/user/hand/right/output/haptic");
    for (XrPath profile : {oculus, pico_legacy, pico_neo3, pico4}) { XrInteractionProfileSuggestedBinding suggested{}; suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING; suggested.interactionProfile = profile; suggested.suggestedBindings = bindings; suggested.countSuggestedBindings = b; xrSuggestInteractionProfileBindings(xr.instance, &suggested); }
    XrSessionActionSetsAttachInfo attach{}; attach.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO; attach.countActionSets = 1; attach.actionSets = &xr.action_set; if (!XR_Ok(xrAttachSessionActionSets(xr.session, &attach), "xrAttachSessionActionSets")) return false;
    for (int hand = 0; hand < 2; ++hand) { XrActionSpaceCreateInfo space{}; space.type = XR_TYPE_ACTION_SPACE_CREATE_INFO; space.subactionPath = hands[hand]; space.poseInActionSpace.orientation.w = 1.f; space.action = xr.aim_pose; if (!XR_Ok(xrCreateActionSpace(xr.session, &space, &xr.aim_spaces[hand]), "xrCreateActionSpace aim")) return false; space.action = xr.grip_pose; if (!XR_Ok(xrCreateActionSpace(xr.session, &space, &xr.grip_spaces[hand]), "xrCreateActionSpace grip")) return false; }
    XR_LOG("gameplay actions attached with controller poses, thumbsticks, buttons, and haptics"); return true;
}
static void PollEvents(AndroidXRRuntime &xr) {
    XrEventDataBuffer event{}; event.type = XR_TYPE_EVENT_DATA_BUFFER;
    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) { auto *changed = reinterpret_cast<XrEventDataSessionStateChanged *>(&event); XrSessionState previous_state = xr.state; xr.state = changed->state; XR_LOG("session state=%d running=%d", (int)xr.state, xr.running ? 1 : 0); if (xr.state == XR_SESSION_STATE_READY && !xr.running) { XrSessionBeginInfo begin{}; begin.type = XR_TYPE_SESSION_BEGIN_INFO; begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; if (XR_Ok(xrBeginSession(xr.session, &begin), "xrBeginSession")) xr.running = true; } if (previous_state == XR_SESSION_STATE_FOCUSED && xr.state != XR_SESSION_STATE_FOCUSED) { IW_Android_NativeHaptic(0, 0.f, 0.f); IW_Android_NativeHaptic(1, 0.f, 0.f); } if ((xr.state == XR_SESSION_STATE_STOPPING || xr.state == XR_SESSION_STATE_EXITING || xr.state == XR_SESSION_STATE_LOSS_PENDING) && xr.running) { xrEndSession(xr.session); xr.running = false; } }
        event = XrEventDataBuffer{}; event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

static float ActionFloat(AndroidXRRuntime &xr, XrAction action, XrPath hand) { XrActionStateGetInfo info{}; info.type = XR_TYPE_ACTION_STATE_GET_INFO; info.action = action; info.subactionPath = hand; XrActionStateFloat state{}; state.type = XR_TYPE_ACTION_STATE_FLOAT; xrGetActionStateFloat(xr.session, &info, &state); return state.isActive ? state.currentState : 0.f; }
static bool ActionBool(AndroidXRRuntime &xr, XrAction action, XrPath hand) { XrActionStateGetInfo info{}; info.type = XR_TYPE_ACTION_STATE_GET_INFO; info.action = action; info.subactionPath = hand; XrActionStateBoolean state{}; state.type = XR_TYPE_ACTION_STATE_BOOLEAN; xrGetActionStateBoolean(xr.session, &info, &state); return state.isActive && state.currentState; }
static void UpdateActions(AndroidXRRuntime &xr) {
    iw_xr_action_snapshot_t actions{}; bool focused; pthread_mutex_lock(&g_host.mutex); focused = g_host.focused; pthread_mutex_unlock(&g_host.mutex);
    if (!focused || xr.state != XR_SESSION_STATE_FOCUSED || !xr.action_set) { IW_Android_SetXRActions(&actions); return; } XrActiveActionSet active{}; active.actionSet = xr.action_set; XrActionsSyncInfo sync{}; sync.type = XR_TYPE_ACTIONS_SYNC_INFO; sync.countActiveActionSets = 1; sync.activeActionSets = &active; if (xrSyncActions(xr.session, &sync) != XR_SUCCESS) { IW_Android_SetXRActions(&actions); return; }
    for (int hand = 0; hand < 2; ++hand) { auto &out = actions.hand[hand]; XrPath path = hand == 0 ? xr.left_hand : xr.right_hand; out.trigger = ActionFloat(xr, xr.trigger, path); out.grip = ActionFloat(xr, xr.grip, path); if (out.trigger > .5f || ActionBool(xr, xr.trigger_click, path)) out.buttons |= IW_XR_BUTTON_TRIGGER; if (out.grip > .5f || ActionBool(xr, xr.grip_click, path)) out.buttons |= IW_XR_BUTTON_GRIP; if (ActionBool(xr, xr.stick_click, path)) out.buttons |= IW_XR_BUTTON_STICK; if (ActionBool(xr, xr.primary, path)) out.buttons |= IW_XR_BUTTON_PRIMARY; if (ActionBool(xr, xr.secondary, path)) out.buttons |= IW_XR_BUTTON_SECONDARY; if (ActionBool(xr, xr.menu, path)) out.buttons |= IW_XR_BUTTON_MENU;
        XrActionStateGetInfo stick_info{}; stick_info.type = XR_TYPE_ACTION_STATE_GET_INFO; stick_info.action = xr.stick; stick_info.subactionPath = path; XrActionStateVector2f stick{}; stick.type = XR_TYPE_ACTION_STATE_VECTOR2F; xrGetActionStateVector2f(xr.session, &stick_info, &stick); if (stick.isActive) { out.stick[0] = stick.currentState.x; out.stick[1] = stick.currentState.y; }
        XrSpaceLocation location{}; XrSpaceVelocity aim_velocity{}; aim_velocity.type = XR_TYPE_SPACE_VELOCITY; location.type = XR_TYPE_SPACE_LOCATION; location.next = &aim_velocity; if (xrLocateSpace(xr.aim_spaces[hand], xr.screen_space, xr.last_predicted, &location) == XR_SUCCESS && (location.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) == (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) { out.aim_valid = true; out.aim_position[0] = location.pose.position.x; out.aim_position[1] = location.pose.position.y; out.aim_position[2] = location.pose.position.z; out.aim_orientation[0] = location.pose.orientation.x; out.aim_orientation[1] = location.pose.orientation.y; out.aim_orientation[2] = location.pose.orientation.z; out.aim_orientation[3] = location.pose.orientation.w; }
        XrSpaceVelocity grip_velocity{}; grip_velocity.type = XR_TYPE_SPACE_VELOCITY; location = {}; location.type = XR_TYPE_SPACE_LOCATION; location.next = &grip_velocity; if (xrLocateSpace(xr.grip_spaces[hand], xr.screen_space, xr.last_predicted, &location) == XR_SUCCESS && (location.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) == (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) { out.grip_valid = true; out.grip_position[0] = location.pose.position.x; out.grip_position[1] = location.pose.position.y; out.grip_position[2] = location.pose.position.z; out.grip_orientation[0] = location.pose.orientation.x; out.grip_orientation[1] = location.pose.orientation.y; out.grip_orientation[2] = location.pose.orientation.z; out.grip_orientation[3] = location.pose.orientation.w; }
        const XrVector3f *velocity = nullptr; if ((grip_velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0) velocity = &grip_velocity.linearVelocity; else if ((aim_velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0) velocity = &aim_velocity.linearVelocity; if (velocity) { out.velocity_valid = true; out.linear_velocity[0] = velocity->x; out.linear_velocity[1] = velocity->y; out.linear_velocity[2] = velocity->z; }
        out.active = out.aim_valid || out.grip_valid || out.buttons != 0 || out.stick[0] != 0.f || out.stick[1] != 0.f; actions.active |= out.active;
    }
    IW_Android_SetXRActions(&actions);
}
static bool g_hud_pose_logged = false; static bool g_hud_pose_missing_logged = false;static bool LocateScreenPose(AndroidXRRuntime &xr, XrTime predicted, XrPosef *pose) {
    float shared_position[3], shared_orientation[4];
    (void)xr; (void)predicted;
    if (IW_Android_GetXRScreenPose(shared_position, shared_orientation)) {
        pose->position = {shared_position[0], shared_position[1], shared_position[2]};
        pose->orientation = {shared_orientation[0], shared_orientation[1], shared_orientation[2], shared_orientation[3]};
        return true;
    }
    XrViewLocateInfo locate{}; locate.type = XR_TYPE_VIEW_LOCATE_INFO; locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; locate.displayTime = predicted; locate.space = xr.screen_space;
    XrViewState state{}; state.type = XR_TYPE_VIEW_STATE; uint32_t capacity = 0;
    if (!XR_Ok(xrEnumerateViewConfigurationViews(xr.instance, xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &capacity, nullptr), "xrEnumerateViewConfigurationViews count") || !capacity) return false;
    std::vector<XrView> source(capacity); for (auto &view : source) view.type = XR_TYPE_VIEW; uint32_t count = 0;
    if (!XR_Ok(xrLocateViews(xr.session, &locate, &state, capacity, &count, source.data()), "xrLocateViews") || !count) return false;
    std::vector<iw_xr_virtual_screen_view_t> views(count);
    for (uint32_t i = 0; i < count; ++i) { views[i].position[0] = source[i].pose.position.x; views[i].position[1] = source[i].pose.position.y; views[i].position[2] = source[i].pose.position.z; views[i].orientation[0] = source[i].pose.orientation.x; views[i].orientation[1] = source[i].pose.orientation.y; views[i].orientation[2] = source[i].pose.orientation.z; views[i].orientation[3] = source[i].pose.orientation.w; }
    float scale = 1.0f, distance = 2.5f; int follow = 1; IW_Android_GetXRScreenGeometry(&scale, &distance, reinterpret_cast<qboolean *>(&follow)); (void)scale;
    iw_xr_virtual_screen_pose_t result{}; if (!IW_XRVirtualScreen_UpdatePose(&xr.screen_follow, views.data(), count, (double)predicted * 1e-9, distance, follow != 0, &result)) return false;
    pose->position = {result.position[0], result.position[1], result.position[2]}; pose->orientation = {result.orientation[0], result.orientation[1], result.orientation[2], result.orientation[3]}; return true;
}static XrPosef CylinderPose(const XrPosef &screen_pose, float radius) {
    XrPosef pose = screen_pose; float offset[3] = {0.f, 0.f, radius};
    float qv[3] = {screen_pose.orientation.x, screen_pose.orientation.y, screen_pose.orientation.z}, t[3], c[3];
    t[0] = 2.f * (qv[1] * offset[2] - qv[2] * offset[1]); t[1] = 2.f * (qv[2] * offset[0] - qv[0] * offset[2]); t[2] = 2.f * (qv[0] * offset[1] - qv[1] * offset[0]);
    c[0] = qv[1] * t[2] - qv[2] * t[1]; c[1] = qv[2] * t[0] - qv[0] * t[2]; c[2] = qv[0] * t[1] - qv[1] * t[0];
    offset[0] += screen_pose.orientation.w * t[0] + c[0]; offset[1] += screen_pose.orientation.w * t[1] + c[1]; offset[2] += screen_pose.orientation.w * t[2] + c[2];
    pose.position.x += offset[0]; pose.position.y += offset[1]; pose.position.z += offset[2]; return pose;
}
static XrPosef PointerPose(const float start[3], const float hit[3], const float eye[3])
{
    XrPosef pose{}; float x[3] = {hit[0]-start[0], hit[1]-start[1], hit[2]-start[2]}; float length = std::sqrt(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]);
    pose.orientation.w = 1.f; if (length < 0.001f) return pose; for (int i=0;i<3;++i) x[i] /= length;
    float mid[3] = {(start[0]+hit[0])*.5f,(start[1]+hit[1])*.5f,(start[2]+hit[2])*.5f}; float view[3] = {eye[0]-mid[0],eye[1]-mid[1],eye[2]-mid[2]}; float vl=std::sqrt(view[0]*view[0]+view[1]*view[1]+view[2]*view[2]); if (vl < 0.001f) vl=1.f; for (int i=0;i<3;++i) view[i]/=vl;
    float y[3] = {view[1]*x[2]-view[2]*x[1], view[2]*x[0]-view[0]*x[2], view[0]*x[1]-view[1]*x[0]}; float yl=std::sqrt(y[0]*y[0]+y[1]*y[1]+y[2]*y[2]); if (yl < 0.001f) { y[0]=0.f; y[1]=1.f; y[2]=0.f; yl=1.f; } for (int i=0;i<3;++i) y[i]/=yl;
    float check_z[3] = {x[1]*y[2]-x[2]*y[1], x[2]*y[0]-x[0]*y[2], x[0]*y[1]-x[1]*y[0]};
    float check_len = std::sqrt(check_z[0]*check_z[0]+check_z[1]*check_z[1]+check_z[2]*check_z[2]);
    if (check_len < 0.01f) {
        float fallback_y0 = std::fabs(x[1]) > 0.9f ? 1.f : x[2];
        float fallback_y1 = 0.f, fallback_y2 = std::fabs(x[1]) > 0.9f ? 0.f : -x[0];
        y[0] = fallback_y0; y[1] = fallback_y1; y[2] = fallback_y2;
        float fallback_len = std::sqrt(y[0]*y[0]+y[1]*y[1]+y[2]*y[2]);
        if (fallback_len > 0.001f) for (int i=0;i<3;++i) y[i] /= fallback_len;
    }
    float z[3] = {x[1]*y[2]-x[2]*y[1], x[2]*y[0]-x[0]*y[2], x[0]*y[1]-x[1]*y[0]}; float trace = x[0]+y[1]+z[2];
    if (trace > 0.f) { float q=std::sqrt(trace+1.f)*2.f; pose.orientation.w=.25f*q; pose.orientation.x=(y[2]-z[1])/q; pose.orientation.y=(z[0]-x[2])/q; pose.orientation.z=(x[1]-y[0])/q; } else if (x[0] > y[1] && x[0] > z[2]) { float q=std::sqrt(1.f+x[0]-y[1]-z[2])*2.f; pose.orientation.w=(y[2]-z[1])/q; pose.orientation.x=.25f*q; pose.orientation.y=(y[0]+x[1])/q; pose.orientation.z=(z[0]+x[2])/q; } else if (y[1] > z[2]) { float q=std::sqrt(1.f+y[1]-x[0]-z[2])*2.f; pose.orientation.w=(z[0]-x[2])/q; pose.orientation.x=(y[0]+x[1])/q; pose.orientation.y=.25f*q; pose.orientation.z=(z[1]+y[2])/q; } else { float q=std::sqrt(1.f+z[2]-x[0]-y[1])*2.f; pose.orientation.w=(x[1]-y[0])/q; pose.orientation.x=(z[0]+x[2])/q; pose.orientation.y=(z[1]+y[2])/q; pose.orientation.z=.25f*q; }
    pose.position = {(start[0]+hit[0])*.5f,(start[1]+hit[1])*.5f,(start[2]+hit[2])*.5f}; return pose;
}
static bool LocateHUDPose(AndroidXRRuntime &xr, XrTime predicted, XrPosef *pose)
{
    XrViewLocateInfo locate{}; locate.type = XR_TYPE_VIEW_LOCATE_INFO; locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; locate.displayTime = predicted; locate.space = xr.screen_space;
    XrViewState state{}; state.type = XR_TYPE_VIEW_STATE; uint32_t capacity = 0, count = 0;
    if (xrEnumerateViewConfigurationViews(xr.instance, xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &capacity, nullptr) != XR_SUCCESS || capacity < 2) return false;
    std::vector<XrView> source(capacity); for (auto &view : source) view.type = XR_TYPE_VIEW;
    if (!XR_Ok(xrLocateViews(xr.session, &locate, &state, capacity, &count, source.data()), "xrLocateViews HUD") || count < 2) return false;
    iw_xr_virtual_screen_view_t views[2];
    for (unsigned i = 0; i < 2; ++i) { views[i].position[0] = source[i].pose.position.x; views[i].position[1] = source[i].pose.position.y; views[i].position[2] = source[i].pose.position.z; views[i].orientation[0] = source[i].pose.orientation.x; views[i].orientation[1] = source[i].pose.orientation.y; views[i].orientation[2] = source[i].pose.orientation.z; views[i].orientation[3] = source[i].pose.orientation.w; }
    float scale = 0.35f, distance = 0.5f, yoffset = 0.f; IW_Android_GetXRHUDGeometry(&scale, &distance, &yoffset); (void)scale; (void)predicted;
    float center[3] = {0.f, 0.f, 0.f}, forward[3] = {0.f, 0.f, 0.f};
    for (unsigned i = 0; i < 2; ++i) {
        center[0] += views[i].position[0]; center[1] += views[i].position[1]; center[2] += views[i].position[2];
        float qx = views[i].orientation[0], qy = views[i].orientation[1], qz = views[i].orientation[2], qw = views[i].orientation[3];
        float tx = -2.f * qy, ty = 2.f * qx, tz = 0.f;
        forward[0] +=       qw * tx + (qy * tz - qz * ty);
        forward[1] +=       qw * ty + (qz * tx - qx * tz);
        forward[2] += -1.f + qw * tz + (qx * ty - qy * tx);
    }
    center[0] *= 0.5f; center[1] *= 0.5f; center[2] *= 0.5f; forward[1] = 0.f;
    float length = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    if (length < 0.0001f) { forward[0] = 0.f; forward[2] = -1.f; } else { forward[0] /= length; forward[2] /= length; }
    float yaw = std::atan2(-forward[0], -forward[2]), half = yaw * 0.5f;
    pose->orientation = {0.f, std::sin(half), 0.f, std::cos(half)};
    pose->position = {center[0] + forward[0] * distance, center[1] + yoffset, center[2] + forward[2] * distance};
    return true;
}
static bool LocateStereoFrame(AndroidXRRuntime &xr, XrTime predicted, iw_xr_frame_snapshot_t *snapshot)
{
    if (!snapshot) return false;
    XrViewLocateInfo locate{}; locate.type = XR_TYPE_VIEW_LOCATE_INFO; locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; locate.displayTime = predicted; locate.space = xr.screen_space;
    XrViewState state{}; state.type = XR_TYPE_VIEW_STATE;
    uint32_t capacity = 2, count = 0;
    for (auto &view : xr.located_views) { view = {}; view.type = XR_TYPE_VIEW; }
    if (!XR_Ok(xrLocateViews(xr.session, &locate, &state, capacity, &count, xr.located_views), "xrLocateViews stereo") || count < 2) return false;
    std::memset(snapshot, 0, sizeof(*snapshot));
    snapshot->predicted_display_time = (uint64_t)predicted;
    snapshot->view_count = 2;
    snapshot->should_render = true;
    xr.located_view_count = count;
    for (unsigned eye = 0; eye < 2; ++eye)
    {
        const XrView &view = xr.located_views[eye];
        snapshot->views[eye].position[0] = view.pose.position.x; snapshot->views[eye].position[1] = view.pose.position.y; snapshot->views[eye].position[2] = view.pose.position.z;
        snapshot->views[eye].orientation[0] = view.pose.orientation.x; snapshot->views[eye].orientation[1] = view.pose.orientation.y; snapshot->views[eye].orientation[2] = view.pose.orientation.z; snapshot->views[eye].orientation[3] = view.pose.orientation.w;
        snapshot->views[eye].fov.left = view.fov.angleLeft; snapshot->views[eye].fov.right = view.fov.angleRight; snapshot->views[eye].fov.up = view.fov.angleUp; snapshot->views[eye].fov.down = view.fov.angleDown;
    }
    return true;
}

static bool RenderMultiviewFrame(AndroidXRRuntime &xr, XrTime predicted, const iw_xr_frame_snapshot_t *snapshot)
{
    auto &target = xr.multiview;
    XrSwapchainImageAcquireInfo acquire{}; acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    XrSwapchainImageWaitInfo wait{}; wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; wait.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo release{}; release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    bool mono_acquired = false;
    uint32_t mono_index = 0;
    unsigned overlay_fbos[2] = {};
    if (!XR_Ok(xrAcquireSwapchainImage(xr.swapchain, &acquire, &mono_index), "xrAcquireSwapchainImage multiview mono")) return false;
    mono_acquired = true;
    if (!XR_Ok(xrWaitSwapchainImage(xr.swapchain, &wait), "xrWaitSwapchainImage multiview mono")) goto done;
    if (!XR_Ok(xrAcquireSwapchainImage(target.swapchain, &acquire, &target.image_index), "xrAcquireSwapchainImage multiview")) goto done;
    target.acquired = true;
    if (!XR_Ok(xrWaitSwapchainImage(target.swapchain, &wait), "xrWaitSwapchainImage multiview")) goto done;
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbos[target.image_index]);
    glViewport(0, 0, target.width, target.height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, xr.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, xr.images[mono_index].image, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) goto done;
    glViewport(0, 0, xr.width, xr.height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    xr.virtual_environment_rendered = false;
    overlay_fbos[0] = target.overlay_fbos[target.image_index * 2];
    overlay_fbos[1] = target.overlay_fbos[target.image_index * 2 + 1];
    if (!IW_Android_FrameXRStereoMultiview((uint64_t)predicted, snapshot, xr.fbo, xr.width, xr.height, target.fbos[target.image_index], overlay_fbos, target.width, target.height)) goto done;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    xrReleaseSwapchainImage(target.swapchain, &release); target.acquired = false;
    xrReleaseSwapchainImage(xr.swapchain, &release);
    return true;
done:
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (target.acquired) { xrReleaseSwapchainImage(target.swapchain, &release); target.acquired = false; }
    if (mono_acquired) xrReleaseSwapchainImage(xr.swapchain, &release);
    return false;
}
static bool RenderStereoFrame(AndroidXRRuntime &xr, XrTime predicted, const iw_xr_frame_snapshot_t *snapshot)
{
    xr.multiview_rendered = false;
    const bool requested = IW_Android_XRMultiviewRequested();
    const bool gameplay = IW_Android_XRGameplayStereoEligible();
    const bool active = requested && gameplay && xr.multiview.capable;
    if (xr.multiview_last_log_state != (active ? 1 : 0)) {
        XR_LOG("multiview requested=%d target_ready=%d gameplay=%d active=%d", requested ? 1 : 0, xr.multiview.capable ? 1 : 0, gameplay ? 1 : 0, active ? 1 : 0);
        xr.multiview_last_log_state = active ? 1 : 0;
    }
    if (active) {
        xr.multiview_rendered = RenderMultiviewFrame(xr, predicted, snapshot);
        if (xr.multiview_rendered) return true;
        XR_LOG("multiview frame failed; falling back to two-pass stereo");
    }
    XrSwapchainImageAcquireInfo acquire{}; acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    XrSwapchainImageWaitInfo wait{}; wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; wait.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo release{}; release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    bool mono_acquired = false;
    auto release_targets = [&]() {
        for (unsigned eye = 0; eye < 2; ++eye) {
            AndroidXREyeTarget &target = xr.eyes[eye];
            if (target.acquired) {
                xrReleaseSwapchainImage(target.swapchain, &release);
                target.acquired = false;
            }
        }
        if (mono_acquired) {
            xrReleaseSwapchainImage(xr.swapchain, &release);
            mono_acquired = false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    };
    uint32_t mono_index = 0;
    if (!XR_Ok(xrAcquireSwapchainImage(xr.swapchain, &acquire, &mono_index), "xrAcquireSwapchainImage mono")) return false;
    mono_acquired = true;
    if (!XR_Ok(xrWaitSwapchainImage(xr.swapchain, &wait), "xrWaitSwapchainImage mono")) { release_targets(); return false; }
    unsigned eye_fbos[2] = {}; int eye_widths[2] = {}; int eye_heights[2] = {};
    for (unsigned eye = 0; eye < 2; ++eye)
    {
        AndroidXREyeTarget &target = xr.eyes[eye];
        if (!XR_Ok(xrAcquireSwapchainImage(target.swapchain, &acquire, &target.image_index), "xrAcquireSwapchainImage eye")) { release_targets(); return false; }
        target.acquired = true;
        if (!XR_Ok(xrWaitSwapchainImage(target.swapchain, &wait), "xrWaitSwapchainImage eye")) { release_targets(); return false; }
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.images[target.image_index].image, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { release_targets(); return false; }
        glViewport(0, 0, target.width, target.height); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        eye_fbos[eye] = target.fbo; eye_widths[eye] = target.width; eye_heights[eye] = target.height;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, xr.fbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, xr.images[mono_index].image, 0); glViewport(0, 0, xr.width, xr.height); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { release_targets(); return false; }
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
    glDisable(GL_FRAMEBUFFER_SRGB);
    qboolean stereo_used = IW_Android_FrameXRStereo((uint64_t)predicted, snapshot, xr.fbo, xr.width, xr.height, eye_fbos, eye_widths, eye_heights);
    xr.virtual_environment_rendered = false;
    if (!stereo_used && IW_Android_GetXRBackdropScene()) {
        iw_xr_virtual_screen_t screen{};
        float screen_position[3], screen_orientation[4], scale = 1.f, radius = 3.f;
        qboolean curved = false;
        IW_Android_GetXRScreenGeometry(&scale, nullptr, nullptr);
        IW_Android_GetXRScreenStyle(&curved, &radius);
        if (IW_Android_GetXRScreenPose(screen_position, screen_orientation)) {
            std::memcpy(screen.position, screen_position, sizeof(screen.position));
            std::memcpy(screen.orientation, screen_orientation, sizeof(screen.orientation));
            screen.width = 2.97f * scale; screen.height = 2.2275f * scale;
            screen.curved = curved && radius > 1.2f; screen.curve_radius = screen.curved ? radius : 0.f;
            stereo_used = IW_XRVirtualEnvironment_Render(&snapshot->views[0], &screen, xr.images[mono_index].image, eye_fbos[0], eye_widths[0], eye_heights[0]) &&
                IW_XRVirtualEnvironment_Render(&snapshot->views[1], &screen, xr.images[mono_index].image, eye_fbos[1], eye_widths[1], eye_heights[1]);
            xr.virtual_environment_rendered = stereo_used != false;
        }
    }
    release_targets();
    return stereo_used != false;
}
static void RenderFrame(AndroidXRRuntime &xr, XrTime predicted) {
    XrSwapchainImageAcquireInfo acquire{}; acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO; uint32_t index = 0; if (!XR_Ok(xrAcquireSwapchainImage(xr.swapchain, &acquire, &index), "xrAcquireSwapchainImage")) return;
    XrSwapchainImageWaitInfo wait{}; wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; wait.timeout = XR_INFINITE_DURATION; if (!XR_Ok(xrWaitSwapchainImage(xr.swapchain, &wait), "xrWaitSwapchainImage")) return;
    glBindFramebuffer(GL_FRAMEBUFFER, xr.fbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, xr.images[index].image, 0); glViewport(0, 0, xr.width, xr.height); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
    glDisable(GL_FRAMEBUFFER_SRGB);
    IW_Android_FrameXR((uint64_t)predicted, xr.fbo, xr.width, xr.height);
glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo release{}; release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO; xrReleaseSwapchainImage(xr.swapchain, &release);
}

static void *XRThread(void *) {
    JNIEnv *env = nullptr; g_host.vm->AttachCurrentThread(&env, nullptr);
    AndroidXRRuntime xr;
    while (true) {
        pthread_mutex_lock(&g_host.mutex); bool stop = g_host.stopping; pthread_mutex_unlock(&g_host.mutex); if (stop) break;
        if (!xr.instance) {
            if (!CreateEGL(xr) || !CreateInstance(xr) || !CreateSession(xr) || !SetupActions(xr)) { XR_ERR("OpenXR setup failed"); DestroyRuntime(xr); pthread_mutex_lock(&g_host.mutex); g_host.paused = true; pthread_mutex_unlock(&g_host.mutex); continue; }
            g_haptic_runtime = &xr; IW_Android_Resize(xr.width, xr.height); IW_Android_SurfaceCreated(); std::vector<std::string> init_strings = BuildEngineArgs(); std::vector<const char *> init_args; for (const std::string &arg : init_strings) init_args.push_back(arg.c_str()); if (!IW_Android_Init(g_base_dir.c_str(), (int)init_args.size(), init_args.data())) { XR_ERR("Ironwail init failed"); pthread_mutex_lock(&g_host.mutex); g_host.stopping = true; pthread_mutex_unlock(&g_host.mutex); continue; } g_host.initialized = true; ConfigureRefreshRate(xr);
        }
        PollEvents(xr);
        if (g_host.initialized) ResizeEyeTargets(xr);
        if (!xr.running) { if (g_host.initialized && !g_host.engine_paused) { IW_Android_Pause(true); g_host.engine_paused = true; } usleep(1000); continue; }
        XrFrameWaitInfo wait{}; wait.type = XR_TYPE_FRAME_WAIT_INFO; XrFrameState frame{}; frame.type = XR_TYPE_FRAME_STATE; if (!XR_Ok(xrWaitFrame(xr.session, &wait, &frame), "xrWaitFrame")) continue;
        qboolean engine_paused = frame.shouldRender ? false : true;
        if (engine_paused != g_host.engine_paused && g_host.initialized) { IW_Android_Pause(engine_paused); g_host.engine_paused = engine_paused; }
        XrFrameBeginInfo begin{}; begin.type = XR_TYPE_FRAME_BEGIN_INFO; if (!XR_Ok(xrBeginFrame(xr.session, &begin), "xrBeginFrame")) continue; xr.last_predicted = frame.predictedDisplayTime; UpdateActions(xr);
        std::vector<XrCompositionLayerBaseHeader *> layers;
        XrCompositionLayerProjection projection{}; XrCompositionLayerProjectionView projection_views[2]{};
        XrCompositionLayerQuad quad{}; XrCompositionLayerQuad hud{}; XrCompositionLayerQuad pointer_layer{}; XrCompositionLayerCylinderKHR cylinder{}; XrSwapchainSubImage sub{};
        bool stereo_used = false;
        bool located = false;
        iw_xr_frame_snapshot_t android_snapshot{};
        if (frame.shouldRender)
        {
            ConfigureRefreshRate(xr);
            located = LocateStereoFrame(xr, frame.predictedDisplayTime, &android_snapshot);
            android_snapshot.predicted_display_period = (uint64_t)frame.predictedDisplayPeriod;
            if (xr.last_pacing_predicted && (++xr.pacing_frame_count % 180u) == 0u) XR_LOG("frame pacing predicted=%.3fms interval=%.3fms target=%.3fms", (double)frame.predictedDisplayTime * 1e-6, (double)(frame.predictedDisplayTime - xr.last_pacing_predicted) * 1e-6, (double)frame.predictedDisplayPeriod * 1e-6);
            xr.last_pacing_predicted = frame.predictedDisplayTime;
            if (located) stereo_used = RenderStereoFrame(xr, frame.predictedDisplayTime, &android_snapshot);
            bool pointer_used = false;
            float pointer_start[3], pointer_hit[3], pointer_eye[3]; unsigned pointer_color = 0; float pointer_alpha = 0.f, pointer_width = 1.f;
            if (located && xr.pointer.swapchain != XR_NULL_HANDLE && IW_Android_GetVirtualPointer(pointer_start, pointer_hit, &pointer_color, &pointer_alpha, &pointer_width)) {
                XrSwapchainImageAcquireInfo pointer_acquire{}; pointer_acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO; uint32_t pointer_index = 0;
                XrSwapchainImageWaitInfo pointer_wait{}; pointer_wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; pointer_wait.timeout = XR_INFINITE_DURATION;
                if (xrAcquireSwapchainImage(xr.pointer.swapchain, &pointer_acquire, &pointer_index) == XR_SUCCESS && xrWaitSwapchainImage(xr.pointer.swapchain, &pointer_wait) == XR_SUCCESS) {
                    glBindFramebuffer(GL_FRAMEBUFFER, xr.pointer.fbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, xr.pointer.images[pointer_index].image, 0); glViewport(0, 0, xr.pointer.width, xr.pointer.height); glDisable(GL_SCISSOR_TEST); glClearColor(0.f, 0.f, 0.f, 0.f); glClear(GL_COLOR_BUFFER_BIT); glEnable(GL_SCISSOR_TEST); glScissor(0, 26, xr.pointer.width, 12); glClearColor(((pointer_color >> 16) & 255) / 255.f, ((pointer_color >> 8) & 255) / 255.f, (pointer_color & 255) / 255.f, pointer_alpha); glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
                    XrSwapchainImageReleaseInfo pointer_release{}; pointer_release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO; xrReleaseSwapchainImage(xr.pointer.swapchain, &pointer_release); pointer_eye[0] = 0.5f * (xr.located_views[0].pose.position.x + xr.located_views[1].pose.position.x); pointer_eye[1] = 0.5f * (xr.located_views[0].pose.position.y + xr.located_views[1].pose.position.y); pointer_eye[2] = 0.5f * (xr.located_views[0].pose.position.z + xr.located_views[1].pose.position.z);
                    pointer_layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD; pointer_layer.space = xr.screen_space; pointer_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH; pointer_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT; pointer_layer.subImage.swapchain = xr.pointer.swapchain; pointer_layer.subImage.imageRect.extent = {xr.pointer.width, xr.pointer.height}; pointer_layer.pose = PointerPose(pointer_start, pointer_hit, pointer_eye); pointer_layer.size.width = std::sqrt((pointer_hit[0]-pointer_start[0])*(pointer_hit[0]-pointer_start[0])+(pointer_hit[1]-pointer_start[1])*(pointer_hit[1]-pointer_start[1])+(pointer_hit[2]-pointer_start[2])*(pointer_hit[2]-pointer_start[2])); pointer_layer.size.height = 0.012f * std::max(0.25f, std::min(8.f, pointer_width)); pointer_used = true;
                }
            }
            if (stereo_used)
            {
                projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION; projection.space = xr.screen_space; projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; projection.viewCount = 2; projection.views = projection_views;
                for (unsigned eye = 0; eye < 2; ++eye)
                {
                    projection_views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW; projection_views[eye].pose = xr.located_views[eye].pose; projection_views[eye].fov = xr.located_views[eye].fov;
                    projection_views[eye].subImage.swapchain = xr.multiview_rendered ? xr.multiview.swapchain : xr.eyes[eye].swapchain; projection_views[eye].subImage.imageRect.offset = {0, 0}; projection_views[eye].subImage.imageRect.extent = xr.multiview_rendered ? XrExtent2Di{xr.multiview.width, xr.multiview.height} : XrExtent2Di{xr.eyes[eye].width, xr.eyes[eye].height}; projection_views[eye].subImage.imageArrayIndex = xr.multiview_rendered ? eye : 0;
                }
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&projection));
                if (!xr.virtual_environment_rendered) {
                hud.type = XR_TYPE_COMPOSITION_LAYER_QUAD; hud.space = xr.screen_space; hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH; hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; hud.subImage.swapchain = xr.swapchain; hud.subImage.imageRect.offset = {0, 0}; hud.subImage.imageRect.extent = {xr.width, xr.height};
                if (LocateHUDPose(xr, frame.predictedDisplayTime, &hud.pose)) { float hud_scale = 0.35f, hud_distance = 0.5f, hud_yoffset = 0.f; IW_Android_GetXRHUDGeometry(&hud_scale, &hud_distance, &hud_yoffset); hud.size.width = 1.8f * hud_scale; hud.size.height = hud.size.width * (float)xr.height / (float)xr.width; layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&hud)); if (!g_hud_pose_logged) { XR_LOG("Android HUD layer submitted size=%.2fx%.2f distance=%.2f", hud.size.width, hud.size.height, hud_distance); g_hud_pose_logged = true; } } else if (!g_hud_pose_missing_logged) { XR_LOG("Android HUD pose locate failed"); g_hud_pose_missing_logged = true; }
                }
                if (pointer_used) layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&pointer_layer));
            }
            else
            {
                if (!located) RenderFrame(xr, frame.predictedDisplayTime);
                sub.swapchain = xr.swapchain; sub.imageRect.offset = {0,0}; sub.imageRect.extent = {xr.width,xr.height}; sub.imageArrayIndex = 0;
                quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD; quad.space = xr.screen_space; quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH; quad.subImage = sub;
                float scale = 1.0f, distance = 2.5f; int follow = 1; IW_Android_GetXRScreenGeometry(&scale, &distance, &follow); (void)distance; (void)follow;
                if (!LocateScreenPose(xr, frame.predictedDisplayTime, &quad.pose)) { quad.pose.orientation.w = 1.0f; quad.pose.position.y = 1.6f; quad.pose.position.z = -2.0f; }
                quad.size = {2.97f * scale, 2.2275f * scale}; qboolean curved = false; float radius = 3.0f; IW_Android_GetXRScreenStyle(&curved, &radius);
                if (curved && xr.cylinder_supported && radius > 1.2f) { cylinder.type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR; cylinder.space = xr.screen_space; cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH; cylinder.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; cylinder.subImage = sub; cylinder.pose = CylinderPose(quad.pose, radius); cylinder.radius = radius; cylinder.centralAngle = quad.size.width / radius; cylinder.aspectRatio = quad.size.width / quad.size.height; layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&cylinder)); }
                else layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&quad));
                if (pointer_used) layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&pointer_layer));
            }
        }
        XrFrameEndInfo end{}; end.type = XR_TYPE_FRAME_END_INFO; end.displayTime = frame.predictedDisplayTime; end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; end.layerCount = (uint32_t)layers.size(); end.layers = layers.data(); xrEndFrame(xr.session, &end);
    }
    IW_Android_ClearActions(); if (g_host.initialized) IW_Android_Shutdown(); DestroyRuntime(xr); g_host.vm->DetachCurrentThread(); return nullptr;
}

extern "C" JNIEXPORT void JNICALL Java_com_ermac_ironwail_GLES3OpenXRActivity_nativeCreate(JNIEnv *env, jobject self, jstring data_dir, jobject activity) { pthread_mutex_lock(&g_host.mutex); g_host.stopping = false; g_host.surface_changed = false; g_host.focused = false; g_host.activity_paused = true; g_host.audio_focused = false; g_host.paused = g_host.activity_paused || !g_host.surface_ready; g_host.initialized = false; g_host.engine_paused = false; pthread_mutex_unlock(&g_host.mutex); if (data_dir) { const char *path = env->GetStringUTFChars(data_dir, nullptr); if (path) { g_base_dir = path; env->ReleaseStringUTFChars(data_dir, path); } } env->GetJavaVM(&g_host.vm); g_host.activity = env->NewGlobalRef(activity); g_host.started = true; pthread_create(&g_host.thread, nullptr, XRThread, nullptr); }
extern "C" JNIEXPORT void JNICALL Java_com_ermac_ironwail_GLES3OpenXRActivity_nativeSurfaceCreated(JNIEnv *env, jobject, jobject surface) { ANativeWindow *window = ANativeWindow_fromSurface(env, surface); pthread_mutex_lock(&g_host.mutex); if (g_host.window) ANativeWindow_release(g_host.window); g_host.window = window; g_host.surface_ready = true; g_host.paused = g_host.activity_paused; if (!g_host.paused) pthread_cond_signal(&g_host.cond); pthread_mutex_unlock(&g_host.mutex); }
extern "C" JNIEXPORT void JNICALL Java_com_ermac_ironwail_GLES3OpenXRActivity_nativeSurfaceDestroyed(JNIEnv *, jobject) { pthread_mutex_lock(&g_host.mutex); g_host.surface_changed = true; g_host.surface_ready = false; g_host.paused = true; pthread_mutex_unlock(&g_host.mutex); }
extern "C" JNIEXPORT void JNICALL Java_com_ermac_ironwail_GLES3OpenXRActivity_nativeAudioFocus(JNIEnv *, jobject, jboolean focused) { pthread_mutex_lock(&g_host.mutex); g_host.audio_focused = focused; g_host.paused = g_host.activity_paused || !g_host.surface_ready; if (!g_host.paused && !g_host.stopping) pthread_cond_signal(&g_host.cond); pthread_mutex_unlock(&g_host.mutex); IW_Android_AudioFocus(focused ? true : false); }
extern "C" JNIEXPORT void JNICALL Java_com_ermac_ironwail_GLES3OpenXRActivity_nativePause(JNIEnv *, jobject, jboolean paused) { pthread_mutex_lock(&g_host.mutex); g_host.activity_paused = paused; g_host.paused = paused || !g_host.surface_ready; if (!g_host.paused) pthread_cond_signal(&g_host.cond); pthread_mutex_unlock(&g_host.mutex); }
extern "C" JNIEXPORT void JNICALL Java_com_ermac_ironwail_GLES3OpenXRActivity_nativeFocus(JNIEnv *, jobject, jboolean focused) { pthread_mutex_lock(&g_host.mutex); g_host.focused = focused; g_host.paused = g_host.activity_paused || !g_host.surface_ready; if (!g_host.paused && !g_host.stopping) pthread_cond_signal(&g_host.cond); pthread_mutex_unlock(&g_host.mutex); if (!focused) { IW_Android_NativeHaptic(0, 0.f, 0.f); IW_Android_NativeHaptic(1, 0.f, 0.f); } }
extern "C" JNIEXPORT void JNICALL Java_com_ermac_ironwail_GLES3OpenXRActivity_nativeShutdown(JNIEnv *env, jobject) { pthread_mutex_lock(&g_host.mutex); XrSession active_session = g_active_session; if (!g_host.stopping) { g_host.stopping = true; pthread_cond_broadcast(&g_host.cond); } pthread_mutex_unlock(&g_host.mutex); if (active_session != XR_NULL_HANDLE) xrRequestExitSession(active_session); if (g_host.started) pthread_join(g_host.thread, nullptr); if (g_host.window) { ANativeWindow_release(g_host.window); g_host.window = nullptr; } if (g_host.activity) { env->DeleteGlobalRef(g_host.activity); g_host.activity = nullptr; } g_host.initialized = false; g_host.engine_paused = false; g_host.surface_ready = false; g_host.started = false; }
