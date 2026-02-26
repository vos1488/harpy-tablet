# NavRelay — Jailbreak Tweak Makefile
# Relays turn-by-turn navigation to HARPY Remote (ESP32) via BLE
#
# Build:
#   export THEOS=~/theos
#   make package install THEOS_DEVICE_IP=<your-iphone-ip>
#
# For rootless jailbreaks (Dopamine, palera1n 2.x):
#   make package install THEOS_PACKAGE_SCHEME=rootless

TARGET := iphone:clang:latest:14.0
ARCHS = arm64 arm64e

include $(THEOS)/makefiles/common.mk

TWEAK_NAME = NavRelay
NavRelay_FILES = Tweak.x
NavRelay_FRAMEWORKS = CoreBluetooth CoreLocation UIKit Foundation
NavRelay_CFLAGS = -fobjc-arc

include $(THEOS_MAKE_PATH)/tweak.mk
