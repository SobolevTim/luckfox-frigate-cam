# Luckfox Camera RTSP — стабильная камера для Frigate + Home Assistant

## Описание

Проект создаёт стабильный RTSP-стрим H.264 с камеры **MIS5001** или **SC3336** на плате  
**Luckfox Pico Ultra W** (RV1106). Никаких оверлеев и CPU-конвертации цвета —  
вся обработка выполняется аппаратно (ISP → VI → VENC).

Параметры изображения и стрима управляются через **MQTT** с автоматическим
HA Discovery — все сущности появляются в Home Assistant автоматически без ручной
конфигурации.

> **🧪 Аудио (экспериментально):** встроенный микрофон доступен только на **Luckfox Pico Ultra**.
> Текущее качество звука неудовлетворительное — ведётся поиск решений.
> Сборка с `--audio on` работает, но версия аудио не финальная.

---

## Возможности

| Функция | Статус |
|---|---|
| RTSP H.264 (профиль сенсора: 2K или 1080p) | ✅ |
| RTSP Sub stream (detect для Frigate, RGA downscale) | ✅ |
| RTSP Audio (G.711A / PCMA) | 🧪 экспериментально (только Pico Ultra, встроенный микрофон) |
| Аппаратный ISP (rkaiq) | ✅ |
| Нет CPU конвертации YUV→RGB | ✅ |
| Яркость / Контраст / Насыщенность / Оттенок | ✅ |
| Резкость | ✅ |
| Режим день / ночь (оттенки серого) | ✅ |
| Баланс белого (авто + 7 пресетов) | ✅ |
| Зеркало / Поворот | ✅ |
| Антимигание 50 Гц / auto | ✅ |
| Изменение битрейта на лету | ✅ |
| Изменение FPS на лету | ✅ |
| MQTT + HA Auto-Discovery | ✅ (все сущности автоматически) |
| Валидация и отклонение невалидных значений | ✅ |
| Сохранение настроек на диск | ✅ (`/etc/camera_rtsp.json`) |
| Корректное завершение по SIGINT/SIGTERM | ✅ |
| Интеграция с Home Assistant | ✅ (см. ниже) |

---

## Требования

### Оборудование
- Основная тестовая платформа: **Luckfox Pico Max (RV1106)**
- Дополнительная платформа: **Luckfox Pico Ultra W (RV1106)** с встроенным модулем Wi-Fi
- Поддерживаемые сенсоры: camera MIS5001 или SC3336
- Для централизованной работы: Ethernet LAN (Pico Ultra W также поддерживает Wi-Fi по 802.11ac)
- Платы используют стандартную Buildroot + BusyBox систему от luckfox.

### Платформа и прошивка
- Камеры работают на стандартной системе `Buildroot + BusyBox` от luckfox. Это базовая ОС для плат серии Pico.
- Инструкции по установке системного образа и ссылки для скачивания официальных прошивок есть в вики:
  https://wiki.luckfox.com/Luckfox-Pico-Ultra/Flash-image (ссылка для Pico Ultra W, аналогично для Pico Max)
- Для `Luckfox Pico Ultra W` доступен встроенный Wi-Fi. Параметры модуля и примеры конфигурации см. здесь:
  https://wiki.luckfox.com/Luckfox-Pico-Ultra/WiFi-BT#1-wifi (настройка через `/etc/wpa_supplicant.conf `)
- После прошивки плату нужно настроить в сети: для стабильной работы с Frigate и Home Assistant рекомендуется задавать статический IP.
  https://wiki.luckfox.com/Luckfox-Pico-Ultra/Autostart#4-configuring-a-static-ip (ссылка для Pico Ultra W, аналогично для Pico Max)


### Платы Luckfox Pico
- `Luckfox Pico Max` — RV1106G3, 256MB DDR3L, SPI NAND FLASH(256MB), MIPI CSI 2-lane, DPI-interface RGB666, Ethernet 100M, USB 2.0 Host/Device, 26 GPIO pins.
- `Luckfox Pico Ultra W` — RV1106G3, 256MB DDR3L, eMMC(8GB), MIPI CSI 2-lane, DPI-interface RGB666, 2.4GHz WiFi6, Bluetooth 5.2/BLE, USB 2.0 Host/Device, 33 GPIO pins.

