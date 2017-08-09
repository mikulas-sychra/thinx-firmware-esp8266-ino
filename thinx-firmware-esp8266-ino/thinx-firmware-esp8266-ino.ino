/* OTA enabled firmware for Wemos D1 (ESP 8266, Arduino) */

#include "Arduino.h"
#include <THiNXLib.h>

#define __DEBUG_WIFI__ /* use as fallback when device gets stucked with incorrect WiFi configuration, overwrites Flash in ESP */
#ifdef __DEBUG_WIFI__
#include "Settings.h" /* inject WiFi SSID/password here for testing */
#endif

THiNX thx;

void setup() {
  Serial.begin(115200);

#ifdef __DEBUG__
  while (!Serial);
#else
  delay(500);
#endif

  const char* apikey = "71679ca646c63d234e957e37e4f4069bf4eed14afca4569a0c74abf503076732";
  thx = THiNX(apikey);
  
  Serial.println("Setup completed.");
}

void loop()
{
  delay(10000);
  thx.loop(); // registers, checks MQTT status, reconnects, updates, etc.
  Serial.printf("Free size: %u\n", ESP.getFreeSketchSpace());
}
