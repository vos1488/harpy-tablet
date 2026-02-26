# HARPY Tablet Firmware

Прошивка мини-планшета для **Waveshare ESP32-S3-Touch-LCD-4.3**

## Возможности

- **Бутлого "HARPY"** — анимированный экран загрузки с эффектами
- **WiFi менеджер** — сканирование сетей, подключение, сохранение пароля в NVS
- **Bluetooth (BLE)** — сканер BLE-устройств через NimBLE
- **MJPEG Stream Viewer** — просмотр видеопотока по IP:порт (MJPEG over HTTP)
- **Тач-интерфейс** — полная поддержка сенсорного экрана GT911
- **Экранная клавиатура** — ввод IP, порта, пароля WiFi

## Аппаратные требования

| Параметр | Значение |
|----------|----------|
| Плата | Waveshare ESP32-S3-Touch-LCD-4.3 |
| Дисплей | 4.3" 800x480 RGB LCD |
| Тачскрин | GT911 (I2C) |
| Flash | 16MB |
| PSRAM | 8MB OPI |
| WiFi | 802.11 b/g/n |
| Bluetooth | BLE 5.0 (NimBLE) |

## Сборка

### Требования
- ESP-IDF >= 5.1.0
- CMake >= 3.16

### Команды

```bash
# Установить переменные окружения ESP-IDF
. $IDF_PATH/export.sh

# Собрать проект
idf.py build

# Прошить (замените /dev/ttyUSB0 на ваш порт)
idf.py -p /dev/ttyUSB0 flash monitor

# Под Windows
idf.py -p COM3 flash monitor
```

## Структура проекта

```
├── CMakeLists.txt              # Корневой CMake
├── sdkconfig.defaults          # Конфигурация ESP-IDF
├── partitions.csv              # Таблица разделов
├── main/
│   ├── CMakeLists.txt          # CMake компонента main
│   ├── idf_component.yml       # Зависимости (LVGL, GT911)
│   ├── main.c                  # Точка входа
│   ├── boot_logo.c             # Бутлого HARPY
│   ├── lcd_driver.c            # Драйвер RGB LCD
│   ├── touch_driver.c          # Драйвер GT911
│   ├── wifi_manager.c          # WiFi менеджер
│   ├── bt_manager.c            # Bluetooth (BLE) менеджер
│   ├── stream_viewer.c         # MJPEG стрим просмотрщик
│   ├── ui_home.c               # Домашний экран (лаунчер)
│   ├── ui_keyboard.c           # Экранная клавиатура
│   └── include/
│       ├── harpy_config.h      # Конфигурация пинов и параметров
│       ├── boot_logo.h
│       ├── lcd_driver.h
│       ├── touch_driver.h
│       ├── wifi_manager.h
│       ├── bt_manager.h
│       ├── stream_viewer.h
│       ├── ui_home.h
│       └── ui_keyboard.h
```

## Использование Stream Viewer

1. Подключитесь к WiFi через приложение **WiFi** на домашнем экране
2. Откройте приложение **Stream**
3. Нажмите на поля **IP**, **Port**, **Path** и введите данные:
   - IP: адрес сервера (например `192.168.1.100`)
   - Port: порт потока (например `8080`)
   - Path: путь (например `/mjpeg` или `/`)
4. Нажмите **Start** для начала просмотра

### Поддерживаемые источники
- IP камеры с MJPEG выходом
- OBS Studio (с плагином MJPEG)
- ESP32-CAM
- Любой HTTP MJPEG поток

## Настройка пинов

Если ваша версия платы имеет другую распиновку, отредактируйте файл
`main/include/harpy_config.h` — все пины LCD, подсветки и тача определены там.

## Лицензия

MIT
