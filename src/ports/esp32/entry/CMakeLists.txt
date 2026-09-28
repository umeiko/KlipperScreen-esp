idf_component_register(SRCS "app_main.c" "debug_cli.c" "web_config.c"
                       INCLUDE_DIRS "."
                       REQUIRES bsp ui core esp_http_server esp_jpeg)