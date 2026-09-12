# core 组件（ESP-IDF 构建时消费）：配置存储 + 打印机数据模型 + Moonraker 客户端
# desktop 后端不用本文件（在 ports/desktop/CMakeLists.txt 里直接列 mock 源文件）
idf_component_register(SRCS "app_settings.c"
                            "printer_model.c"
                            "moonraker_client.c"
                            "klipper_api.c"
                            "bambu_status.c"
                            "bambu_status_stream.c"
                            "bambu_monitor_stub.c"
                            "bambu_cloud_stub.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl json esp_websocket_client esp_timer
                       PRIV_REQUIRES bsp)
