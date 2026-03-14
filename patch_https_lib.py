"""
PlatformIO pre-build script: patch fhessel/esp32_https_server for ESP32-S3.

HTTPConnection.hpp includes <hwcrypto/sha.h> which does not exist on
ESP32-S3 (espressif32@6.12.0 / Arduino Core 2.x).  This script replaces
that include with an inline mbedTLS-based esp_sha() shim every build,
so the patch survives `pio update` and fresh clones.
"""

Import("env")  # noqa: F821 — PlatformIO injects this

import os

HEADER = os.path.join(
    env["PROJECT_LIBDEPS_DIR"],          # e.g. .pio/libdeps/t-dongle-s3
    env["PIOENV"],                        # t-dongle-s3
    "esp32_https_server", "src", "HTTPConnection.hpp"
)

OLD = '#include <hwcrypto/sha.h>'

NEW = """\
// hwcrypto/sha.h does not exist on ESP32-S3 (espressif32@6.12.0 / Arduino Core 2.x).
// esp_sha_type is defined by hal/sha_types.h (pulled in via mbedtls on ESP32-S3).
// Only provide the esp_sha() function body here.
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#ifndef ESP_SHA_COMPAT_DEFINED
#define ESP_SHA_COMPAT_DEFINED
static inline void esp_sha(esp_sha_type type,
                            const unsigned char* input, size_t ilen,
                            unsigned char* output) {
    if (type == SHA1) {
        mbedtls_sha1_context ctx; mbedtls_sha1_init(&ctx);
        mbedtls_sha1_starts_ret(&ctx);
        mbedtls_sha1_update_ret(&ctx, input, ilen);
        mbedtls_sha1_finish_ret(&ctx, output);
        mbedtls_sha1_free(&ctx);
    } else {
        mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts_ret(&ctx, 0);
        mbedtls_sha256_update_ret(&ctx, input, ilen);
        mbedtls_sha256_finish_ret(&ctx, output);
        mbedtls_sha256_free(&ctx);
    }
}
#endif"""


def patch():
    if not os.path.isfile(HEADER):
        print(f"patch_https_lib: {HEADER} not found — library not yet installed, skipping")
        return

    with open(HEADER, "r", encoding="utf-8") as fh:
        text = fh.read()

    if OLD not in text:
        print("patch_https_lib: already patched, nothing to do")
        return

    patched = text.replace(OLD, NEW, 1)
    with open(HEADER, "w", encoding="utf-8") as fh:
        fh.write(patched)
    print("patch_https_lib: applied hwcrypto/sha.h shim to HTTPConnection.hpp")


patch()
