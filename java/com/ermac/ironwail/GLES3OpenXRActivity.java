package com.ermac.ironwail;

import android.app.Activity;
import android.media.AudioManager;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
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
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.URL;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicLong;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class GLES3OpenXRActivity extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("ironrift"); }
    private SurfaceView surfaceView;
    private File dataDir;
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
    private native void nativeAddonDownloadProgress(long bytes);
    private native boolean nativeAddonDownloadCancelled();
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
        copyAssetIfMissing("vr_weapons.pk3", new File(dir, "vr_weapons.pk3"));
        copyAssetIfMissing("addons.json", new File(dir, "addons.json"));
        copyAssetIfMissing("addons.url.dat", new File(dir, "addons.url.dat"));
        File id1 = new File(dir, "id1");
        if (!id1.exists()) id1.mkdirs();
        copyAssetIfMissing("quake_shareware.zip", new File(dir, "quake_shareware.zip"));
        extractShareware(new File(dir, "quake_shareware.zip"), id1);
        ensureCommandLine(new File(dir, "commandline.txt"));
        dataDir = dir;
        return dir;
    }

    private HttpURLConnection openAddonConnection(String address) throws IOException {
        HttpURLConnection connection = (HttpURLConnection)new URL(address).openConnection();
        connection.setInstanceFollowRedirects(true);
        connection.setConnectTimeout(15000);
        connection.setReadTimeout(30000);
        connection.setRequestProperty("Accept-Encoding", "identity");
        int status = connection.getResponseCode();
        if (status < 200 || status >= 300) {
            connection.disconnect();
            throw new IOException("HTTP " + status);
        }
        return connection;
    }

    public String downloadAddonText(String address) {
        HttpURLConnection connection = null;
        try {
            connection = openAddonConnection(address);
            try (InputStream in = connection.getInputStream(); ByteArrayOutputStream out = new ByteArrayOutputStream()) {
                byte[] buffer = new byte[65536];
                int count;
                while ((count = in.read(buffer)) != -1) {
                    if (out.size() + count > 4 * 1024 * 1024) throw new IOException("catalog exceeds 4 MB");
                    out.write(buffer, 0, count);
                }
                Log.i("Ironwail", "Downloaded add-on catalog");
                return out.toString("UTF-8");
            }
        } catch (IOException e) {
            Log.w("Ironwail", "Could not download add-on catalog: " + e.getMessage());
            return null;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    public boolean downloadAddonFile(String address, String destination) {
        try {
            if (dataDir == null) throw new IOException("game data path is unavailable");
            File root = dataDir.getCanonicalFile();
            File target = new File(destination).getCanonicalFile();
            if (!target.getPath().startsWith(root.getPath() + File.separator)) throw new IOException("download target is outside game data");
            File parent = target.getParentFile();
            if (parent == null || (!parent.exists() && !parent.mkdirs())) throw new IOException("could not create add-on directory");
            long total = probeAddonSize(address);
            /* Android builds omit libcurl; use the platform TLS stack and split only range-capable large archives, with a single-stream fallback for every server. */
            if (total >= 4 * 1024 * 1024 && supportsAddonRanges(address)) {
                Log.i("Ironwail", "Downloading add-on archive with 4 HTTP ranges");
                downloadAddonRanges(address, target, total);
            } else {
                Log.i("Ironwail", "Downloading add-on archive with one HTTP stream");
                downloadAddonStream(address, target);
            }
            Log.i("Ironwail", "Downloaded add-on archive: " + target.getName());
            return true;
        } catch (IOException e) {
            Log.w("Ironwail", "Could not download add-on archive: " + e.getMessage());
            return false;
        }
    }

    private long probeAddonSize(String address) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection)new URL(address).openConnection();
            connection.setInstanceFollowRedirects(true);
            connection.setConnectTimeout(15000);
            connection.setReadTimeout(30000);
            connection.setRequestMethod("HEAD");
            connection.setRequestProperty("Accept-Encoding", "identity");
            int status = connection.getResponseCode();
            return status >= 200 && status < 300 ? connection.getContentLengthLong() : -1;
        } catch (IOException ignored) {
            return -1;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }
    private boolean supportsAddonRanges(String address) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection)new URL(address).openConnection();
            connection.setInstanceFollowRedirects(true);
            connection.setConnectTimeout(15000);
            connection.setReadTimeout(30000);
            connection.setRequestProperty("Accept-Encoding", "identity");
            connection.setRequestProperty("Range", "bytes=0-0");
            return connection.getResponseCode() == HttpURLConnection.HTTP_PARTIAL;
        } catch (IOException ignored) {
            return false;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    private void downloadAddonStream(String address, File target) throws IOException {
        HttpURLConnection connection = openAddonConnection(address);
        connection.setReadTimeout(5000);
        AtomicLong downloaded = new AtomicLong();
        AtomicLong lastReport = new AtomicLong(android.os.SystemClock.elapsedRealtime());
        try (InputStream in = connection.getInputStream(); FileOutputStream out = new FileOutputStream(target, false)) {
            copyAddonBytes(in, out, downloaded, lastReport);
            out.getFD().sync();
        } finally {
            connection.disconnect();
        }
        nativeAddonDownloadProgress(downloaded.get());
    }

    private void downloadAddonRanges(String address, File target, long total) throws IOException {
        final int parts = 4;
        final AtomicLong downloaded = new AtomicLong();
        final AtomicLong lastReport = new AtomicLong(android.os.SystemClock.elapsedRealtime());
        try (RandomAccessFile output = new RandomAccessFile(target, "rw")) {
            output.setLength(total);
        }
        ExecutorService executor = Executors.newFixedThreadPool(parts);
        List<Future<Void>> jobs = new ArrayList<>();
        try {
            for (int index = 0; index < parts; index++) {
                final long start = total * index / parts;
                final long end = total * (index + 1) / parts - 1;
                jobs.add(executor.submit(new Callable<Void>() {
                    @Override public Void call() throws Exception {
                        downloadAddonRange(address, target, start, end, downloaded, lastReport);
                        return null;
                    }
                }));
            }
            for (Future<Void> job : jobs) job.get();
        } catch (Exception e) {
            for (Future<Void> job : jobs) job.cancel(true);
            throw new IOException("parallel download failed", e);
        } finally {
            executor.shutdownNow();
        }
        nativeAddonDownloadProgress(downloaded.get());
    }

    private void downloadAddonRange(String address, File target, long start, long end, AtomicLong downloaded, AtomicLong lastReport) throws IOException {
        HttpURLConnection connection = (HttpURLConnection)new URL(address).openConnection();
        try {
            connection.setInstanceFollowRedirects(true);
            connection.setConnectTimeout(15000);
            connection.setReadTimeout(5000);
            connection.setRequestProperty("Accept-Encoding", "identity");
            connection.setRequestProperty("Range", "bytes=" + start + "-" + end);
            if (connection.getResponseCode() != HttpURLConnection.HTTP_PARTIAL) throw new IOException("server rejected byte range");
            try (InputStream in = connection.getInputStream(); RandomAccessFile out = new RandomAccessFile(target, "rw")) {
                out.seek(start);
                copyAddonBytes(in, out, downloaded, lastReport);
            }
        } finally {
            connection.disconnect();
        }
    }

    private void copyAddonBytes(InputStream in, FileOutputStream out, AtomicLong downloaded, AtomicLong lastReport) throws IOException {
        byte[] buffer = new byte[65536];
        int count;
        while (true) {
            ensureAddonDownloadActive();
            count = in.read(buffer);
            if (count == -1) break;
            out.write(buffer, 0, count);
            reportAddonProgress(downloaded.addAndGet(count), lastReport);
        }
    }

    private void copyAddonBytes(InputStream in, RandomAccessFile out, AtomicLong downloaded, AtomicLong lastReport) throws IOException {
        byte[] buffer = new byte[65536];
        int count;
        while (true) {
            ensureAddonDownloadActive();
            count = in.read(buffer);
            if (count == -1) break;
            out.write(buffer, 0, count);
            reportAddonProgress(downloaded.addAndGet(count), lastReport);
        }
    }

    private void ensureAddonDownloadActive() throws IOException {
        if (nativeAddonDownloadCancelled()) throw new IOException("download cancelled");
    }
    private void reportAddonProgress(long downloaded, AtomicLong lastReport) {
        long now = android.os.SystemClock.elapsedRealtime();
        long previous = lastReport.get();
        if (now - previous >= 1000 && lastReport.compareAndSet(previous, now)) nativeAddonDownloadProgress(downloaded);
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
