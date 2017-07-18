#include "thinx-lib-esp.h"
#include "thinx.h"

extern "C" {
  #include "user_interface.h"
}

THiNX::THiNX() {
  // We could init from SPIFFS directly but it is initially empty anyway
  // and otherwise it could cause a lot of distraction.
}

// Should use THINX_API_KEY from thinx.h
THiNX::THiNX(String __apikey) {
  thinx_api_key = __apikey;
  autoconf_ssid  = "AP-THiNX"; // SSID in AP mode
  autoconf_pwd   = "PASSWORD"; // fallback to default password, however this should be generated uniquely as it is logged to console
  thx_udid[64] = {0};
  status = WL_IDLE_STATUS;
  shouldSaveConfig = false;
  connected = false;
  once = true;
  mqtt_client = NULL;

  initWithAPIKey(thinx_api_key);
}

// Designated initializer
void THiNX::initWithAPIKey(String api_key) {

  // Import build-time values from thinx.h
  thinx_commit_id = String(THINX_COMMIT_ID);
  thinx_mqtt_url = String(THINX_MQTT_URL);
  thinx_cloud_url = String(THINX_CLOUD_URL);
  thinx_firmware_version = String(THINX_FIRMWARE_VERSION);
  thinx_firmware_version_short = String(THINX_FIRMWARE_VERSION_SHORT);
  app_version = String(THINX_APP_VERSION);

  thinx_mqtt_port = THINX_MQTT_PORT;
  thinx_api_port = THINX_API_PORT;

  // dynamic variables
  thinx_alias = String(THINX_ALIAS);
  thinx_owner = String(THINX_OWNER);
  thinx_udid = String(THINX_UDID);

  if (once == true) {
    once = false;
  } else {
     return;
  }

  if (api_key != "") {
    THiNX::thinx_api_key = api_key;
    sprintf(thx_api_key, "%s", thinx_api_key.c_str()); // 40 max
  }

  Serial.println("*TH: Mounting SPIFFS...");
  bool result = SPIFFS.begin();
  delay(50);
  Serial.println("*TH: SPIFFS mounted: " + result);

  restoreDeviceInfo();

  Serial.println("*TH: Connecting...");

  connect();

#ifdef __DEBUG__
  if (connected) {
    Serial.println("*TH: Connected to WiFi...");
  } else {
    Serial.println("*TH: Connection to WiFi failed...");
  }
#endif

  //delay(5000);
  Serial.println("*TH: Checking in...");
  checkin();

  delay(1000);

  mqtt_result = start_mqtt(); // requires valid udid and api_keys, and allocated WiFiClient.

  if (mqtt_result == true) {
    Serial.println("*TH: Starting MQTT...");
  } else {
    Serial.println("*TH: MQTT delayed...");
  }

#ifdef __DEBUG__
  // test == our tenant name from THINX platform
  // Serial.println("[update] Trying direct update...");
  // esp_update("/bin/test/firmware.elf");
#endif
}

/* Should be moved to private library method */

void THiNX::esp_update(String url) {

#ifdef __DEBUG__
  Serial.println("[update] Starting update...");
#endif

// #define __USE_ESP__
#ifdef __USE_ESP__
  uint32_t size = pub.payload_len();
  if (ESP.updateSketch(*pub.payload_stream(), size, true, false)) {
    Serial.println("Clearing retained message.");
    THiNX::mqtt_client->publish(MQTT::Publish(pub.topic(), "").set_retain());
    THiNX::mqtt_client->disconnect();

    Serial.printf("Update Success: %u\nRebooting...\n", millis() - startTime);
    ESP.restart();
    delay(10000);
  }
#else

  t_httpUpdate_return ret = ESPhttpUpdate.update("thinx.cloud", 80, url.c_str());

  switch(ret) {
    case HTTP_UPDATE_FAILED:
    Serial.println("[update] Update failed.");
    break;
    case HTTP_UPDATE_NO_UPDATES:
    Serial.println("[update] Update no Update.");
    break;
    case HTTP_UPDATE_OK:
    Serial.println("[update] Update ok."); // may not called we reboot the ESP
    delay(1000);
    break;
  }

  if (ret != HTTP_UPDATE_OK) {
    Serial.println("[update] WiFi connected, trying advanced update...");
    Serial.println("[update] TODO: Rewrite to secure binary provider on the API side!");
    ret = ESPhttpUpdate.update("images.thinx.cloud", 80, "ota.php", "5ccf7fee90e0");
    switch(ret) {
      case HTTP_UPDATE_FAILED:
      Serial.println("[update] Update failed.");
      break;
      case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[update] Update no Update.");
      break;
      case HTTP_UPDATE_OK:
      Serial.println("[update] Update ok."); // may not called we reboot the ESP
      break;
    }
  }

#endif

}