### Аудио (🧪 экспериментально)
- Встроенный микрофон доступен **только на Luckfox Pico Ultra**. На остальных платах (Pro, Max и т.д.) микрофон отсутствует.
- Сборка с `--audio on` включает G.711A (PCMA) в main RTSP stream.
- **Качество звука пока неудовлетворительное** — ведётся поиск решений. Версия аудио не финальная.

### Зависимости (SDK)
Заголовки и библиотеки Rockchip MPI уже включены в каталог `sdk/` репозитория.  
Для сборки нужен только кросс-тулчейн из Luckfox SDK (`LUCKFOX_SDK_PATH`).

---

## Сборка

```bash
# 1. Установить переменную с путём к Luckfox SDK (тулчейн)
export LUCKFOX_SDK_PATH=<luckfox-pico SDK path>

# 2. Собрать (заголовки и библиотеки уже в sdk/)
chmod +x build.sh
./build.sh

# 3. Сборка под SC3336 (1080p)
./build.sh --sensor SC3336

# 4. Сборка со звуком (🧪 экспериментально, только Pico Ultra)
./build.sh --sensor MIS5001 --audio on
```

Профили сенсора:
- `MIS5001` (по умолчанию): `2592x1944 @ 25fps`
- `SC3336`: `1920x1080 @ 25fps`

При необходимости stream можно переопределить вручную через CMake:
`-DSTREAM_WIDTH=... -DSTREAM_HEIGHT=... -DSTREAM_FPS=...`

Sub stream (по умолчанию `640x360 @ 10fps, 512 kbps`):
`-DSUB_STREAM_WIDTH=... -DSUB_STREAM_HEIGHT=... -DSUB_STREAM_FPS=... -DSUB_STREAM_BITRATE_KBPS=...`

Опции аудио (через CMake):
- `-DENABLE_AUDIO=ON|OFF`
- `-DAUDIO_SAMPLE_RATE=8000|16000`
- `-DAUDIO_CHANNELS=1|2`

Сборку приложения можно выполнить самостоятельно по инструкции выше. Альтернативно, выберите готовую сборку для нужного сенсора в релизах этого репозитория.

Бинарник появится в `install/uclibc/bin/luckfox_camera_rtsp`.

---

## Деплой на устройство

```bash
# Скопировать всё на камеру
scp -r install/uclibc/ root@<CAMERA_IP>:/opt/camera_rtsp/

# Запустить на камере с указанием MQTT-брокера
ssh root@<CAMERA_IP>
/opt/camera_rtsp/bin/luckfox_camera_rtsp \
  --mqtt-host <BROKER_IP> \
  --mqtt-port 1883 \
  --mqtt-user <user> \
  --mqtt-pass <password>
```

### Параметры запуска

| Параметр | По умолчанию | Описание |
|---|---|---|
| `--mqtt-host` | `127.0.0.1` | IP или hostname MQTT брокера |
| `--mqtt-port` | `1883` | TCP порт брокера (`1..65535`) |
| `--mqtt-user` | — | Имя пользователя (необязательно) |
| `--mqtt-pass` | — | Пароль (необязательно) |
| `--mqtt-id` | `luckfox_camera` | Client ID и префикс топиков |
| `--mqtt-name` | `Luckfox Camera` | Отображаемое имя устройства в HA |
| `--mqtt-discovery-refresh` | `0` | Периодическое обновление discovery (сек), `0..86400`, `0` = выключено |

### Автозапуск (init.d)

Примеры скриптов автозапуска находятся в `deploy/`:

```bash
# 1. Скопировать примеры и заполнить свои параметры
cp deploy/run_camera.sh.example deploy/run_camera.sh
cp deploy/S99camera.example deploy/S99camera
# Отредактировать deploy/run_camera.sh — указать MQTT_HOST, MQTT_USER, MQTT_PASS и т.д.

# 2. Загрузить на камеру
scp deploy/run_camera.sh root@<CAMERA_IP>:/opt/camera_rtsp/run_camera.sh
scp deploy/S99camera root@<CAMERA_IP>:/etc/init.d/S99camera
ssh root@<CAMERA_IP> chmod +x /opt/camera_rtsp/run_camera.sh /etc/init.d/S99camera
```

В `run_camera.sh` укажите параметры MQTT:
```sh
MQTT_HOST="<BROKER_IP>"
MQTT_USER="<user>"
MQTT_PASS="<password>"
MQTT_ID="luckfox_camera"
MQTT_NAME="Luckfox Camera"
```

---

## Подключение к Frigate

Рекомендуемые стартовые параметры стрима:

