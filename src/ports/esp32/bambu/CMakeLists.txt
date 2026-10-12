# Bambu Cloud port for ESP32: one bambu_net actor serializes HTTPS and MQTT.
idf_component_register(SRCS "bambu_cloud_esp32.c"
                            "bambu_monitor_esp32.c"
                            "bambu_runtime_esp32.c"
                            "bambu_http_esp32.c"
                            "bambu_secret_esp32.c"
                       INCLUDE_DIRS "."
                       REQUIRES core bsp json esp_http_client nvs_flash mbedtls heap mqtt esp_timer)

# Protocol/state-machine code is latency-insensitive.  Keep it size-optimized
# even on boards that build the display path with -O3; this preserves the
# existing 4 MB partition layout on the ESP32-C3 target.
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
