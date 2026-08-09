package com.ermac.ironwail;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
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

import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.InputMethodManager;
import android.graphics.Canvas;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Paint;
import android.widget.FrameLayout;
import android.media.AudioManager;
import java.io.BufferedReader;
import java.io.FileReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileInputStream;
import java.util.Properties;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import org.libsdl.app.SDL;


import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public final class GLES3JNIActivity extends Activity {
    static { System.loadLibrary("ironrift"); }

    private GLSurfaceView view;
    private FrameLayout root;
    private android.widget.EditText keyboardInput;
    private boolean keyboardVisible;
    private IronRiftTouchOverlay overlay;
    private boolean nativeStarted;
    private AudioManager audioManager;
    private AudioManager.OnAudioFocusChangeListener audioFocusListener;
    private static final int STORAGE_ACCESS_REQUEST = 4101;

    private static native void nativeInit(String dataDir, String[] launchArgs);
    private static native void nativeResize(int width, int height);
    private static native void nativeRender();
    private static native void nativeKey(int keycode, boolean down);
    private static native void nativeText(String text);
    private static native void nativeAction(int action, boolean down);
    private static native void nativeCommand(String command);
    private static native void nativeAxis(int deviceId, int axis, float value);
    private static native void nativeTouch(int action, float x, float y);
    private static native void nativeTouchPointer(int action, int pointerId, float x, float y);
    private static native void nativeLook(int deltaX, int deltaY);
    private static native void nativeContextRestored();
    private static native void nativeSurfaceDestroyed();
    private static native void nativePause(boolean paused);
    private static native void nativeAudioFocus(boolean focused);
    private static native int nativeScreenMode();
    private static native void nativeShutdown();

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        enableImmersiveFullscreen();
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN, WindowManager.LayoutParams.FLAG_FULLSCREEN);


        if (requiresStorageAccess()) {
            requestStorageAccess();
            return;
        }