- Main stream: H.264, `2592×1944`, `25 fps`, `10240 kbps`
- Sub stream: H.264, `640×360`, `10 fps`, `512 kbps` (для Frigate detect)
- Эффективный `sub_fps` не может превышать текущий `main fps` (ограничение общего кадропотока)
- GOP автоматически равен текущему `fps` (то есть примерно 1 секунда между I-frame)
- при сборке с `--audio on` (🧪): RTSP аудио `G.711A (PCMA)` только в main stream

```yaml
# frigate/config.yaml
cameras:
  luckfox:
    ffmpeg:
      inputs:
        - path: rtsp://<CAMERA_IP>:554/live/0
          roles:
            - record
            # - audio   # включите, если нужен аудио-детект во Frigate
        - path: rtsp://<CAMERA_IP>:554/live/1
          roles:
            - detect
    detect:
      width: 640
      height: 360
      fps: 10
```

---

## MQTT управление

Камера публикует **MQTT Discovery** при каждом подключении к брокеру.  
Home Assistant автоматически создаёт устройство **"Luckfox Camera"** со всеми сущностями — ручной настройки не нужно.

### Топики

| Топик | Назначение |
|---|---|
| `luckfox_camera/availability` | `online` / `offline` (LWT) |
| `luckfox_camera/state` | retained JSON только с настройками камеры |
| `luckfox_camera/telemetry` | non-retained JSON с диагностикой и runtime-метриками |
| `luckfox_camera/ack` | non-retained JSON-ответ на каждую команду (`ok/error`) |
| `luckfox_camera/<парам>/set` | команды управления |

Подробный контракт топиков и примеры payload:
- [`MQTT_CONTRACT.md`](MQTT_CONTRACT.md)

В `telemetry` дополнительно публикуются сервисные поля по хранилищу:
- `root_fs_usage_pct`, `root_fs_avail_mb`
- `userdata_fs_usage_pct`, `userdata_fs_avail_mb`
- `oem_fs_usage_pct`, `oem_fs_avail_mb`

`runtime_s` считается по монотонным часам, поэтому не уходит в минус при коррекции системного времени.

`availability` теперь публикуется корректно и при штатной остановке процесса, и через MQTT LWT при обрыве питания/сети.

Telemetry-сенсоры в HA публикуются с `expire_after`, поэтому устаревшие `FPS`, `runtime`, `uptime` и другие runtime-метрики автоматически становятся `unavailable`, если плата перестала слать свежую телеметрию.

При сборке с `--audio on` (🧪 экспериментально) в `state` добавляются поля аудио:
- `audio_runtime_enabled` (`ON`/`OFF`) — микрофон реально запущен или нет.
- `audio_adc_alc_left_gain` (0–31), `audio_adc_mic_left_gain` (0–3).
- `audio_hpf` (`ON`/`OFF`), `audio_adc_micbias_voltage`, `audio_adc_mode`.

Важно: команды `audio_*` применяются только при `audio_runtime_enabled=ON`.

### Ручное управление (для отладки)

```bash
# Считать текущее состояние
mosquitto_sub -h <BROKER_IP> -t 'luckfox_camera/state'

# Установить яркость
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/brightness/set -m 100

# Принудительный night_mode (grayscale + сниженный FPS + повышенный битрейт)
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/night_mode/set -m ON

# Базовый day/night (только цвет/серый)
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/daynight/set -m grayscale

# Баланс белого
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/wb_preset/set -m daylight

# Включить зеркало
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/mirror/set -m ON

# Снизить битрейт
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/bitrate_kbps/set -m 4096

# Понизить FPS
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/fps/set -m 15

# Audio (🧪 экспериментально, только Pico Ultra, встроенный микрофон)
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/audio_adc_alc_left_gain/set -m 23
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/audio_adc_mic_left_gain/set -m 3
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/audio_adc_micbias_voltage/set -m VREFx0_975
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/audio_adc_mode/set -m SingadcL
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/audio_hpf/set -m ON
```

> Все числовые значения валидируются; при выходе за диапазон команда отклоняется.

> Невалидные payload не применяются: камера публикует `ack` со статусом `error` и оставляет прежнее состояние.

### Таблица параметров

