#include <jni.h>
#include <android/log.h>
#include <GLES3/gl3.h>
#include <EGL/egl.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "IronRift", __VA_ARGS__)

extern "C" JNIEXPORT void JNICALL Java_com_ermac_ironwail_GLES3JNIActivity_nativeInit(JNIEnv*, jclass) {
    LOGI("IronRift GLES placeholder initialized; Ironwail Android backend is not connected yet");
}
extern "C" JNIEXPORT void JNICALL Java_com_ermac_ironwail_GLES3JNIActivity_nativeResize(JNIEnv*, jclass, jint width, jint height) {
    glViewport(0, 0, width, height);
}
extern "C" JNIEXPORT void JNICALL Java_com_ermac_ironwail_GLES3JNIActivity_nativeRender(JNIEnv*, jclass) {
    glClearColor(0.035f, 0.055f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}



