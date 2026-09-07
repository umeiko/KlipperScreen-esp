# 注意：不能加 CONFIGURE_DEPENDS——IDF 在 script 模式下 include 本文件时会报错。
# 新增源文件（字体/面板）后若链接报 undefined，touch 本文件触发 CMake 重配即可。
file(GLOB UI_SRCS "*.c" "panels/*.c" "widgets/*.c" "assets/*.c")
idf_component_register(SRCS ${UI_SRCS}
                       INCLUDE_DIRS "." "panels" "widgets"
                       REQUIRES lvgl core
                       PRIV_REQUIRES bsp)
