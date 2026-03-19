/* usb_stubs.c — Stub callbacks for unused TinyUSB class drivers.
 *
 * libarduino_tinyusb.a (pioarduino Core 3.x) unconditionally compiles in
 * HID, DFU-RT, DFU and NCM class drivers.  Their application-side callbacks
 * must be provided even though this project only uses MSC.
 *
 * All stubs are __attribute__((weak)) so they can be overridden if those
 * classes are ever added.
 */
#include <stdint.h>
#include <stdbool.h>

/* ── HID ─────────────────────────────────────────────────────────────────── */
__attribute__((weak))
uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance)
{ (void)instance; return 0; }

__attribute__((weak))
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                int report_type,
                                uint8_t* buffer, uint16_t reqlen)
{ (void)instance; (void)report_id; (void)report_type;
  (void)buffer; (void)reqlen; return 0; }

__attribute__((weak))
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            int report_type,
                            uint8_t const* buffer, uint16_t bufsize)
{ (void)instance; (void)report_id; (void)report_type;
  (void)buffer; (void)bufsize; }

/* ── DFU Runtime ─────────────────────────────────────────────────────────── */
__attribute__((weak))
void tud_dfu_runtime_reboot_to_dfu_cb(void) {}

/* ── DFU ─────────────────────────────────────────────────────────────────── */
__attribute__((weak))
uint32_t tud_dfu_get_timeout_cb(uint8_t alt, uint8_t state)
{ (void)alt; (void)state; return 0; }

__attribute__((weak))
void tud_dfu_download_cb(uint8_t alt, uint16_t block_num,
                         uint8_t const* data, uint16_t length)
{ (void)alt; (void)block_num; (void)data; (void)length; }

__attribute__((weak))
void tud_dfu_manifest_cb(uint8_t alt)
{ (void)alt; }

/* ── NCM (USB-Ethernet) ──────────────────────────────────────────────────── */
__attribute__((weak))
bool tud_network_recv_cb(uint8_t const* buf, uint16_t size)
{ (void)buf; (void)size; return false; }
