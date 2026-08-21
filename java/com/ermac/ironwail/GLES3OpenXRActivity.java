package com.ermac.ironwail;

import android.app.Activity;
import android.media.AudioManager;
import android.os.Bundle;
import android.os.Environment;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.Window;
import android.view.WindowManager;
import org.libsdl.app.SDL;
import android.view.View;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class GLES3OpenXRActivity extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("ironrift"); }
    private SurfaceView surfaceView;
    private boolean nativeStarted;
    private boolean resumed;
    private boolean focused;
    private boolean audioFocused;
    private AudioManager audioManager;
    private AudioManager.OnAudioFocusChangeListener audioFocusListener;

    private native void nativeCreate(String dataDir, Object activity);
    private native void nativeSurfaceCreated(Surface surface);
    private native void nativeSurfaceDestroyed();
    private native void nativePause(boolean paused);
    private native void nativeFocus(boolean focused);
    private native void nativeAudioFocus(boolean focused);
    private native void nativeShutdown();

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        SDL.setupJNI();
        SDL.initialize();
        SDL.setContext(this);
        audioManager = (AudioManager)getSystemService(AUDIO_SERVICE);
        audioFocusListener = change -> { audioFocused = change == AudioManager.AUDIOFOCUS_GAIN; if (nativeStarted) nativeAudioFocus(audioFocused); };
        requestAudioFocus();
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN, WindowManager.LayoutParams.FLAG_FULLSCREEN);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        final File dataDir = prepareDataDir();
        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        setContentView(surfaceView);
        nativeCreate(dataDir.getAbsolutePath(), this);
        nativeStarted = true;
        nativeAudioFocus(audioFocused);
        nativeFocus(hasWindowFocus());
    }

    private void requestAudioFocus() {
        if (audioManager == null) return;
        int result = audioManager.requestAudioFocus(audioFocusListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN);
        audioFocused = result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        if (nativeStarted) nativeAudioFocus(audioFocused);
    }

    private File prepareDataDir() {
        File dir = new File(Environment.getExternalStorageDirectory(), "ironwail");
        if (!dir.exists()) dir.mkdirs();
        copyAssetIfChanged("ironwail.pak", new File(dir, "ironwail.pak"));
        copyAssetIfChanged("ironwail_vr.pak", new File(dir, "ironwail_vr.pak"));
        File id1 = new File(dir, "id1");
        if (!id1.exists()) id1.mkdirs();
        copyAssetIfMissing("quake_shareware.zip", new File(dir, "quake_shareware.zip"));
        extractShareware(new File(dir, "quake_shareware.zip"), id1);
        ensureCommandLine(new File(dir, "commandline.txt"));
        return dir;
    }

    private void ensureCommandLine(File file) {
        if (file.isFile()) return;
        try (FileOutputStream out = new FileOutputStream(file)) { out.write("ironwail -condebug\n".getBytes("UTF-8")); } catch (Exception ignored) { }
    }

    private void extractShareware(File archive, File id1) {
        File pak = new File(id1, "pak0.pak");
        if (pak.isFile()) return;
        try (ZipInputStream zip = new ZipInputStream(new java.io.FileInputStream(archive))) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                String name = entry.getName();
                if (name.endsWith("pak0.pak") || name.endsWith("PAK0.PAK")) {
                    try (FileOutputStream out = new FileOutputStream(pak)) {
                        byte[] buffer = new byte[65536]; int count;
                        while ((count = zip.read(buffer)) >= 0) out.write(buffer, 0, count);
                    }
                    return;
                }
            }
        } catch (Exception ignored) { }
    }

    private void copyAssetIfMissing(String name, File destination) {
        if (destination.isFile()) return;
        try (InputStream in = getAssets().open(name); FileOutputStream out = new FileOutputStream(destination)) {
            byte[] buffer = new byte[65536]; int count;
            while ((count = in.read(buffer)) >= 0) out.write(buffer, 0, count);
        } catch (Exception ignored) { }
    }

    private byte[] sha256(InputStream in) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        byte[] buffer = new byte[65536]; int count;
        while ((count = in.read(buffer)) >= 0) digest.update(buffer, 0, count);
        return digest.digest();
    }

    private byte[] sha256(File file) throws Exception {
        try (FileInputStream in = new FileInputStream(file)) { return sha256(in); }
    }

    private void copyAssetIfChanged(String name, File destination) {
        try (InputStream asset = getAssets().open(name)) {
            byte[] assetHash = sha256(asset);
            if (destination.isFile() && MessageDigest.isEqual(assetHash, sha256(destination))) return;
        } catch (Exception ignored) { return; }

        File temporary = new File(destination.getPath() + ".part");
        try (InputStream in = getAssets().open(name); FileOutputStream out = new FileOutputStream(temporary)) {
            byte[] buffer = new byte[65536]; int count;
            while ((count = in.read(buffer)) >= 0) out.write(buffer, 0, count);
            out.getFD().sync();
        } catch (Exception ignored) {
            temporary.delete();
            return;
        }
        if (!temporary.renameTo(destination)) temporary.delete();
    }
    @Override protected void onResume() { super.onResume(); resumed = true; requestAudioFocus(); if (nativeStarted) { nativeAudioFocus(audioFocused); nativeFocus(true); nativePause(false); } }
    @Override protected void onPause() { resumed = false; audioFocused = false; if (nativeStarted) { nativeAudioFocus(false); nativePause(true); } if (audioManager != null) audioManager.abandonAudioFocus(audioFocusListener); super.onPause(); }
    @Override public void onWindowFocusChanged(boolean hasFocus) { super.onWindowFocusChanged(hasFocus); focused = hasFocus; if (hasFocus) requestAudioFocus(); if (nativeStarted) { nativeFocus(hasFocus); if (hasFocus && resumed) { nativeAudioFocus(audioFocused); nativePause(false); } } }
    @Override protected void onDestroy() { audioFocused = false; if (audioManager != null) audioManager.abandonAudioFocus(audioFocusListener); if (nativeStarted) { nativeShutdown(); nativeStarted = false; } super.onDestroy(); }
    @Override public void surfaceCreated(SurfaceHolder holder) { nativeSurfaceCreated(holder.getSurface()); }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) { nativeSurfaceCreated(holder.getSurface()); }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { nativeSurfaceDestroyed(); }
}