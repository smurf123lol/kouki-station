idf_component_register(
    SRCS
        "./main.c"
        "./app_wifi.c"
        "./app_voice.c"
        "./app_ui.c"
        "./app_wake.c"
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
        "./drivers/exio"
        "./drivers/lcd"
        "./drivers/lcd/esp_lcd_st77916"
        "./drivers/touch"
        "./drivers/touch/esp_lcd_touch"
        "./drivers/lvgl"
        "./ui"
    REQUIRES
        driver
        esp_lcd
        esp_http_client
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        json
        audio_board
        espressif__esp_codec_dev
        # Wake-word: esp-sr (модели — в разделе model, см. partitions.csv)
        espressif__esp-sr
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
