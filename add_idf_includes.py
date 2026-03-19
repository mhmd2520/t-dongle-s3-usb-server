"""
add_idf_includes.py — pioarduino clean-build fix.

In hybrid/source-build mode (custom_sdkconfig), pioarduino adds the
framework-arduinoespressif32-libs include paths via the IDF cmake output.
On a first/clean build that cmake cache doesn't exist yet, so headers used
by the Arduino core layer (TinyUSB, mDNS) are not found.

Fix: add only the specific top-level include directories that Arduino core
and libraries need. Do NOT add subdirectories — TinyUSB uses relative
includes internally, and adding subdirs (osal/, host/) shadows IDF component
headers (esp_http_server osal.h, usb/hub.h) causing type errors.
"""
Import("env")
import os

# Resolve PlatformIO home — try env var, then sysenv, then fallback
piohome = (
    env.subst("$PIOHOME_DIR") or
    os.environ.get("PLATFORMIO_HOME_DIR") or
    os.path.join(os.path.expanduser("~"), ".platformio")
)
piohome = os.path.realpath(piohome)

board_mcu = env.BoardConfig().get("build.mcu", "esp32s3").lower()
for suffix in ("_dev", "_r8", "_n16r8", "_n8r8"):
    board_mcu = board_mcu.replace(suffix, "")

chip_dir = os.path.join(
    piohome, "packages",
    "framework-arduinoespressif32-libs",
    board_mcu, "include",
)

print(f"[add_idf_includes] piohome={piohome}  mcu={board_mcu}")
print(f"[add_idf_includes] chip_dir={chip_dir}  exists={os.path.isdir(chip_dir)}")

if not os.path.isdir(chip_dir):
    print("[add_idf_includes] WARNING: chip_dir not found, skipping")
else:
    # Exact subdirectories to add (relative to chip_dir).
    # Only the roots that contain headers directly included by Arduino core/libs.
    # Do NOT add subdirs of arduino_tinyusb — tusb.h uses relative includes
    # internally, and adding subdirs shadows IDF component headers.
    EXACT_DIRS = [
        "arduino_tinyusb/tinyusb/src",   # tusb.h — esp32-hal-tinyusb.h
        "arduino_tinyusb/include",        # Arduino TinyUSB wrapper headers
        "espressif__mdns/include",        # mdns.h — ESPmDNS library
    ]
    added = 0
    for rel in EXACT_DIRS:
        d = os.path.join(chip_dir, rel)
        if os.path.isdir(d):
            env.Append(CPPPATH=[d])
            added += 1
        else:
            print(f"[add_idf_includes] WARNING: {rel} not found")
    print(f"[add_idf_includes] Added {added} targeted IDF include dirs")
