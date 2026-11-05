# Прошивка станции Kouki: ESP32-S3-Touch-LCD-1.85C (ревизия 2).
# Требуется ESP-IDF 5.5 или новее — этого требует контроллер дисплея ST77916
# и ESP-IDF-драйверы из примеров Waveshare.
cmake_minimum_required(VERSION 3.16)

# Версия прошивки (T16 OTA). В коде доступна через esp_app_get_description()->version
set(PROJECT_VER "0.1.5")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kouki_station)
