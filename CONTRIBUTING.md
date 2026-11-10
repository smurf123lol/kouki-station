# Как контрибьютить в Kouki Station

Прошивка станции: ESP-IDF 5.5.1, плата Waveshare ESP32-S3-Touch-LCD-1.85C (ревизия 2).
Правила кода и грабли — в [AGENTS.md](AGENTS.md); здесь — рабочий цикл.

## Что нужно на машине

- Сервер разработки (`/srv/kouki-workspace`): ESP-IDF v5.5.1 в `_scratch/esp-idf`,
  тулчейн в `_scratch/esp-tools`, `git`, плата по USB (видна как `/dev/ttyACM0`).
- Либо Windows-машина с ESP-IDF v5.5.1, `python` 3.12, `git` и платой на `COM3`
  (вспомогательный путь, см. `docs/build.md`, «Вариант 2»).
- Свободные ~3 ГБ на диске: тулчейн + собранный образ.

## Цикл разработки (канон — сервер)

```bash
# из каталога kouki-station
tools/build-linux.sh --secret --build-dir _scratch/build-master   # сборка
BUILD_DIR=_scratch/build-master tools/flash-linux.sh -p /dev/ttyACM0   # прошивка
```

На Windows те же шаги: `. .\tools\env-idf.ps1` (один раз на shell), затем
`.\tools\build-native.ps1 -Secret` и `.\tools\flash-windows.ps1 -Port COM3`.

Смотреть лог платы: `tools/flash-linux.sh --monitor` (или `idf.py monitor`), на сервере ещё
`_scratch/read-serial.py` — прошивка сама печатает состояние: версию прошивки, IP, режим (кнопка/кодовое слово), итог каждого
запроса (`отправка: err=…, HTTP …, услышали: …`).

## Проверка без микрофона

1. **Тестовый режим** (`KOUKI_TEST_MODE` в menuconfig): через N секунд после старта станция
   сама пишет и отправляет. Удобно ловить «сервер не отвечает», «порог слишком высокий».
2. **Сервер с распознаванием** должен быть собран с `-p:KoukiEnableSherpa=true`, иначе
   `/api/voice` отвечает `503`, и станция покажет «Не вышло» — это не поломка станции.
3. **Синтез речи** на серверной машине: `Kouki/tools/Kouki.SpeechLab` (`say` / `hear` / `roundtrip`, сборка с `-p:KoukiEnableSherpa=true`)
   — проверяет распознавание и весь HTTP-путь без человека.

## Что считается готовым

- [ ] `idf.py build` без ошибок, образ прошит, лог загрузки чистый (Wi-Fi, LCD, микрофон).
- [ ] Кнопка «Записать» и «Спросить» работают; запись доходит до сервера (видно в `actions`).
- [ ] Фраза не обрезана: в задаче лежит именно то, что сказали.
- [ ] Настройки описаны в `README.md`, а решения — в `docs/decisions.md`.
- [ ] `sdkconfig.secret`, `build/`, `station-db/`, `station-audio/` не в коммите.

## Правки драйверов

`main/drivers/**` и `components/audio_board/**` — чужой код. Если без правки не обойтись:
минимальный диф, комментарий `// kouki: зачем`, и запись в `docs/build.md` о том, что именно
сломалось (иначе следующая сверка с Waveshare это потеряет).

## Коммиты

`тип(область): описание` — `fix(mic)`, `feat(ui)`, `docs(build)`, `ci(idf)`.
В одном коммите — одно изменение; образы и логи не прикладываем.
