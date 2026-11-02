package com.bepinex.android.tools;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.os.Process;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;

import com.bepinex.android.BuildConfig;

import java.io.File;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Compatibility-only diagnostic for Oxide's Android application context.
 * Inspect metadata path existence, Android access results and supplied paths.
 * Does not read metadata bytes, modify files, install hooks or change protection.
 */
public final class OxidePathDiagnostics {
    private static final String TAG = "OxidePathDiag";
    private static final String GAME = "com.catsbit.oxidesurvivalisland";
    private static final String EXPECTED = "c8a2812a5d8d7ea0b94";
    private static final AtomicBoolean BEFORE_RECORDED = new AtomicBoolean();
    private static final AtomicBoolean WRAPPER_BEFORE_RECORDED = new AtomicBoolean();
    private static final AtomicBoolean WRAPPER_AFTER_RECORDED = new AtomicBoolean();

    private OxidePathDiagnostics() {}

    public static boolean enabled(Context game) {
        return BuildConfig.APPLICATION_ID.endsWith(".oxidepaths")
                && game != null && GAME.equals(game.getPackageName());
    }

    private static void probe(String label, File file) {
        try {
            android.system.StructStat stat = Os.stat(file.getAbsolutePath());
            boolean canRead;
            try {
                canRead = Os.access(file.getAbsolutePath(), OsConstants.R_OK);
            } catch (ErrnoException e) {
                Log.i(TAG, label + " access error errno=" + e.errno);
                canRead = false;
            }
            Log.i(TAG, label + " FOUND type=" +
                    (OsConstants.S_ISDIR(stat.st_mode) ? "dir" : "file") +
                    " mode=" + Integer.toOctalString(stat.st_mode & 0777) +
                    " bytes=" + stat.st_size +
                    " uid=" + stat.st_uid +
                    " readable=" + canRead);
        } catch (ErrnoException e) {
            Log.i(TAG, label + " NOT_ACCESSIBLE errno=" + e.errno +
                    " path=" + file.getAbsolutePath());
        } catch (Exception e) {
            Log.i(TAG, label + " check_failed=" + e.getClass().getSimpleName());
        }
    }

    public static void bootstrap(Context game, Context launcher) {
        if (!enabled(game) || !BEFORE_RECORDED.compareAndSet(false, true)) return;
        ApplicationInfo gi = game.getApplicationInfo();
        ApplicationInfo li = launcher.getApplicationInfo();
        Log.i(TAG, "=== original-context snapshot pid=" + Process.myPid() + " ===");
        Log.i(TAG, "game.package=" + game.getPackageName());
        Log.i(TAG, "game.applicationInfo.dataDir=" + gi.dataDir);
        Log.i(TAG, "game.applicationInfo.nativeLibraryDir=" + gi.nativeLibraryDir);
        Log.i(TAG, "launcher.applicationInfo.dataDir=" + li.dataDir);
        Log.i(TAG, "launcher.processUid=" + Process.myUid() +
                " originalGameUid=" + gi.uid);
        String original = gi.dataDir;
        if (original != null && !original.isEmpty()) {
            probe("original-root", new File(original));
            probe("original-metadata-folder", new File(original, "il2cpp/Metadata"));
            probe("original-expected-metadata", new File(original, "il2cpp/Metadata/" + EXPECTED));
            probe("original-files-metadata", new File(original, "files/il2cpp/Metadata/" + EXPECTED));
        }
        String redirected = new File(new File(android.os.Environment.getExternalStorageDirectory(), "BepInEx_Android/" + GAME), "PersistentData").getAbsolutePath();
        Log.i(TAG, "launcher-redirect-target=" + redirected);
        probe("redirected-folder", new File(redirected, "il2cpp/Metadata"));
        probe("redirected-expected-metadata", new File(redirected, "il2cpp/Metadata/" + EXPECTED));
        probe("redirected-global-metadata", new File(redirected, "il2cpp/Metadata/global-metadata.dat"));
    }

    public static void wrapperBefore(Context game) {
        if (!enabled(game) || !WRAPPER_BEFORE_RECORDED.compareAndSet(false, true)) return;
        Log.i(TAG, "wrapper BEFORE dataDir=" + game.getApplicationInfo().dataDir +
                " nativeLibraryDir=" + game.getApplicationInfo().nativeLibraryDir);
    }

    public static void wrapperAfter(Context game) {
        if (!enabled(game) || !WRAPPER_AFTER_RECORDED.compareAndSet(false, true)) return;
        Log.i(TAG, "wrapper AFTER dataDir=" + game.getApplicationInfo().dataDir +
                " nativeLibraryDir=" + game.getApplicationInfo().nativeLibraryDir);
        String redirected = game.getApplicationInfo().dataDir;
        if (redirected != null && !redirected.isEmpty()) {
            probe("wrapper-expected-metadata",
                    new File(redirected, "il2cpp/Metadata/" + EXPECTED));
        }
    }
}
