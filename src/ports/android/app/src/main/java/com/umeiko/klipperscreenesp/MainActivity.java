package com.umeiko.klipperscreenesp;

import android.content.Intent;
import android.net.Uri;
import androidx.core.content.FileProvider;
import java.io.File;

import org.libsdl.app.SDLActivity;

/**
 * KlipperScreen-esp —— Klipper 远程显示屏（Moonraker 控制端）。
 * 原生入口 libmain.so（src/ports/desktop/main.c 的 SDL_main）。
 */
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL2",
            "main"
        };
    }

    /** 自更新：用系统安装器打开已下载的 APK（self_update_android.c 经 JNI 调用） */
    public void installApk(String path) {
        File file = new File(path);
        Uri uri = FileProvider.getUriForFile(this,
                "com.umeiko.klipperscreenesp.fileprovider", file);
        Intent intent = new Intent(Intent.ACTION_VIEW);
        intent.setDataAndType(uri, "application/vnd.android.package-archive");
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                | Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivity(intent);
    }
}