SDL.setupJNI();
        SDL.initialize();
        SDL.setContext(this);
        audioManager = (AudioManager)getSystemService(AUDIO_SERVICE);
        audioFocusListener = change -> { boolean focused = change == AudioManager.AUDIOFOCUS_GAIN; if (view != null) view.queueEvent(() -> nativeAudioFocus(focused)); };

        final File dataDir = prepareDataDir();
        final String[] launchArgs = readCommandLine(dataDir);

        view = new GLSurfaceView(this);
        view.setEGLContextClientVersion(3);
        view.setPreserveEGLContextOnPause(false);
        view.setFocusableInTouchMode(true);
        view.setRenderer(new GLSurfaceView.Renderer() {
            public void onSurfaceCreated(GL10 gl, EGLConfig config) {
                if (!nativeStarted) nativeInit(dataDir.getAbsolutePath(), launchArgs); else nativeContextRestored();
                nativeStarted = true;
                if (view.getWidth() > 0 && view.getHeight() > 0)
                    nativeResize(view.getWidth(), view.getHeight());
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
            final int action = event.getActionMasked();
            final float x = event.getX();
            final float y = event.getY();
            final int pointerIndex = event.getActionIndex();
            final int pointerId = event.getPointerId(pointerIndex);
            view.queueEvent(() -> nativeTouchPointer(action, pointerId, x, y));
            return true;
        });
        root = new FrameLayout(this);
        root.addView(view, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setupKeyboardInput();
        overlay = new IronRiftTouchOverlay();
        overlay.setLayerType(View.LAYER_TYPE_SOFTWARE, null);
        root.addView(overlay, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(root);
        enableImmersiveFullscreen();
    }

    private void setupKeyboardInput() {
        keyboardInput = new android.widget.EditText(this);
        keyboardInput.setSingleLine(true);
        keyboardInput.setTextSize(1);
        keyboardInput.setAlpha(.01f);
        keyboardInput.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        keyboardInput.addTextChangedListener(new android.text.TextWatcher() {
            private boolean clearing;
            @Override public void beforeTextChanged(CharSequence text, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence text, int start, int before, int count) {
                if (!clearing && count > 0 && view != null) {
                    final String typed = text.subSequence(start, start + count).toString();
                    view.queueEvent(() -> nativeText(typed));
                }
            }
            @Override public void afterTextChanged(android.text.Editable text) {
                if (text.length() > 0) {
                    clearing = true;
                    text.clear();
                    clearing = false;
                }
            }
        });
        keyboardInput.setOnKeyListener((ignored, keyCode, event) -> {
            if (keyCode != KeyEvent.KEYCODE_DEL)
                return false;
            if (view != null)
                view.queueEvent(() -> nativeKey(keyCode, event.getAction() == KeyEvent.ACTION_DOWN));
            return true;
        });
        keyboardInput.setOnEditorActionListener((ignored, actionId, event) -> {
            if (view != null) view.queueEvent(() -> { nativeKey(KeyEvent.KEYCODE_ENTER, true); nativeKey(KeyEvent.KEYCODE_ENTER, false); });
            return true;
        });
        root.addView(keyboardInput, new FrameLayout.LayoutParams(1, 1));
    }

    private void toggleSoftKeyboard() {
        if (keyboardInput == null || view == null) return;
        InputMethodManager input = (InputMethodManager)getSystemService(INPUT_METHOD_SERVICE);
        if (keyboardVisible) {
            input.hideSoftInputFromWindow(keyboardInput.getWindowToken(), 0);
            keyboardVisible = false;
            view.requestFocus();
        } else {
            keyboardInput.requestFocus();
            input.showSoftInput(keyboardInput, InputMethodManager.SHOW_IMPLICIT);
            keyboardVisible = true;
        }
    }
    private void enableImmersiveFullscreen() {
        Window window = getWindow();
        if (Build.VERSION.SDK_INT >= 30) window.setDecorFitsSystemWindows(false);
        if (Build.VERSION.SDK_INT >= 28) { WindowManager.LayoutParams lp = window.getAttributes(); lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES; window.setAttributes(lp); }
        window.setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN, WindowManager.LayoutParams.FLAG_FULLSCREEN);
        int flags = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION;
        window.getDecorView().setSystemUiVisibility(flags);
    }

    private boolean requiresStorageAccess() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager();
    }

    private void requestStorageAccess() {
        try {
            Intent intent = new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
            intent.setData(Uri.parse("package:" + getPackageName()));
            startActivityForResult(intent, STORAGE_ACCESS_REQUEST);
        } catch (Exception e) {
            Log.e("Ironwail", "Unable to open storage permission settings", e);
        }
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == STORAGE_ACCESS_REQUEST) {
            if (requiresStorageAccess()) recreate();
            else Log.e("Ironwail", "Storage access was denied; Ironwail will not start");
        }
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
        extractEnginePak(dataDir);
        extractSharewarePak(dataDir);
        return dataDir;
    }

    private String[] readCommandLine(File dataDir) {
        File commandLine = new File(dataDir, "commandline.txt");
        if (!commandLine.isFile()) {
            createDefaultCommandLine(commandLine);
            return new String[0];
        }

        StringBuilder text = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new FileReader(commandLine))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (text.length() > 0)
                    text.append(" ");
                text.append(line);
            }
        } catch (IOException e) {
            Log.w("Ironwail", "Could not read commandline.txt: " + e.getMessage());
            return new String[0];
        }

        List<String> tokens = tokenizeCommandLine(text.toString());
        if (tokens.size() <= 1) {
            Log.w("Ironwail", "commandline.txt needs an executable name followed by launch arguments");
            return new String[0];
        }
        tokens.remove(0);
        Log.i("Ironwail", "Loaded " + tokens.size() + " launch arguments from commandline.txt");
        return tokens.toArray(new String[0]);
    }

    private void createDefaultCommandLine(File commandLine) {
        try (FileOutputStream out = new FileOutputStream(commandLine)) {
            out.write("ironwail\n".getBytes("UTF-8"));
            Log.i("Ironwail", "Created empty commandline.txt");
        } catch (IOException e) {
            Log.w("Ironwail", "Could not create commandline.txt: " + e.getMessage());
        }
    }

    private static List<String> tokenizeCommandLine(String text) {
        List<String> tokens = new ArrayList<>();
        StringBuilder token = new StringBuilder();
        char quote = 0;
        for (int i = 0; i < text.length(); ++i) {
            char c = text.charAt(i);
            if (quote != 0) {
                if (c == quote)
                    quote = 0;
                else
                    token.append(c);
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (Character.isWhitespace(c)) {
                if (token.length() > 0) {
                    tokens.add(token.toString());
                    token.setLength(0);
                }
            } else {
                token.append(c);
            }
        }
        if (token.length() > 0)
            tokens.add(token.toString());
        return tokens;
    }


    private void extractEnginePak(File dataDir) {
        File pak = new File(dataDir, "ironwail.pak");
        if (pak.isFile())
            return;
        File temp = new File(dataDir, "ironwail.pak.part");
        try (InputStream in = getAssets().open("ironwail.pak");
             FileOutputStream out = new FileOutputStream(temp)) {
            byte[] buffer = new byte[65536];
            int count;
            while ((count = in.read(buffer)) != -1)
                out.write(buffer, 0, count);
            if (!temp.renameTo(pak))
                temp.delete();
        } catch (IOException e) {
            temp.delete();
            Log.w("Ironwail", "Could not extract engine support pak: " + e.getMessage());
        }
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
        if (keyCode == KeyEvent.KEYCODE_VOLUME_UP || keyCode == KeyEvent.KEYCODE_VOLUME_DOWN) {
            if (audioManager != null) audioManager.adjustStreamVolume(AudioManager.STREAM_MUSIC, keyCode == KeyEvent.KEYCODE_VOLUME_UP ? AudioManager.ADJUST_RAISE : AudioManager.ADJUST_LOWER, AudioManager.FLAG_SHOW_UI);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_VOLUME_MUTE)
            return super.onKeyDown(keyCode, event);
        if (view != null) view.queueEvent(() -> nativeKey(keyCode, true));
        return true;
    }

    @Override public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_VOLUME_UP || keyCode == KeyEvent.KEYCODE_VOLUME_DOWN)
            return true;
        if (keyCode == KeyEvent.KEYCODE_VOLUME_MUTE)
            return super.onKeyUp(keyCode, event);
        if (view != null) view.queueEvent(() -> nativeKey(keyCode, false));
        return true;
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) != 0) {
            InputDevice device = event.getDevice();
            int deviceId = device != null ? device.getId() : -1;
            view.queueEvent(() -> nativeAxis(deviceId, MotionEvent.AXIS_X, event.getAxisValue(MotionEvent.AXIS_X)));
            view.queueEvent(() -> nativeAxis(deviceId, MotionEvent.AXIS_Y, event.getAxisValue(MotionEvent.AXIS_Y)));
            view.queueEvent(() -> nativeAxis(deviceId, MotionEvent.AXIS_Z, event.getAxisValue(MotionEvent.AXIS_Z)));
            view.queueEvent(() -> nativeAxis(deviceId, MotionEvent.AXIS_RZ, event.getAxisValue(MotionEvent.AXIS_RZ)));
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override protected void onPause() {
        if (overlay != null) overlay.releaseAll();
        if (view != null) view.queueEvent(() -> { nativeSurfaceDestroyed(); nativePause(true); nativeAudioFocus(false); });
        super.onPause();
        if (view != null) view.onPause();
    }

    @Override protected void onResume() {
        super.onResume();
        if (view != null) view.onResume();
        if (audioManager != null) audioManager.requestAudioFocus(audioFocusListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN);
        if (view != null) view.queueEvent(() -> nativePause(false));
    }

    @Override public void onWindowFocusChanged(boolean hasFocus) { super.onWindowFocusChanged(hasFocus); if (hasFocus) enableImmersiveFullscreen(); }

    @Override protected void onDestroy() {
        if (nativeStarted && view != null)
            view.queueEvent(GLES3JNIActivity::nativeShutdown);
        if (audioManager != null) audioManager.abandonAudioFocus(audioFocusListener);
        SDL.setContext(null);
        super.onDestroy();
    }
    private final class IronRiftTouchOverlay extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Bitmap moveIcon, runIcon, shootIcon, crouchIcon, jumpIcon, backIcon, gearIcon, eyeIcon, hideIcon, resetIcon, closeIcon, prevIcon, nextIcon, consoleIcon, keyboardIcon, saveIcon, loadGameIcon;
        private int movePointer = -1, menuAxis;
        private int dragTarget = -1;
        private float moveX=.14053717f, moveY=.77580464f, runX=.295f, runY=.77580464f, shootX=.8026234f, shootY=.71920085f, jumpX=.9294191f, jumpY=.69256383f, downX=.9294191f, downY=.8523862f, backX=.068707064f, backY=.1154273f, consoleX=.16801998f, consoleY=.10876803f, prevX=.88819486f, prevY=.37735847f, nextX=.88819486f, nextY=.4861265f, keyboardX=.2604622f, keyboardY=.10876803f, saveX=.66f, saveY=.12f, loadX=.75f, loadY=.12f;
        private float moveOriginX, moveOriginY, lastLookX, lastLookY, lookAccumX, lookAccumY;
        private int lookPointer = -1;
        private boolean attack, menuShoot, swimDown, jump, menu, console, editMode, hidden;
        private void postKey(int key, boolean down) { view.queueEvent(() -> nativeKey(key, down)); }
        private void postAction(int action, boolean down) { view.queueEvent(() -> nativeAction(action, down)); }
        private void menuTap(int key) { postKey(key, true); postKey(key, false); }
        private void postCommand(String command) { view.queueEvent(() -> nativeCommand(command)); }
        private void postLook(float dx, float dy) { lookAccumX += dx; lookAccumY += dy; int ix = (int)lookAccumX; int iy = (int)lookAccumY; lookAccumX -= ix; lookAccumY -= iy; if (ix != 0 || iy != 0) view.queueEvent(() -> nativeLook(ix, iy)); }
        private void postMove(float dx, float dy) { float scale = Math.min(1.0f, (float)Math.sqrt(dx * dx + dy * dy) / (getWidth() * .12f)); if (scale == 0) { view.queueEvent(() -> { nativeAxis(-1, 0, 0); nativeAxis(-1, 1, 0); }); return; } float nx = dx / (float)Math.sqrt(dx * dx + dy * dy) * scale; float ny = dy / (float)Math.sqrt(dx * dx + dy * dy) * scale; view.queueEvent(() -> { nativeAxis(-1, 0, nx); nativeAxis(-1, 1, ny); }); }
        private Bitmap loadIcon(String name) { try { return BitmapFactory.decodeStream(getAssets().open("touchwiz/" + name)); } catch (IOException e) { Log.w("IronRiftTouch", "missing touch asset " + name, e); return null; } }
        IronRiftTouchOverlay() { super(GLES3JNIActivity.this); setFocusable(false); paint.setTextAlign(Paint.Align.CENTER); moveIcon = loadIcon("fixed_stick_circle.png"); runIcon = loadIcon("run.png"); shootIcon = loadIcon("shoot.png"); crouchIcon = loadIcon("crouch.png"); jumpIcon = loadIcon("jump.png"); backIcon = loadIcon("back_button.png"); loadWidgets(); gearIcon = loadIcon("settings.png"); eyeIcon = loadIcon("edit_show.png"); hideIcon = loadIcon("edit_hide.png"); resetIcon = loadIcon("edit_reset.png"); closeIcon = loadIcon("edit_close.png"); prevIcon=loadIcon("prev_weap.png"); nextIcon=loadIcon("next_weap.png"); consoleIcon=loadIcon("console.png"); keyboardIcon=loadIcon("keyboard.png"); saveIcon=loadIcon("save.png"); loadGameIcon=loadIcon("load.png"); }
        private int widgetAt(float x, float y) { float[][] p={{moveX,moveY},{runX,runY},{shootX,shootY},{jumpX,jumpY},{downX,downY},{backX,backY},{consoleX,consoleY},{prevX,prevY},{nextX,nextY},{keyboardX,keyboardY},{saveX,saveY},{loadX,loadY}}; for(int i=0;i<p.length;i++){ float dx=x-p[i][0],dy=y-p[i][1];if(dx*dx+dy*dy<.012f)return i;} return -1; }
        private void setWidgetPosition(int n,float x,float y) { switch(n){case 0:moveX=x;moveY=y;break;case 1:runX=x;runY=y;break;case 2:shootX=x;shootY=y;break;case 3:jumpX=x;jumpY=y;break;case 4:downX=x;downY=y;break;case 5:backX=x;backY=y;break;case 6:consoleX=x;consoleY=y;break;case 7:prevX=x;prevY=y;nextX=x;break;case 8:nextX=x;nextY=y;prevX=x;break;case 9:keyboardX=x;keyboardY=y;break;case 10:saveX=x;saveY=y;break;case 11:loadX=x;loadY=y;break;} }
        private File widgetConfig() { return new File("/sdcard/ironwail/widgets.cfg"); }
        private void loadWidgets() { File cfg=widgetConfig(); if (!cfg.exists()) { saveWidgets(); return; } try (FileInputStream in=new FileInputStream(cfg)) { Properties p=new Properties(); p.load(in); moveX=Float.parseFloat(p.getProperty("move.x",Float.toString(moveX)));moveY=Float.parseFloat(p.getProperty("move.y",Float.toString(moveY)));runX=Float.parseFloat(p.getProperty("run.x",Float.toString(runX)));runY=Float.parseFloat(p.getProperty("run.y",Float.toString(runY)));shootX=Float.parseFloat(p.getProperty("shoot.x",Float.toString(shootX)));shootY=Float.parseFloat(p.getProperty("shoot.y",Float.toString(shootY)));jumpX=Float.parseFloat(p.getProperty("jump.x",Float.toString(jumpX)));jumpY=Float.parseFloat(p.getProperty("jump.y",Float.toString(jumpY)));downX=Float.parseFloat(p.getProperty("down.x",Float.toString(downX)));downY=Float.parseFloat(p.getProperty("down.y",Float.toString(downY)));backX=Float.parseFloat(p.getProperty("back.x",Float.toString(backX)));backY=Float.parseFloat(p.getProperty("back.y",Float.toString(backY)));consoleX=Float.parseFloat(p.getProperty("console.x",Float.toString(consoleX)));consoleY=Float.parseFloat(p.getProperty("console.y",Float.toString(consoleY)));prevX=Float.parseFloat(p.getProperty("prev.x",Float.toString(prevX)));prevY=Float.parseFloat(p.getProperty("prev.y",Float.toString(prevY)));nextX=Float.parseFloat(p.getProperty("next.x",Float.toString(nextX)));nextY=Float.parseFloat(p.getProperty("next.y",Float.toString(nextY)));keyboardX=Float.parseFloat(p.getProperty("keyboard.x",Float.toString(keyboardX)));keyboardY=Float.parseFloat(p.getProperty("keyboard.y",Float.toString(keyboardY)));saveX=Float.parseFloat(p.getProperty("save.x",Float.toString(saveX)));saveY=Float.parseFloat(p.getProperty("save.y",Float.toString(saveY)));loadX=Float.parseFloat(p.getProperty("load.x",Float.toString(loadX)));loadY=Float.parseFloat(p.getProperty("load.y",Float.toString(loadY))); prevX=nextX; saveWidgets(); } catch(Exception ignored) {} }
        private void saveWidgets() { try { Properties p=new Properties(); p.setProperty("move.x",Float.toString(moveX));p.setProperty("move.y",Float.toString(moveY));p.setProperty("run.x",Float.toString(runX));p.setProperty("run.y",Float.toString(runY));p.setProperty("shoot.x",Float.toString(shootX));p.setProperty("shoot.y",Float.toString(shootY));p.setProperty("jump.x",Float.toString(jumpX));p.setProperty("jump.y",Float.toString(jumpY));p.setProperty("down.x",Float.toString(downX));p.setProperty("down.y",Float.toString(downY));p.setProperty("back.x",Float.toString(backX));p.setProperty("back.y",Float.toString(backY));p.setProperty("console.x",Float.toString(consoleX));p.setProperty("console.y",Float.toString(consoleY));p.setProperty("prev.x",Float.toString(prevX));p.setProperty("prev.y",Float.toString(prevY));p.setProperty("next.x",Float.toString(nextX));p.setProperty("next.y",Float.toString(nextY));p.setProperty("keyboard.x",Float.toString(keyboardX));p.setProperty("keyboard.y",Float.toString(keyboardY));p.setProperty("save.x",Float.toString(saveX));p.setProperty("save.y",Float.toString(saveY));p.setProperty("load.x",Float.toString(loadX));p.setProperty("load.y",Float.toString(loadY)); File f=widgetConfig();File parent=f.getParentFile();if(parent!=null)parent.mkdirs();try(FileOutputStream out=new FileOutputStream(f)){p.store(out,"IronRift touch widget positions");} }catch(Exception ignored){} }
        private boolean hitOverlay(float x, float y, float cx, float cy, float radius) { float dx=x-cx, dy=y-cy; return dx*dx+dy*dy <= radius*radius; }
        private void drawIcon(Canvas canvas, Bitmap bitmap, float cx, float cy, float size) { if (bitmap == null) return; android.graphics.RectF dst = new android.graphics.RectF(cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2); canvas.drawBitmap(bitmap, null, dst, paint); }
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            int w = getWidth(), h = getHeight();
            paint.setAlpha(190); if (!hidden) { drawIcon(canvas, moveIcon, w * moveX, h * moveY, Math.min(w,h) * .24f); drawIcon(canvas, runIcon, w * runX, h * runY, Math.min(w,h) * .12f); }
            paint.setAlpha(190); if (!hidden) { drawIcon(canvas, shootIcon, w * shootX, h * shootY, Math.min(w,h) * .18f); drawIcon(canvas, crouchIcon, w * downX, h * downY, Math.min(w,h) * .12f); drawIcon(canvas, jumpIcon, w * jumpX, h * jumpY, Math.min(w,h) * .12f); drawIcon(canvas, backIcon, w * backX, h * backY, Math.min(w,h) * .12f); }
            if (!hidden) { drawIcon(canvas, consoleIcon, w*consoleX, h*consoleY, Math.min(w,h)*.12f); drawIcon(canvas, keyboardIcon, w*keyboardX, h*keyboardY, Math.min(w,h)*.12f); drawIcon(canvas, saveIcon, w*saveX, h*saveY, Math.min(w,h)*.12f); drawIcon(canvas, loadGameIcon, w*loadX, h*loadY, Math.min(w,h)*.12f); drawIcon(canvas, prevIcon, w*prevX, h*prevY, Math.min(w,h)*.15f); drawIcon(canvas, nextIcon, w*nextX, h*nextY, Math.min(w,h)*.15f); }
            if (!hidden) drawIcon(canvas, gearIcon, w * .84f, h * .12f, Math.min(w,h) * .12f); drawIcon(canvas, hidden ? hideIcon : eyeIcon, w * .94f, h * .12f, Math.min(w,h) * .12f); if (editMode) { drawIcon(canvas, resetIcon, w * .84f, h * .25f, Math.min(w,h) * .12f); drawIcon(canvas, closeIcon, w * .94f, h * .25f, Math.min(w,h) * .12f); }
        }
        void releaseAll() {
            movePointer = lookPointer = -1; menuAxis = 0;
            postAction(0, false); postAction(3, false);
            postAction(2, false);
            postKey(KeyEvent.KEYCODE_BACK, false); postKey(KeyEvent.KEYCODE_GRAVE, false);
            lookAccumX = lookAccumY = 0; postLook(0, 0); attack = menuShoot = jump = menu = console = false; invalidate();
        }
        public boolean onTouchEvent(MotionEvent e) {
            int action = e.getActionMasked(), index = e.getActionIndex(), id = e.getPointerId(index);
            float x = e.getX(index), y = e.getY(index); int w = getWidth(), h = getHeight();
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                float topSize = Math.min(w, h) * .07f;
                if (hitOverlay(x, y, w * .84f, h * .12f, topSize)) { editMode = !editMode; invalidate(); return true; }
                if (hitOverlay(x, y, w * .94f, h * .12f, topSize)) { hidden = !hidden; invalidate(); return true; }
                if (editMode && hitOverlay(x, y, w * .84f, h * .25f, topSize)) { moveX=.16f;moveY=.78f;runX=.30f;runY=.78f;shootX=.82f;shootY=.78f;jumpX=.93f;jumpY=.78f;downX=.96f;downY=.88f;backX=.12f;backY=.12f;consoleX=.28f;consoleY=.12f;prevX=.90f;prevY=.42f;nextX=.90f;nextY=.55f;keyboardX=.34f;keyboardY=.12f;saveX=.66f;saveY=.12f;loadX=.75f;loadY=.12f;saveWidgets();invalidate(); return true; }
                if (editMode && hitOverlay(x, y, w * .94f, h * .25f, topSize)) { editMode = false; saveWidgets(); invalidate(); return true; }
                if (editMode) { dragTarget = widgetAt(x / w, y / h); return true; }
                float controlRadius = Math.min(w, h) * .09f;
                if (hitOverlay(x,y,w*runX,h*runY,controlRadius)) { postAction(4,true); return true; }
                if (hitOverlay(x,y,w*shootX,h*shootY,controlRadius)) { if (nativeScreenMode() != 0) { menuShoot=true; postKey(KeyEvent.KEYCODE_ENTER, true); } else { lookPointer=id; lastLookX=x; lastLookY=y; attack=true; postAction(0,true); } return true; }

                if (hitOverlay(x,y,w*jumpX,h*jumpY,controlRadius)) { jump=true; postAction(2,true); return true; }
                if (hitOverlay(x,y,w*downX,h*downY,controlRadius)) { swimDown=true; postAction(3,true); return true; }
                if (hitOverlay(x,y,w*backX,h*backY,controlRadius)) { menu=true; postKey(KeyEvent.KEYCODE_BACK,true); return true; }
                if (hitOverlay(x,y,w*prevX,h*prevY,controlRadius)) { postCommand("impulse 10"); return true; }
                if (hitOverlay(x,y,w*nextX,h*nextY,controlRadius)) { postCommand("impulse 12"); return true; }
                if (hitOverlay(x,y,w*consoleX,h*consoleY,controlRadius)) { postKey(KeyEvent.KEYCODE_GRAVE,true); console=true; return true; }
                if (hitOverlay(x,y,w*keyboardX,h*keyboardY,controlRadius)) { toggleSoftKeyboard(); return true; }
                if (hitOverlay(x,y,w*saveX,h*saveY,controlRadius)) { postCommand("menu_save"); return true; }
                if (hitOverlay(x,y,w*loadX,h*loadY,controlRadius)) { postCommand("menu_load"); return true; }
                if (x < w * .34f && y > h * .60f) { movePointer = id; moveOriginX = x; moveOriginY = y; menuAxis = 0; postMove(0, 0); }
                else { lookPointer = id; lastLookX = x; lastLookY = y; }
                invalidate(); return true;
            }
            if (action == MotionEvent.ACTION_MOVE) {
                for (int i=0; i<e.getPointerCount(); ++i) {
                    int pointer = e.getPointerId(i);
                    if (dragTarget >= 0 && editMode) { setWidgetPosition(dragTarget, e.getX(i) / w, e.getY(i) / h); invalidate(); } if (pointer == movePointer) { float dx=e.getX(i)-moveOriginX, dy=e.getY(i)-moveOriginY; if (nativeScreenMode()!=0) { float ax=Math.abs(dx), ay=Math.abs(dy); if (menuAxis == 0 && Math.max(ax,ay) > 28) menuAxis = ax > ay * 1.5f ? 1 : 2; if (menuAxis == 1 && ax > 28) { menuTap(dx>0 ? KeyEvent.KEYCODE_DPAD_RIGHT : KeyEvent.KEYCODE_DPAD_LEFT); moveOriginX=e.getX(i); } if (menuAxis == 2 && ay > 28) { menuTap(dy>0 ? KeyEvent.KEYCODE_DPAD_DOWN : KeyEvent.KEYCODE_DPAD_UP); moveOriginY=e.getY(i); } } else postMove(dx, dy); }
                    if (pointer == lookPointer) postLook(e.getX(i) - lastLookX, e.getY(i) - lastLookY);
                    if (pointer == lookPointer) { lastLookX = e.getX(i); lastLookY = e.getY(i); }
                }
                return true;
            }
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_CANCEL) {
                if (id == movePointer) { movePointer = -1; menuAxis = 0; postMove(0, 0); } if (id == e.getPointerId(e.getActionIndex())) { dragTarget = -1; saveWidgets(); }
                if (id == lookPointer) { lookPointer = -1; }
                if (menuShoot) { menuShoot=false; postKey(KeyEvent.KEYCODE_ENTER, false); } if (attack) { attack = false; postAction(0, false); }
                if (jump) { jump = false; postAction(2, false); }

                if (menu) { menu = false; postKey(KeyEvent.KEYCODE_BACK, false); }
                if (console) { console = false; postKey(KeyEvent.KEYCODE_GRAVE, false); }
                if (swimDown) { swimDown = false; postAction(3, false); }
                invalidate(); return true;
            }
            return true;
        }    }}
