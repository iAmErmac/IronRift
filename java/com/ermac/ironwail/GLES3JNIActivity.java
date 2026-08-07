package com.ermac.ironwail;

import android.app.Activity;
import android.os.Bundle;
import android.opengl.GLSurfaceView;
import android.view.Window;
import android.view.WindowManager;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public final class GLES3JNIActivity extends Activity {
    static { System.loadLibrary("ironrift"); }
    private GLSurfaceView view;
    private static native void nativeInit();
    private static native void nativeResize(int width, int height);
    private static native void nativeRender();
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN, WindowManager.LayoutParams.FLAG_FULLSCREEN);
        view = new GLSurfaceView(this);
        view.setEGLContextClientVersion(3);
        view.setRenderer(new GLSurfaceView.Renderer() {
            public void onSurfaceCreated(GL10 gl, EGLConfig config) { nativeInit(); }
            public void onSurfaceChanged(GL10 gl, int width, int height) { nativeResize(width, height); }
            public void onDrawFrame(GL10 gl) { nativeRender(); }
        });
        setContentView(view);
    }
    @Override protected void onPause() { super.onPause(); if (view != null) view.onPause(); }
    @Override protected void onResume() { super.onResume(); if (view != null) view.onResume(); }
}

