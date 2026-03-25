idf_component_register(
    SRCS "main_tab5.cpp" "pal_tab5.cpp" "uac_host.c" "../core/app.cpp" "../core/dsp.cpp"
    INCLUDE_DIRS "." "../pal" "../core"
    REQUIRES M5Unified
    PRIV_REQUIRES espressif__esp-dsp esp_hw_support esp_lcd
)
