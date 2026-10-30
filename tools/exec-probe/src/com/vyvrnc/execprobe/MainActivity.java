package com.vyvrnc.execprobe;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.ByteArrayOutputStream;
import java.util.Arrays;
import java.util.List;

/** Runs each way of starting a program once and logs the result (tag ExecProbe). */
public class MainActivity extends Activity {
    private static final String TAG = "ExecProbe";
    private final StringBuilder report = new StringBuilder();
    private TextView view;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        view = new TextView(this);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(view);
        setContentView(scroll);
        new Thread(this::probe).start();
    }

    private void line(String s) {
        Log.i(TAG, s);
        report.append(s).append('\n');
        runOnUiThread(() -> view.setText(report));
    }

    private File extract(String asset, File dir) throws Exception {
        File out = new File(dir, asset);
        out.getParentFile().mkdirs();
        try (InputStream in = getAssets().open(asset); FileOutputStream os = new FileOutputStream(out)) {
            byte[] buf = new byte[65536];
            for (int n; (n = in.read(buf)) > 0; ) os.write(buf, 0, n);
        }
        out.setExecutable(true, false);
        return out;
    }

    private void run(String name, String... cmd) {
        try {
            Process p = new ProcessBuilder(Arrays.asList(cmd)).redirectErrorStream(true).start();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            InputStream in = p.getInputStream();
            byte[] buf = new byte[4096];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            int rc = p.waitFor();
            line(name + ": rc=" + rc + " out=" + out.toString().trim().replace('\n', '|'));
        } catch (Exception e) {
            line(name + ": EXCEPTION " + e.getMessage());
        }
    }

    private void probe() {
        try {
            String lib = getApplicationInfo().nativeLibraryDir;
            File data = new File(getFilesDir(), "probe");
            line("targetSdk=" + getApplicationInfo().targetSdkVersion + " nativeLibraryDir=" + lib);
            File libc = extract("glibc/libc.so.6", data);
            File glibcHello = extract("hello-glibc", data);
            File bionicHello = extract("hello-bionic", data);
            String libPath = libc.getParent();
            String ld = lib + "/libld.so";
            run("A direct exec of app data file (expect EACCES)", bionicHello.getPath());
            run("B packaged lib*.so exec (control)", lib + "/libhello.so");
            run("C glibc program via libld.so", ld, "--library-path", libPath, glibcHello.getPath());
            run("D glibc program spawning glibc child via libld.so", ld, "--library-path", libPath,
                glibcHello.getPath(), "spawn", ld, libPath, glibcHello.getPath());
            run("E bionic program via /system/bin/linker64", "/system/bin/linker64", bionicHello.getPath());
            run("F glibc program direct exec of data file (expect EACCES)", ld, "--library-path", libPath,
                glibcHello.getPath(), "spawn-direct", glibcHello.getPath());
            line("DONE");
        } catch (Exception e) {
            line("FAILED " + e);
        }
    }
}