/* Private library definitions */


/* Private library method */

void THiNX::thinx_parse(String payload) {

  // TODO: Should parse response only for this device_id (which must be internal and not a mac)
  int startIndex = payload.indexOf("{\"registration\"") ;
  int endIndex = payload.indexOf("}}") + 2;
  String body = payload.substring(startIndex, endIndex);

#ifdef __DEBUG__
    Serial.print("*TH: Parsing response: '");
    Serial.print(body);
    Serial.println("'");
#endif

  JsonObject& root = jsonBuffer.parseObject(body.c_str());

  if ( !root.success() ) {
#ifdef __DEBUG__
  Serial.println("Failed parsing root node.");
#endif
    return;
  }

  JsonObject& registration = root["registration"];

  if ( !registration.success() ) {
#ifdef __DEBUG__
    Serial.println("Failed parsing registration node.");
#endif
    return;
  }

  bool success = registration["success"];
  String status = registration["status"];

#ifdef __DEBUG__
    Serial.print("success: ");
    Serial.println(success);

    Serial.print("status: ");
    Serial.println(status);
#endif

  if (status == "OK") {

    String alias = registration["alias"];
    Serial.print("Reading alias: ");
    Serial.print(alias);
    if ( alias.length() > 0 ) {
      Serial.println(String("assigning alias: ") + alias);
      thinx_alias = alias;
    }

    String owner = registration["owner"];
    Serial.println("Reading owner: ");
    if ( owner.length() > 0 ) {
      Serial.println(String("assigning owner: ") + owner);
      thinx_owner = owner;
    }

    Serial.println("Reading udid: ");
    String udid = registration["udid"];
    if ( udid.length() > 0 ) {
      Serial.println(String("assigning udid: ") + udid);
      THiNX::thinx_udid = udid;
    }

    delay(1);

    saveDeviceInfo();

  } else if (status == "FIRMWARE_UPDATE") {

    String mac = registration["mac"];
    Serial.println(String("mac: ") + mac);
    // TODO: must be this or ANY

    String commit = registration["commit"];
    Serial.println(String("commit: ") + commit);

    // should not be this
    if (commit == thinx_commit_id) {
      Serial.println("*TH: Warning: new firmware has same commit_id as current.");
    }

    String version = registration["version"];
    Serial.println(String("version: ") + version);

    Serial.println("Starting update...");

    String url = registration["url"];
    if (url) {
#ifdef __DEBUG__
      Serial.println("*TH: SKIPPING force update with URL:" + url);
#else
      // TODO: must not contain HTTP, extend with http://thinx.cloud/" // could use node.js as a secure provider instead of Apache!
      esp_update(url);
#endif
    }
  }
}

/* Private library method */

/*
* Designated MQTT channel for each device. Devices can talk to each other
* through /thinx/owner_id/shared/ channels.
*/

String THiNX::thinx_mqtt_channel() {
  return String("/") + thinx_owner + "/" + thinx_udid;
}

String THiNX::thinx_mqtt_status_channel() {
  return String("/") + thinx_owner + "/" + thinx_udid + "/status";
}

/* Private library method */

/*
* May return hash only in future.
*/

