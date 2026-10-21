package org.xtframe.view;

import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.view.DisplayCutout;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;

import java.util.ArrayList;

import org.libsdl.app.SDLActivity;

/**
 * XTFrame View (T-240, docs/design/23-host-integration.md section 23.11.1): SDL's activity with
 * the XTFrame runtime statically linked into libmain.so. Intent extras become the native
 * arguments: XT_BUNDLE (a bundle directory on the device instead of the APK's assets/bundle),
 * XT_UI (the UI id to show) and XT_STATS ("0" turns the once-per-second frame-time log off).
 */
public class XTFrameActivity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        // SDL2 is linked statically into libmain.so (no libSDL2.so).
        return new String[] {"main"};
    }

    @Override
    protected String[] getArguments() {
        ArrayList<String> args = new ArrayList<>();
        Intent intent = getIntent();
        if (intent != null) {
            addExtra(intent, "XT_BUNDLE", "--bundle", args);
            addExtra(intent, "XT_UI", "--ui", args);
            addExtra(intent, "XT_STATS", "--stats", args);
        }
        return args.toArray(new String[0]);
    }

    private static void addExtra(Intent intent, String extra, String option, ArrayList<String> args) {
        String value = intent.getStringExtra(extra);
        if (value != null && !value.isEmpty()) {
            args.add(option);
            args.add(value);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (Build.VERSION.SDK_INT >= 28) {
            // Draw under the display cutout; the UI keeps clear of it through its safe area.
            WindowManager.LayoutParams attributes = getWindow().getAttributes();
            attributes.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(attributes);
        }
    }

    /**
     * Called from native code (JNI): the safe-area insets as fractions of the window, in the order
     * top, right, bottom, left -- the larger of the display cutout's safe insets and the system
     * window insets per side. All 0 below API 28 or before the window is laid out.
     */
    @SuppressWarnings("deprecation")
    public float[] safeInsets() {
        float[] out = new float[4];
        if (Build.VERSION.SDK_INT < 28) {
            return out;
        }
        View decor = getWindow().getDecorView();
        WindowInsets insets = decor.getRootWindowInsets();
        int width = decor.getWidth();
        int height = decor.getHeight();
        if (insets == null || width <= 0 || height <= 0) {
            return out;
        }
        int top = insets.getSystemWindowInsetTop();
        int right = insets.getSystemWindowInsetRight();
        int bottom = insets.getSystemWindowInsetBottom();
        int left = insets.getSystemWindowInsetLeft();
        DisplayCutout cutout = insets.getDisplayCutout();
        if (cutout != null) {
            top = Math.max(top, cutout.getSafeInsetTop());
            right = Math.max(right, cutout.getSafeInsetRight());
            bottom = Math.max(bottom, cutout.getSafeInsetBottom());
            left = Math.max(left, cutout.getSafeInsetLeft());
        }
        out[0] = Math.min(0.5f, top / (float) height);
        out[1] = Math.min(0.5f, right / (float) width);
        out[2] = Math.min(0.5f, bottom / (float) height);
        out[3] = Math.min(0.5f, left / (float) width);
        return out;
    }
}
