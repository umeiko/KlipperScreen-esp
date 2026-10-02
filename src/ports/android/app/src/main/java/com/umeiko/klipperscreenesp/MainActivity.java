package com.umeiko.klipperscreenesp;

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
}