String THiNX::thinx_mac() {
  byte mac[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  WiFi.macAddress(mac);
  char macString[16] = {0};
  sprintf(macString, "5CCF7F%6X", ESP.getChipId());
  return String(macString);
}

/* Private library method */

void THiNX::checkin() {

  Serial.println("*TH: Starting API checkin...");

  if(!connected) {
    Serial.println("*TH: Cannot checkin while not connected, exiting.");
    return;
  }

  // Default MAC address for AP controller
  byte mac[] = {
    0xDE, 0xFA, 0xDE, 0xFA, 0xDE, 0xFA
  };

  Serial.println("*TH: Building JSON...");

  String tmac = thinx_mac();
  String fw = thinx_firmware_version;
  String fws = thinx_firmware_version_short;
  String cid = String(thinx_commit_id);
  String oid = thinx_owner;
  String als = thinx_alias;
  String uid = thinx_udid;
  Serial.print("UDID:");
  Serial.println(thinx_udid);

  JsonObject& root = jsonBuffer.createObject();
  root["mac"] = thinx_mac();
  root["firmware"] = thinx_firmware_version;
  root["version"] = thinx_firmware_version_short;
  root["commit"] = thinx_commit_id;
  root["owner"] = thinx_owner;
  root["alias"] = thinx_alias;

  if (thinx_udid != NULL) {
    root["udid"] = thinx_udid;
  }

  root["platform"] = String(THINX_PLATFORM);

  Serial.println("*TH: Wrapping JSON...");

  JsonObject& wrapper = wrapperBuffer.createObject();
  wrapper["registration"] = root;

#ifdef __DEBUG_JSON__
  wrapper.printTo(Serial);
  Serial.println();
#endif

  String body;
  wrapper.printTo(body);
  senddata(body);
}

/* Private library method */

//
// MQTT Connection
//

bool THiNX::start_mqtt() {

  if (thinx_udid.length() == 0) {
    return false;
  }

  Serial.print("*TH: UDID: ");
  Serial.println(thinx_udid);

  Serial.print("*TH: Contacting MQTT server ");
  Serial.println(thinx_mqtt_url);

  //PubSubClient mqtt_client(thx_wifi_client, thinx_mqtt_url.c_str());
  Serial.print("*TH: Starting client");
  if (mqtt_client == NULL) {
    mqtt_client = new PubSubClient(*thx_wifi_client, thinx_mqtt_url.c_str());
  }

  Serial.print(" on port ");
  Serial.println(thinx_mqtt_port);

  last_mqtt_reconnect = 0;

  String channel = thinx_mqtt_channel();
  Serial.println("*TH: Connecting to MQTT...");


  Serial.print("*TH: AK: ");
  Serial.println(thinx_api_key);
  Serial.print("*TH: CH: ");
  Serial.println(channel);

  String mac = thinx_mac();

  const char* id = mac.c_str();
  const char* user = thinx_udid.c_str();
  const char* pass = thinx_api_key.c_str();
  const char* willTopic = thinx_mqtt_channel().c_str();
  int willQos = 0;
  bool willRetain = false;

  if (mqtt_client->connect(MQTT::Connect(id)
                .set_clean_session()
                .set_will(willTopic, thx_disconnected_response.c_str())
                .set_auth(user, pass)
                .set_keepalive(30)
              )) {

    //! Set the callback function
    // PubSubClient& set_callback(callback_t cb) { _callback = cb; return *this; }

    mqtt_client->set_callback([this](const MQTT::Publish &pub) {
      Serial.print("*TH: MQTT Callback Zero...");
      this->mqtt_callback(pub);
    });

    Serial.print("*TH: MQTT Subscribing device channel: ");
    Serial.println(thinx_mqtt_channel());
    if (mqtt_client->subscribe(thinx_mqtt_channel().c_str())) {
      Serial.print("*TH: ");
      Serial.print(thinx_mqtt_channel());
      Serial.println(" successfully subscribed.");
    } else {
      Serial.println("*TH: Not subscribed.");
    }

    mqtt_client->publish(thinx_mqtt_status_channel().c_str(), thx_connected_response.c_str());
    // mqtt_client->publish(thinx_mqtt_status_channel().c_str(), thx_connected_response.c_str()); TODO: send registration JSON for possible firmware updates

    return true;
  } else {
    Serial.println("*TH: MQTT Not connected.");
    return false;
  }
}

//
// EAVManager Setup Callbacks
//


// `api_key_param` should have its value set when this gets called
// ICACHE_FLASH_ATTR
void THiNX::saveConfigCallback() {
  Serial.println("Save config callback:");
  strcpy(thx_api_key, api_key_param->getValue());
  if (String(thx_api_key).length() > 0) {
    THiNX::thinx_api_key = String(thx_api_key);
    Serial.print("Saving thinx_api_key: ");
    Serial.println(thinx_api_key);
    //Will be saved on checkin? NO! Will be lost on reset!
    Serial.println("SAVE:2");
    saveDeviceInfo();
  }
}

/* Private library method */

//
// WiFi Connection
//

void THiNX::connect() { // should return status bool
  thx_wifi_client = new WiFiClient();
  manager = new EAVManager();

  EAVManagerParameter *api_key_param = new EAVManagerParameter("apikey", "API Key", thx_api_key, 64);
  manager->addParameter(api_key_param);
  manager->setTimeout(10000);
  manager->autoConnect(autoconf_ssid,autoconf_pwd);

  // attempt to connect to Wifi network:
  while ( !connected ) {
    status = manager->autoConnect(autoconf_ssid,autoconf_pwd);
    if (status == true) {
      connected = true;
      return;
    } else {
      Serial.print("*TH: Connection Status: false!");
      delay(3000);
      connected = false;
    }
  }
}

//
// PERSISTENCE
//

bool THiNX::restoreDeviceInfo() {

  File f = SPIFFS.open("/thinx.cfg", "r");
  if (!f) {
      Serial.println("*TH: No remote configuration found so far...");
      return false;
  } else {
    String data = f.readStringUntil('\n');
    JsonObject& config = jsonBuffer.parseObject(data.c_str());
    if (!config.success()) {
      Serial.println("*TH: parseObject() failed");
    } else {

      const char* saved_alias = config["alias"];
      if (strlen(saved_alias) > 1) {
        thinx_alias = String(saved_alias);
      }

      const char* saved_owner = config["owner"];
      if (strlen(saved_owner) > 5) {
        thinx_owner = String(saved_owner);
      }

      const char* saved_apikey = config["apikey"];
      if (strlen(saved_apikey) > 8) {
       thinx_api_key = String(saved_apikey);
       sprintf(thx_api_key, "%s", saved_apikey); // 40 max
      }

      const char* saved_udid = config["udid"];
      Serial.print("*TH: Saved udid: "); Serial.println(saved_udid);
      if ((strlen(saved_udid) > 1)) {
       thinx_udid = String(saved_udid);
     } else {
       thinx_udid = THINX_UDID;
     }
     sprintf(thx_udid, "%s", THiNX::thinx_udid.c_str()); // 40 max

     f.close();
    }
  }
}

/* Stores mutable device data (alias, owner) retrieved from API */
void THiNX::saveDeviceInfo()
{

  //Serial.println("*TH: Opening/creating config file...");

  const char *config = deviceInfo().c_str();
  Serial.println(config);
  Serial.println("*TH: Crashes even when NOT Writing configuration to file because it would crash, everything stays in memory, api key must be set manually so far...");

  File f = SPIFFS.open("/thx.cfg", "w");
  if (f) {
    Serial.print("*TH: saving configuration: ");
    f.println(config);
    Serial.println("*TH: closing file crashes here...");
    f.close();
  } else {
    Serial.println("*TH: Cannot save configuration, formatting SPIFFS...");
    SPIFFS.format();
    Serial.println("*TH: Trying to save again..."); // TODO: visual feedback
    f = SPIFFS.open("/thinx.cfg", "w");
    if (f) {
      saveDeviceInfo();
      f.close();
    } else {
      Serial.println("*TH: Retry failed...");
    }
  }

  Serial.println("*TH: saveDeviceInfo() completed.");
  SPIFFS.end();
  Serial.println("*TH: SPIFFS.end();");

  restoreDeviceInfo();
}

String THiNX::deviceInfo()
{
  Serial.println("*TH: building device info:");
  JsonObject& root = jsonBuffer.createObject();
  root["alias"] = THiNX::thinx_alias;
  Serial.print("*TH: thinx_alias: ");
  Serial.println(thinx_alias);

  root["owner"] = THiNX::thinx_owner;
  Serial.print("*TH: thinx_owner: ");
  Serial.println(thinx_owner);

  root["apikey"] = THiNX::thx_api_key;
  Serial.print("*TH: thx_api_key: ");
  Serial.println(thx_api_key);

  root["udid"] = THiNX::thinx_udid;
  Serial.print("*TH: thinx_udid: ");
  Serial.println(thinx_udid);

  String jsonString;
  root.printTo(jsonString);

  return jsonString;
}

// Private: HTTP Communication to API

void THiNX::senddata(String body) {
  char shorthost[256] = {0};
  sprintf(shorthost, "%s", thinx_cloud_url.c_str());
  String payload = "";
  if (thx_wifi_client->connect(shorthost, 7442)) {
    // Standard public THiNX-Client device registration request
    // (same request can be called in order to add matching mobile application push token)
    thx_wifi_client->println("POST /device/register HTTP/1.1");
    thx_wifi_client->println("Host: thinx.cloud");
    thx_wifi_client->print("Authentication: "); thx_wifi_client->println(thx_api_key);
    thx_wifi_client->println("Accept: application/json"); // application/json
    thx_wifi_client->println("Origin: device");
    thx_wifi_client->println("Content-Type: application/json");
    thx_wifi_client->println("User-Agent: THiNX-Client");
    thx_wifi_client->print("Content-Length: ");
    thx_wifi_client->println(body.length());
    thx_wifi_client->println();
    Serial.println("Headers set...");
    thx_wifi_client->println(body);
    Serial.println("Body sent...");

    long interval = 10000;
    unsigned long currentMillis = millis(), previousMillis = millis();

    while(!thx_wifi_client->available()){
      if( (currentMillis - previousMillis) > interval ){
        Serial.println("Response Timeout. TODO: Should retry later.");
        thx_wifi_client->stop();
        return;
      }
      currentMillis = millis();
    }

    while ( thx_wifi_client->connected() ) {
      if ( thx_wifi_client->available() ) {
        char str = thx_wifi_client->read();
        payload = payload + String(str);
      }
    }

    thx_wifi_client->stop();
    thinx_parse(payload);

  } else {
    Serial.println("*TH: API connection failed.");
    return;
  }
}

void THiNX::publish() {
  if (mqtt_client == NULL) return;
  if (mqtt_client->connected()) {
    Serial.print("*TH: MQTT still connected...");
  } else {
    String channel = thinx_mqtt_channel();
    String message = thx_connected_response;
    Serial.println("*TH: MQTT not connected, reconnecting...");
    mqtt_result = start_mqtt();
    if (mqtt_result && mqtt_client->connected()) {
      mqtt_client->publish(channel.c_str(), message.c_str());
      Serial.println("*TH: MQTT reconnected, published default message.");
    } else {
      Serial.println("*TH: Reconnect failed...");
    }
  }
}

void THiNX::loop() {
  Serial.println(".");
  uint32_t memfree = system_get_free_heap_size();
  Serial.print("PRE-PUBLISH memfree: ");
  Serial.println(memfree);
  publish();
  Serial.print("POST-PUBLISH memfree: ");
  memfree = system_get_free_heap_size();
  Serial.println(memfree);
}
