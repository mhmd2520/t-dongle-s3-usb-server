#include "led.h"
#include "config.h"
#include <FastLED.h>

static CRGB leds[1];

void led_begin() {
    // APA102 is a 2-wire SPI LED (data + clock), not a single-wire WS2812B.
    // FastLED bitbangs SPI on ESP32-S3; color order is BGR for APA102.
    FastLED.addLeds<APA102, PIN_LED_DI, PIN_LED_CLK, BGR>(leds, 1);
    FastLED.setBrightness(60);   // ~24 % — visible but not blinding
    leds[0] = CRGB::Black;
    FastLED.show();
}

void led_set(uint8_t r, uint8_t g, uint8_t b) {
    leds[0] = CRGB(r, g, b);
    FastLED.show();
}

void led_off() {
    leds[0] = CRGB::Black;
    FastLED.show();
}
