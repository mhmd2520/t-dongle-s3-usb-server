#pragma once
#include <Arduino.h>

// Initialise the APA102 RGB LED (DATA=GPIO40, CLK=GPIO39) — call once in setup().
void led_begin();

// Set the LED to an RGB colour (0–255 each channel).
void led_set(uint8_t r, uint8_t g, uint8_t b);

// Turn the LED off.
void led_off();
