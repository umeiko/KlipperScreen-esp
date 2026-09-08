# BSP 组件（ESP-IDF 构建时消费；desktop 等其他后端忽略本文件）
# 板型选择：Kconfig CONFIG_BOARD_*（src/bsp/Kconfig.projbuild，menuconfig → Board selection）
# 注意：组件注册的第一遍扫描早于 kconfig 加载，CONFIG_BOARD_* 此时为空，
# 因此不能用 if(CONFIG_...) 决定 SRCS/REQUIRES —— 两个板型的源都编译，
# 文件内部用 #if CONFIG_BOARD_* 裁剪；依赖也全部列上（对端目标都能编译）。
idf_component_register(SRCS "esp32/bsp_cyd_2432s028r.c"
                            "esp32/bsp_e32r35t.c"
                            "esp32/bsp_esp32s3_st7789.c"
                            "esp32/bsp_jc8048w550.c"
                            "esp32/bsp_wifi_esp32.c"
                            "esp32/bsp_conf_littlefs.c"
                            "esp32/rgb44.c"
                       INCLUDE_DIRS "." "esp32"
                       REQUIRES lvgl esp_lcd
                                esp_lcd_ili9341 esp_lcd_st7796 esp_lcd_touch_xpt2046
                                espressif__esp_lcd_touch_gt911
                                esp_timer driver esp_common esp_wifi esp_event esp_netif nvs_flash
                                esp_http_client littlefs json
                                hal soc esp_mm esp_psram esp_hw_support esp_driver_gpio)
