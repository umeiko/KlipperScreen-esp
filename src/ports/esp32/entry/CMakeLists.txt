idf_component_register(SRCS "app_main.c" "debug_cli.c" "web_config.c" "ota_auto.c"
                       INCLUDE_DIRS "."
                       REQUIRES bsp ui core esp_http_server esp_jpeg esp_http_client
                       PRIV_REQUIRES app_update esp_app_format)