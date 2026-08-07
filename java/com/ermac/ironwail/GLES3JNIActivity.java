package com.ermac.ironwail;

import android.app.Activity;
import android.os.Bundle;
import android.os.Build;
import android.os.Environment;
import android.util.Log;
import android.opengl.GLSurfaceView;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Window;
import android.view.WindowManager;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import org.libsdl.app.SDL;

import java.io.File;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public final class GLES3JNIActivity extends Activity {
    static { System.loadLibrary("ironrift"); }

    private GLSurfaceView view;
    private boolean nativeStarted;

    private static native void nativeInit(String dataDir);
    private static native void nativeResize(int width, int height);
    private static native void nativeRender();
    private static native void nativeKey(int keycode, boolean down);
    private static native void nativeAxis(int deviceId, int axis, float value);
    private static native void nativeTouch(int action, float x, float y);
    private static native void nativePause(boolean paused);
    private static native void nativeShutdown();

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN, WindowManager.LayoutParams.FLAG_FULLSCREEN);

        SDL.setupJNI();
        SDL.initialize();
        SDL.setContext(this);

        final File dataDir = prepareDataDir();

        view = new GLSurfaceView(this);
        view.setEGLContextClientVersion(3);
        view.setPreserveEGLContextOnPause(true);
        view.setFocusableInTouchMode(true);
        view.setRenderer(new GLSurfaceView.Renderer() {
            public void onSurfaceCreated(GL10 gl, EGLConfig config) {
                nativeInit(dataDir.getAbsolutePath());
                nativeStarted = true;
            }
            public void onSurfaceChanged(GL10 gl, int width, int height) {
                nativeResize(width, height);
            }
            public void onDrawFrame(GL10 gl) {
                nativeRender();
            }
        });
        view.requestFocus();
        view.setOnTouchListener((v, event) -> {
            nativeTouch(event.getActionMasked(), event.getX(), event.getY());
            return true;
        });
        setContentView(view);
    }

    private File prepareDataDir() {
        File sharedDir = new File(Environment.getExternalStorageDirectory(), "ironwail");
        File dataDir = sharedDir;
        boolean sharedAccess = Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
        if (!sharedAccess) {
            dataDir = new File(getExternalFilesDir(null), "ironwail");
            Log.w("Ironwail", "All-files access is not granted; using app-specific Quake data path: " + dataDir);
        }
        if (!dataDir.exists() && !dataDir.mkdirs())
            Log.e("Ironwail", "Could not create Quake data directory: " + dataDir);
        extractSharewarePak(dataDir);
        return dataDir;
    }

    private void extractSharewarePak(File dataDir) {
        File pak = new File(new File(dataDir, "id1"), "pak0.pak");
        if (pak.isFile())
            return;
        File id1 = pak.getParentFile();
        if (!id1.exists() && !id1.mkdirs()) {
            Log.e("Ironwail", "Could not create Quake id1 directory: " + id1);
            return;
        }
        File temp = new File(id1, "pak0.pak.part");
        try (ZipInputStream zip = new ZipInputStream(getAssets().open("quake_shareware.zip"))) {
            ZipEntry entry;
            boolean extracted = false;
            byte[] buffer = new byte[65536];
            while ((entry = zip.getNextEntry()) != null) {
                String name = entry.getName().replace('\\', '/');
                if (entry.isDirectory() || !(name.equalsIgnoreCase("pak0.pak") || name.equalsIgnoreCase("id1/pak0.pak")))
                    continue;
                try (FileOutputStream out = new FileOutputStream(temp)) {
                    int count;
                    while ((count = zip.read(buffer)) != -1)
                        out.write(buffer, 0, count);
                }
                extracted = temp.renameTo(pak);
                break;
            }
            if (!extracted)
                temp.delete();
        } catch (IOException e) {
            temp.delete();
            Log.i("Ironwail", "No bundled shareware pak extracted: " + e.getMessage());
        }
    }
    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        nativeKey(keyCode, true);
        return true;
    }

    @Override public boolean onKeyUp(int keyCode, KeyEvent event) {
        nativeKey(keyCode, false);
        return true;
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) != 0) {
            InputDevice device = event.getDevice();
            int deviceId = device != null ? device.getId() : -1;
            nativeAxis(deviceId, MotionEvent.AXIS_X, event.getAxisValue(MotionEvent.AXIS_X));
            nativeAxis(deviceId, MotionEvent.AXIS_Y, event.getAxisValue(MotionEvent.AXIS_Y));
            nativeAxis(deviceId, MotionEvent.AXIS_Z, event.getAxisValue(MotionEvent.AXIS_Z));
            nativeAxis(deviceId, MotionEvent.AXIS_RZ, event.getAxisValue(MotionEvent.AXIS_RZ));
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override protected void onPause() {
        nativePause(true);
        super.onPause();
        if (view != null) view.onPause();
    }

    @Override protected void onResume() {
        super.onResume();
        if (view != null) view.onResume();
        nativePause(false);
    }

    @Override protected void onDestroy() {
        if (nativeStarted && view != null)
            view.queueEvent(GLES3JNIActivity::nativeShutdown);
        SDL.setContext(null);
        super.onDestroy();
    }
}
