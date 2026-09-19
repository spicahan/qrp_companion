set(app_sources "main_tab5.cpp" "pal_tab5.cpp"
    "../core/app.cpp" "../core/dsp.cpp" "../core/nco.cpp"
    "../core/ui_state.cpp" "../core/ui_widget.cpp")
set(app_includes "." "../pal" "../core")
set(app_deps espressif__esp-dsp esp_driver_ppa)
if(CONFIG_QRP_PLATFORM_TAB5_DX)
    list(APPEND app_sources
        "rf_board/rf_engine.cpp" "rf_board/rf_board.cpp" "rf_board/rf_audio.cpp"
        "rf_board/si5351.c" "rf_board/pcm1808_i2s.c"
        "rf_board/lpf_classify.c" "rf_board/lpf_sense.c")
    list(APPEND app_includes "rf_board" "rf_board/include")
    list(APPEND app_deps esp_adc esp_driver_i2s esp_driver_gpio esp_timer nvs_flash)
else()
    list(APPEND app_sources "uac_host.c" "cat_host.c" "pc_link.c"
        "../core/cat.cpp" "../pal/radio_qmx.cpp")
endif()
idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS ${app_includes}
    REQUIRES M5Unified
    PRIV_REQUIRES ${app_deps}
)
