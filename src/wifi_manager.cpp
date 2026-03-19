#include "wifi_manager.h"
#include "lcd.h"
#include "config.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_wifi.h>

// ── Constants ─────────────────────────────────────────────────────────────────

static const char NVS_WIFI[]   = "wifi";
static const char KEY_SSID[]   = "ssid";
static const char KEY_PASS[]   = "pass";

// ── File-scope state ──────────────────────────────────────────────────────────

static bool      g_connected = false;
static bool      g_ap_mode   = false;
static DNSServer g_dns;

// ── NVS helpers ───────────────────────────────────────────────────────────────

static void creds_save(const String& ssid, const String& pass) {
    Preferences p;
    p.begin(NVS_WIFI, false);
    p.putString(KEY_SSID, ssid);
    p.putString(KEY_PASS, pass);
    p.end();
    Serial.printf("[WiFi] Credentials saved for SSID: %s\n", ssid.c_str());
}

static bool creds_load(String& ssid, String& pass) {
    Preferences p;
    p.begin(NVS_WIFI, true);
    ssid = p.getString(KEY_SSID, "");
    pass = p.getString(KEY_PASS, "");
    p.end();
    return ssid.length() > 0;
}

static void creds_clear() {
    Preferences p;
    p.begin(NVS_WIFI, false);
    p.clear();
    p.end();
}

// ── STA connection attempt ────────────────────────────────────────────────────

static bool try_connect(const String& ssid, const String& pass, uint32_t timeout_ms) {
    // Apply static IP config if configured.
    Preferences wP;
    wP.begin("wifi", true);
    uint8_t ip_mode = wP.getUChar("ip_mode", 0);
    if (ip_mode == 1) {
        IPAddress ip, gw, mask, dns;
        String s_ip   = wP.getString("s_ip",   "");
        String s_gw   = wP.getString("s_gw",   "");
        String s_mask = wP.getString("s_mask",  "255.255.255.0");
        String s_dns  = wP.getString("s_dns",   "8.8.8.8");
        if (ip.fromString(s_ip) && gw.fromString(s_gw) &&
            mask.fromString(s_mask) && dns.fromString(s_dns)) {
            WiFi.config(ip, gw, mask, dns);
            Serial.printf("[WiFi] Static IP: %s / %s / %s\n",
                          s_ip.c_str(), s_gw.c_str(), s_mask.c_str());
        } else {
            Serial.println("[WiFi] Static IP config invalid — falling back to DHCP");
        }
    }
    wP.end();

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 >= timeout_ms) return false;
        delay(200);
    }
    return true;
}

// ── AP mode (non-blocking) ────────────────────────────────────────────────────

static void start_ap_mode() {
    // Cleanly tear down any pending STA connection before starting AP,
    // otherwise the failed STA state can prevent the AP from advertising.
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    delay(100);
    // AP+STA allows WiFi scanning while hosting the AP.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);

    // DNS: redirect all hostnames to the AP IP for OS captive-portal detection.
    g_dns.start(53, "*", IPAddress(192, 168, 4, 1));

    g_ap_mode = true;
    Serial.printf("[WiFi] Config AP started — SSID: %s  Pass: %s  IP: %s\n",
                  AP_SSID, AP_PASS, AP_IP);
}

// ── Public API ────────────────────────────────────────────────────────────────

bool wifi_begin(uint32_t timeout_ms) {
    String ssid, pass;

    if (creds_load(ssid, pass)) {
        Serial.printf("[WiFi] Connecting to saved SSID: %s\n", ssid.c_str());
        if (try_connect(ssid, pass, timeout_ms)) {
            g_connected = true;
            // Disable modem sleep — prevents DTIM-driven burst/pause pattern during downloads.
            WiFi.setSleep(false);
            esp_wifi_set_ps(WIFI_PS_NONE);
            Serial.printf("[WiFi] Connected — IP: %s  (modem sleep disabled)\n",
                          WiFi.localIP().toString().c_str());
            if (MDNS.begin(MDNS_HOST)) {
                MDNS.addService("http", "tcp", 80);
                Serial.printf("[mDNS] http://%s.local\n", MDNS_HOST);
            }
            return true;
        }
        Serial.println("[WiFi] Saved credentials failed — starting config AP");
    } else {
        Serial.println("[WiFi] No saved credentials — starting config AP");
    }

    start_ap_mode();
    return false;
}

void wifi_portal_loop() {
    if (g_ap_mode) g_dns.processNextRequest();
}

bool wifi_is_ap_mode() { return g_ap_mode; }

bool wifi_connected() {
    g_connected = (WiFi.status() == WL_CONNECTED);
    return g_connected;
}

String wifi_ip() {
    if (g_connected) return WiFi.localIP().toString();
    if (g_ap_mode)   return String(AP_IP);
    return String("0.0.0.0");
}

String wifi_ssid() {
    return (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String("");
}

void wifi_reset_credentials() {
    Serial.println("[WiFi] Resetting credentials — restarting...");
    creds_clear();
    delay(300);
    ESP.restart();
}

void wifi_disconnect() {
    MDNS.end();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    g_connected = false;
    g_ap_mode   = false;
    Serial.println("[WiFi] Disconnected");
}
