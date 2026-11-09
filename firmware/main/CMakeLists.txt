idf_component_register(
    SRCS
        "./main.c"
        "./app_config.c"
        "./app_setup.c"
        "./app_wifi.c"
        "./app_http.c"
        "./app_voice.cpp"
        "./fsm/voice_state_manager.cpp"
        "./fsm/voice_response_parser.cpp"
        "./fsm/states/idle_state.cpp"
        "./fsm/states/recording_state.cpp"
        "./fsm/states/sending_state.cpp"
        "./fsm/states/done_state.cpp"
        "./fsm/states/followup_state.cpp"
        "./fsm/states/error_state.cpp"
        "./app_ui.cpp"
        "./ui/ui_state_manager.cpp"
        "./ui/widgets/ui_orb_widget.cpp"
        "./ui/widgets/ui_card_widget.cpp"
        "./ui/states/ui_voice_substates.cpp"
        "./ui/states/ui_history_tile.cpp"
        "./ui/states/ui_settings_tile.cpp"
        "./ui/states/ui_modal_screens.cpp"
        "./app_wake.c"
        "./app_tone.c"
        "./app_audio_out.c"
        "./app_speech.c"
        "./app_ota.c"
        "./app_time.c"
        "./app_power.c"
        "./app_history.c"
        # Драйверы Waveshare (Apache-2.0), см. main/drivers/README.md
        "./drivers/exio/TCA9554PWR.c"
        "./drivers/lcd/esp_lcd_st77916/esp_lcd_st77916.c"
        "./drivers/lcd/ST77916.c"
        "./drivers/touch/esp_lcd_touch/esp_lcd_touch.c"
        "./drivers/touch/CST816.c"
        "./drivers/lvgl/LVGL_Driver.c"
        # Шрифты с кириллицей, сгенерированы lv_font_conv из Arial
        "./ui/font_ru_14.c"
        "./ui/font_ru_16.c"
        "./ui/font_ru_20.c"
    INCLUDE_DIRS
        "."
        "./fsm"
        "./drivers/exio"
        "./drivers/lcd"
        "./drivers/lcd/esp_lcd_st77916"
        "./drivers/touch"
        "./drivers/touch/esp_lcd_touch"
        "./drivers/lvgl"
        "./ui"
        "./ui/widgets"
        "./ui/states"
    REQUIRES
        app_update
        esp_https_ota
        driver
        esp_lcd
        esp_http_client
        esp_http_server
        esp_wifi
        esp_event
        esp_netif
        lwip
        esp-tls
        esp_driver_usb_serial_jtag
        nvs_flash
        json
        # esp_crt_bundle.h живёт в mbedtls: нужен для https без своего CA.
        mbedtls
        audio_board
        espressif__esp_codec_dev
        # Wake-word: esp-sr, esp-dl FFT и microWakeWord
        espressif__esp-sr
        espressif__dl_fft
        espressif__esp-tflite-micro
    EMBED_TXTFILES
        "certs/kouki-local-root.pem"
)

# esp-dl собирает только те ядра, что перечислены в compile_req-файлах компонентов.
# esp-sr перечисляет не все ядра, нужные модели wn10, поэтому добавляем своё
# дополнение (см. kouki-dl-req.yml и комментарий в нём).
idf_build_get_property(_build_components BUILD_COMPONENTS)
if("espressif__esp-dl" IN_LIST _build_components)
    idf_component_get_property(_espdl_dir espressif__esp-dl COMPONENT_DIR)
    include(${_espdl_dir}/cmake/compile_register.cmake)
    esp_dl_compile_add_yml(${CMAKE_CURRENT_LIST_DIR}/kouki-dl-req.yml)
endif()
