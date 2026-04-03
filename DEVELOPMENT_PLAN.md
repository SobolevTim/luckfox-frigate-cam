# DEVELOPMENT_PLAN

## Цель
Создать стабильное и гибкое решение для Luckfox Camera RTSP с удобным управлением через MQTT, поддержкой Frigate/HA и эволюционирующей функцией детекции на камере.

## 1. Тестирование микрофонов
- проверить совместимость с USB, I2C, SPI и аналоговыми микрофонами (Pico Ultra + другие платы).
- измерить качество звука, задержку, SNR, артефакты и устойчивость в реальном случае.
- выбрать оптимальный аудио-стриминг и параметры AEC/AGC/HPF для финальной сборки.
- прописать результат в документации и в техпаспорте `audio_mpi`.

## 2. Корпус на 3D-принтере
- разработать дизайн с учётом охлаждения, антенн, разъёмов и управления кабелями.
- обеспечить герметизацию/защиту от пыли, и при необходимости конденсата.
- сохранить STL/описание в `deploy/case/`, добавить сборочные инструкции.

## 3. RTSP авторизация
- внедрить username/password для потоков RTSP (main/sub).
- поддержать digest/basic, совместимые с FFmpeg/Frigate.
- добавить MQTT-опции `rtsp_user`, `rtsp_pass`, `rtsp_auth_mode`.
- в документации описать настройку и случаи fallback.

## 4. Корректировка MQTT-данных
- провести ревизию `state` и `telemetry`, убрать устаревшие и лишние поля.
- добавить реально важные поля:
  - `stream_bandwidth_actual`
  - `substream_fps_effective`
  - `cpu_temp`, `soc_temp`, `voltage`, `power_w`
  - `video_dropped_frames`, `packet_loss` (если возможно)
  - `audio_status`, `mic_level`, `audio_error`
- сделать режимы `telemetry_mode=light/detailed`.
- зафиксировать контракт топиков в `MQTT_CONTRACT.md`.

## 5. NPU + детекция на камере
- базовый MVP: локальная детекция (person/motion) в `substream` через RKNN.
- собрать pipeline: capture -> postprocess -> inference -> MQTT alert.
- обеспечить низкий TDP: режимы `detection_interval`, `skip_frames`, `low_power`.
- добавить флаг `edge_detection_enabled` и настройки модели.
- продолжить интеграцию Frigate через MQTT-события и RTSP detect stream.

## 6. Дополнительные идеи
- OTA обновления + резервный раздел для безопасного отката.
- локальный мини web UI (HTTPS) для контроля параметров, debug и live preview.
- режим энергосбережения: `sleep`/`wake-on-motion`, fps downscale и bitrate downscale.
- `camera_rtsp.json` v2: версия, checksum, миграция настроек при апгрейде.
- CI проверка: unit + integration + lint + статический анализ.

## Этапы и приоритеты
1. безопасность: RTSP auth, MQTT контракт, стабильность хранения настроек.
2. надёжность: мониторинг/телеметрия, graceful shutdown, FSM ошибок.
3. UX: корпус и удобная документация.
4. продвинутая фича: on-device NPU детекция.
