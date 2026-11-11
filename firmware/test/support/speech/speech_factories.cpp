/*
 * Фабрики C++-модулей для стенда t116 — только на хосте.
 *
 * Стенд t116 проверяет логику речи (поколения, abort, разбор WAV, чанки), а не
 * очередь кодека: kouki_audio_out_* и esp_audio_play подменены счётчиками в
 * stubs.c. Модуль речи при этом настоящий, поэтому его фабрика
 * kouki::speech() требует ссылки на AudioOut и Tone.
 *
 * AudioOut создаётся в статической памяти через placement new: конструктор
 * по умолчанию (AudioOut() = default) ничего не выделяет, а обычный объект со
 * статическим сроком жизни потребовал бы AudioOut::~AudioOut(), который
 * определён в app_audio_out.cpp — его к стенду не линкуют, потому что его
 * C-фасады конфликтовали бы со счётчиками stubs.c. Кучи здесь нет: буфер
 * лежит в .bss стенда.
 *
 * Tone речи не нужен, но входит в её конструктор по спеке §2, поэтому
 * линкуется настоящий app_tone.cpp.
 */
#include <new>

#include "app_audio_out.hpp"

namespace kouki {

AudioOut &audio_out()
{
    alignas(AudioOut) static unsigned char s_storage[sizeof(AudioOut)];
    static AudioOut *s_instance = new (s_storage) AudioOut();
    return *s_instance;
}

}  // namespace kouki