| Парам | Значения | Описание |
|---|---|---|
| `brightness` | `0`–`255` | Яркость |
| `contrast` | `0`–`255` | Контраст |
| `saturation` | `0`–`255` | Насыщенность |
| `hue` | `0`–`255` | Оттенок |
| `sharpness` | `0`–`100` | Резкость |
| `daynight` | `color` / `grayscale` | Режим день/ночь |
| `wb_preset` | `auto` `incandescent` `fluorescent` `warm_fluorescent` `daylight` `cloudy` `twilight` `shade` | Баланс белого |
| `mirror` | `ON` / `OFF` | Зеркало |
| `flip` | `ON` / `OFF` | Переворот |
| `anti_flicker_en` | `ON` / `OFF` | Антимигание |
| `anti_flicker_mode` | `50hz` / `auto` | Режим антифликера |
| `night_mode` | `ON` / `OFF` | Ночной профиль: `ON` = grayscale + `fps=15` + `bitrate=12288`; `OFF` = возврат к дневному профилю |
| `bitrate_kbps` | `1000`–`20000` | Битрейт видео |
| `fps` | `10`–`30` | Частота кадров; GOP автоматически равен `fps` |
| `sub_fps` | `5`–`30` | Частота кадров sub stream; эффективное значение ограничено текущим `fps` основного stream |
| `audio_adc_alc_left_gain` | `0`–`31` | PGA-усиление АЦП: 0 дБ=6, шаг 1.5 дБ, макс +37.5 дБ=31; рекомендуемое значение **23** |
| `audio_adc_mic_left_gain` | `0`–`3` | Бустерное усиление MIC: 0=выкл, 1=0 дБ, 2=20 дБ, 3=макс; рекомендуемое значение **3** |
| `audio_hpf` | `ON` / `OFF` | Фильтр верхних частот (HPF): убирает DC-смещение и инфразвук; **ON** по умолчанию |
| `audio_adc_micbias_voltage` | `VREFx0_8` … `VREFx0_975` | Напряжение смещения микрофона; **VREFx0_975** — максимальное, рекомендуемое |
| `audio_adc_mode` | `SingadcL` / `DiffadcL` / `SingadcR` / … | Режим АЦП; **SingadcL** для встроенного моно-микрофона |

> 🧪 Аудио-параметры экспериментальны. Доступны только при сборке `--audio on`, только на Pico Ultra (встроенный микрофон) и при `audio_runtime_enabled=ON`.

### Значения по умолчанию

| Параметр | Значение |
|---|---|
| Разрешение | `MIS5001: 2592×1944`, `SC3336: 1920×1080` |
| Кодек | `H.264` |
| FPS | `25` |
| GOP | `25` (равен `fps`) |
| Битрейт | `10240 kbps` |

---

## Интеграция с Home Assistant

1. Зайдите в HA: **Настройки → Интеграции → MQTT** и настройте брокер.
2. Запустите камеру с `--mqtt-host <IP брокера>`.
3. Через несколько секунд в HA появится устройство **"Luckfox Camera"** со всеми сущностями — ручная настройка не требуется.

Примеры автоматизаций: [`ha_integration/configuration.yaml`](ha_integration/configuration.yaml)

---

## Структура проекта

```
├── build.sh                    — скрипт сборки
├── CMakeLists.txt              — конфигурация cmake
├── README.md
├── MQTT_CONTRACT.md            — контракт MQTT-топиков
├── DEVELOPMENT_PLAN.md         — roadmap проекта
├── deploy/
│   ├── S99camera.example       — init.d скрипт (пример)
│   └── run_camera.sh.example   — watchdog-скрипт запуска (пример)
├── ha_integration/
│   └── configuration.yaml      — примеры автоматизаций для HA
├── sdk/
│   ├── include/                — заголовки Rockchip MPI/ISP/RGA/RTSP
│   └── lib/                    — прекомпилированные библиотеки (uclibc/glibc)
├── include/
│   ├── audio_mpi.h             — AI/AENC аудио-пайплайн (🧪 экспериментально)
│   ├── camera_mpi.h            — VI / VENC API
│   ├── isp_control.h           — управление ISP параметрами
│   └── mqtt_client.h           — MQTT клиент с HA Discovery
└── src/
    ├── main.cc                 — главный файл
    ├── audio_mpi.cc            — RTSP аудио (G.711A, AI→AENC, 🧪 экспериментально)
    ├── camera_mpi.cc           — реализация VI / VENC
    ├── isp_control.cc          — rkaiq uAPI2 обёртки
    └── mqtt_client.cc          — MQTT 3.1.1 (чистый POSIX, без зависимостей)
```

---


