cmake_minimum_required(VERSION 3.16)
project(klipper_remote_desktop C)

# SDL2 desktop ports share every UI source. On Windows the primary target is
# a real Moonraker controller; a separately named simulator keeps mock data an
# explicit development tool.
set(CMAKE_C_STANDARD 99)

set(SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(REPO_ROOT ${SRC_ROOT}/..)

add_subdirectory(${REPO_ROOT}/third_party/lvgl lvgl)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(SDL2 REQUIRED)

file(GLOB UI_SRCS
    ${SRC_ROOT}/ui/*.c
    ${SRC_ROOT}/ui/panels/*.c
    ${SRC_ROOT}/ui/widgets/*.c
    ${SRC_ROOT}/ui/assets/*.c
)

set(DESKTOP_COMMON_SRCS
    main.c
    ${SRC_ROOT}/bsp/desktop/bsp_sdl2.c
    ${SRC_ROOT}/bsp/desktop/bsp_wifi_windows.c
    ${SRC_ROOT}/bsp/desktop/bsp_wifi_linux.c
    ${SRC_ROOT}/bsp/desktop/bsp_conf_file.c
    ${SRC_ROOT}/core/app_settings.c
    ${UI_SRCS}
)

function(configure_desktop_target target)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SRC_ROOT}/bsp
        ${SRC_ROOT}/core
        ${SRC_ROOT}/ui
        ${SRC_ROOT}/ui/panels
        ${SRC_ROOT}/ui/widgets
        ${SDL2_INCLUDE_DIRS}
    )
    target_link_libraries(${target} PRIVATE lvgl ${SDL2_LIBRARIES})
    if(WIN32)
        target_link_libraries(${target} PRIVATE mingw32 SDL2::SDL2main)
        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE FALSE)
    endif()
endfunction()

if(WIN32)
    find_package(cJSON CONFIG REQUIRED)

    add_executable(klipper_remote_desktop
        ${DESKTOP_COMMON_SRCS}
        ${SRC_ROOT}/core/printer_model.c
        ${SRC_ROOT}/core/klipper_api.c
        ${SRC_ROOT}/core/moonraker_client_winhttp.c
    )
    configure_desktop_target(klipper_remote_desktop)
    target_include_directories(klipper_remote_desktop PRIVATE ${CJSON_INCLUDE_DIR}/cjson)
    target_link_libraries(klipper_remote_desktop PRIVATE cjson-static winhttp)

    add_executable(klipper_remote_simulator
        ${DESKTOP_COMMON_SRCS}
        ${SRC_ROOT}/core/printer_mock.c
        ${SRC_ROOT}/core/moonraker_client_stub.c
    )
    configure_desktop_target(klipper_remote_simulator)
    target_compile_definitions(klipper_remote_simulator PRIVATE KLIPPER_DESKTOP_SIMULATOR=1)
else()
    # Linux remains the layout simulator until it gains a native transport.
    add_executable(klipper_remote_desktop
        ${DESKTOP_COMMON_SRCS}
        ${SRC_ROOT}/core/printer_mock.c
        ${SRC_ROOT}/core/moonraker_client_stub.c
    )
    configure_desktop_target(klipper_remote_desktop)
    target_compile_definitions(klipper_remote_desktop PRIVATE KLIPPER_DESKTOP_SIMULATOR=1)
endif()
