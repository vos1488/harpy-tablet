# NavRelay — Jailbreak Tweak

Relay turn-by-turn navigation from iPhone to HARPY Remote (ESP32-S3) via custom BLE GATT service.

## Как работает

1. Tweak загружается в процессы навигационных приложений (Apple Maps, Google Maps, Yandex)
2. Хукает view controller'ы навигации и перехватывает инструкции маневров
3. Подключается к ESP32 через CoreBluetooth (устройство уже подключено как HID)
4. Записывает навигационные данные в кастомную BLE-характеристику
5. ESP32 парсит данные и отображает на LCD

## Сборка

### Требования
- Mac или Linux с установленным [Theos](https://theos.dev/docs/installation)
- Jailbroken iPhone (iOS 14-17)
- SSH доступ к iPhone

### Установка Theos
```bash
bash -c "$(curl -fsSL https://raw.githubusercontent.com/theos/theos/master/bin/install-theos)"
```

### Сборка и установка
```bash
cd ios_tweak/NavRelay
export THEOS=~/theos

# Для rootful jailbreak (unc0ver, checkra1n, palera1n 1.x):
make package install THEOS_DEVICE_IP=192.168.x.x

# Для rootless jailbreak (Dopamine, palera1n 2.x):
make package install THEOS_DEVICE_IP=192.168.x.x THEOS_PACKAGE_SCHEME=rootless
```

## Адаптация под вашу iOS версию

Tweak содержит хуки для наиболее распространённых классов навигации в Apple Maps.
Если хуки не работают на вашей версии iOS, нужно найти правильные классы:

### 1. Найти заголовки фреймворков
```bash
# SSH на iPhone
ssh root@<ip>

# Дамп классов Apple Maps
class-dump /Applications/Maps.app/Maps > /tmp/maps_classes.h

# Дамп Navigation framework
class-dump /System/Library/PrivateFrameworks/Navigation.framework/Navigation > /tmp/nav_classes.h

# Дамп MapsSupport
class-dump /System/Library/PrivateFrameworks/MapsSupport.framework/MapsSupport > /tmp/maps_support.h
```

### 2. Посмотреть логи
Tweak автоматически логирует все классы, содержащие "Navigation", "Maneuver", "Step" в своём имени:
```bash
# На iPhone
idevicesyslog | grep NavRelay
# или
ssh root@<ip> "cat /var/log/syslog | grep NavRelay"
```

### 3. Обновить хуки в Tweak.x
Замените `MNStepCardViewController` на найденное имя класса.

## Протокол

Tweak записывает UTF-8 строку в BLE-характеристику:
```
DIR|DIST|INSTRUCTION|STREET|ETA|SPEED|APP
```

Где `DIR`:
| Код | Направление |
|-----|-------------|
| 0 | нет |
| 1 | прямо |
| 2 | налево |
| 3 | направо |
| 4 | плавно налево |
| 5 | плавно направо |
| 6 | разворот |
| 7 | прибыли |
| 8 | круговое движение |

Специальное значение: `NAV_END` — навигация завершена.

## BLE UUIDs

| Назначение | UUID |
|------------|------|
| Service | `E6A30000-B5A3-F393-E0A9-E50E24DCCA9E` |
| Nav Data | `E6A30001-B5A3-F393-E0A9-E50E24DCCA9E` |

## Отладка

Если tweak не отправляет данные:
1. Проверьте, что ESP32 подключен как "HARPY Remote" в Bluetooth
2. Проверьте логи: `idevicesyslog | grep NavRelay`
3. Убедитесь, что iOS не кэширует старую GATT таблицу — забудьте устройство и переподключитесь
