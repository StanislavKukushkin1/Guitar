#include <FastLED.h>

#define LED_PIN 6 
#define NUM_LEDS 90

CRGB leds[NUM_LEDS];

void setup() { 
    Serial.begin(115200);
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(8);
    FastLED.clear(true);
}
void loop() { 
    for (int i = 0; i < NUM_LEDS; i++) { 
    FastLED.clear();

    leds[i] = CRGB::Red;
    FastLED.show();

    Serial.print("Светодиод № ");
    Serial.println(i + 1);

    delay(1000);
}

    FastLED.clear(true);
    delay(2000);
}