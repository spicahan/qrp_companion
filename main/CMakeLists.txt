idf_component_register(
    SRCS "main_tab5.cpp" "pal_tab5.cpp" "uac_host.c" "cat_host.c"
         "../core/app.cpp" "../core/dsp.cpp" "../core/cat.cpp" "../core/nco.cpp"
         "../core/ui_state.cpp" "../core/ui_widget.cpp"
         "../core/web.cpp" "../core/mongoose/mongoose.c"
    INCLUDE_DIRS "." "../pal" "../core"
    REQUIRES M5Unified esp_wifi esp_netif nvs_flash
    PRIV_REQUIRES espressif__esp-dsp esp_driver_ppa app_update
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MG_ARCH=MG_ARCH_ESP32
    MG_OTA=MG_OTA_NONE
    MG_ENABLE_PACKED_FS=0
)
