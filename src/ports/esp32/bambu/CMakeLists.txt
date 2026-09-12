# Bambu Cloud port for ESP32: bambu_net actor + bounded HTTPS + NVS profile。
# monitor 仍由 core 里的 bambu_monitor_stub.c 提供（提示词 4 才接入真实 MQTT）。
idf_component_register(SRCS "bambu_cloud_esp32.c"
                            "bambu_runtime_esp32.c"
                            "bambu_http_esp32.c"
                            "bambu_secret_esp32.c"
                       INCLUDE_DIRS "."
                       REQUIRES core bsp json esp_http_client nvs_flash mbedtls heap)
