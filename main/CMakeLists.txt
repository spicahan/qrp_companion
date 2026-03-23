idf_component_register(
    SRCS "main_tab5.cpp" "pal_tab5.cpp" "uac_host.c" "../core/app.cpp" "../core/dsp.cpp" "../core/fft.cpp"
    INCLUDE_DIRS "." "../pal" "../core"
    REQUIRES M5Unified
)
