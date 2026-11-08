package com.kurtisone.trials;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

/**
 * Loads the C++ runtime, the rexglue runtime (which carries SDL) and the game.
 *
 * The game folder (game/, <game>.toml, logs) is the first of:
 *   <internal storage>/<game>, <microSD>/<game>  (needs "All files access")
 *   Android/data/<package>/files                 (always allowed)
 * The native side picks it; this only asks for the permission once.
 */
public class TrialsActivity extends SDLActivity {
    // The game's name: the last part of the package name (com.kurtisone.trials_hd).
    private String gameName() {
        String name = getPackageName();
        return name.substring(name.lastIndexOf('.') + 1);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "c++_shared",
            "rexruntime",
            "main"
        };
    }

    // Full immersive: hide the status and navigation bars (a swipe from an
    // edge shows them for a moment) and draw under the display cutout.
    private void hideSystemBars() {
        Window window = getWindow();
        WindowManager.LayoutParams attributes = window.getAttributes();
        attributes.layoutInDisplayCutoutMode =
            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        window.setAttributes(attributes);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            window.getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemBars();
        }
    }

    // The folder the native side will pick (FindAppRoot), once the game is
    // there; before that, where the player is asked to put it.
    private File appFolder() {
        File internal = new File("/storage/emulated/0", gameName());
        if (new File(internal, "game/default.xex").canRead()) {
            return internal;
        }
        File[] volumes = new File("/storage").listFiles();
        if (volumes != null) {
            for (File volume : volumes) {
                File candidate = new File(volume, gameName());
                if (new File(candidate, "game/default.xex").canRead()) {
                    return candidate;
                }
            }
        }
        boolean allFiles = Build.VERSION.SDK_INT < Build.VERSION_CODES.R
            || Environment.isExternalStorageManager();
        return allFiles ? internal : getExternalFilesDir(null);
    }

    // Writes the packaged settings if the folder has none; never replaces them.
    private void installDefaultConfig() {
        File folder = appFolder();
        if (folder == null) {
            return;
        }
        File config = new File(folder, gameName() + ".toml");
        if (config.exists() || (!folder.isDirectory() && !folder.mkdirs())) {
            return;
        }
        try (InputStream in = getAssets().open("settings.toml");
             OutputStream out = new FileOutputStream(config)) {
            byte[] buffer = new byte[8192];
            for (int read; (read = in.read(buffer)) > 0;) {
                out.write(buffer, 0, read);
            }
        } catch (Exception e) {
            config.delete();
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        installDefaultConfig();
        super.onCreate(savedInstanceState);
        hideSystemBars();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                && !Environment.isExternalStorageManager()
                && !getPreferences(MODE_PRIVATE).getBoolean("asked_all_files", false)) {
            getPreferences(MODE_PRIVATE).edit().putBoolean("asked_all_files", true).apply();
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                         Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        }
    }
}
