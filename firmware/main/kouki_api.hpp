#ifndef KOUKI_API_HPP
#define KOUKI_API_HPP

#include "esp_err.h"

// C-подписи для main.c лежат в C-заголовке: он остаётся C-файлом.
#include "kouki_api.h"

namespace kouki {

class AudioOut;
class Config;
class History;
class Http;
class Ota;
class Power;
class Setup;
class Speech;
class Time;
class Tone;
class Vad;
class Voice;
class Wake;
class WakeVerify;
class Wifi;

/**
 * Сборка объектного графа станции (T121 П13).
 *
 * Все модули после П2–П12 — классы в namespace kouki с фабриками вида
 * `Config &config()`. Здесь они собраны в одном месте: таблица зависимостей
 * (какой класс в какой конструктор что получает) и порядок их создания
 * видны целиком, а не размазаны по модулям.
 *
 * Экземпляры статические и живут всё время работы прошивки: динамического
 * создания/удаления в рантайме нет (спека §2). `station()` только СОЗДАЁТ
 * объекты (конструкторы без побочных эффектов), `station_init()` выполняет
 * прежний порядок инициализации из `app_main`.
 */
struct Station {
    Config &config;
    Time &time;
    History &history;
    Power &power;
    Http &http;
    AudioOut &audio;
    Tone &tone;
    Vad &vad;
    Wifi &wifi;
    Ota &ota;
    Wake &wake;
    WakeVerify &wake_verify;
    Speech &speech;
    Setup &setup;
    Voice &voice;
};

/** Объект, к которому привязаны модули станции (создаёт их при первом вызове). */
Station &station();

/**
 * Создаёт все объекты станции в порядке зависимостей (лист → зависимый).
 *
 * Побочных эффектов нет: модули ничего не инициализируют в конструкторах,
 * всё делается в station_init(). Нужен, чтобы объектный граф был собран
 * одним вызовом до первого обращения к модулю.
 */
esp_err_t station_wire();

/**
 * Порядок инициализации станции — прежний `app_main` (спека §1, решение §5
 * «порядок esp_board_init трогать нельзя»):
 *
 *   esp_board_init(16000, 2, 16) -> audio_out -> player -> EXIO -> LCD -> LVGL
 *   -> voice -> history -> power -> ui -> консоль -> ota -> Wi-Fi/SoftAP
 *
 * Аудио раньше экрана: расширитель GPIO и тач живут на I2C, который поднимает
 * аудио-борда. Возвращает первый esp_err_t отказа (остальные шаги как в master:
 * ESP_ERROR_CHECK внутри — процесс не продолжается).
 */
esp_err_t station_init();

}  // namespace kouki

#endif  // KOUKI_API_HPP
