#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEUtils.h>
#include <FS.h>
#include <SPIFFS.h>
#include <PanasonicHeatpumpIR.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

const char *ssid = "your ssid";
const char *password = "pwd to ssid";

const int ir_gpio = 26;
const String switchbot_addr = "e9:ba:1b:cb:e8:68";

extern "C"
{
#include "homeintegration.h"
}
homekit_service_t *thermostat_service = {0};
homekit_service_t *temperature_service = {0};
homekit_service_t *humidity_service = {0};
homekit_service_t *relaydim_service = {0};
IRsend irsend(ir_gpio);
BLEScan *pBLEScan;
String pair_file_name = "/pair.dat";
String current_relaydim_on = "unknown";
int current_relaydim_brightness = -1;

class BLEAdvertisedDevice_cb : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    std::string address = advertisedDevice.getAddress().toString();
    //std::string name = advertisedDevice.getName();

    if (switchbot_addr.compareTo(address.c_str()) != 0)
    {
      return;
    }
    if (!advertisedDevice.haveServiceData())
    {
      return;
    }

    char *pHexService = BLEUtils::buildHexData(nullptr, (uint8_t *)advertisedDevice.getServiceData().data(), advertisedDevice.getServiceData().length());
    std::string service_data = pHexService;

    String full_str = service_data.c_str();
    String split_str[6] = {"", "", "", "", "", ""};
    uint32_t split_num[6] = {0, 0, 0, 0, 0, 0};
    char *endptr;
    uint32_t i;
    for (i = 0; i < 6; ++i)
    {
      split_str[i] = full_str.substring(i * 2, (i + 1) * 2);
      split_num[i] = strtoul(split_str[i].c_str(), &endptr, 16);
    }
    free(pHexService);

    const uint32_t mask_temp_sign = 0x80;    //128
    const uint32_t mask_temp_integer = 0x7F; //127
    const uint32_t mask_temp_decimal = 0x0F; //15
    uint32_t integer_part_temperature = split_num[4] & mask_temp_integer;
    uint32_t decimal_part_temperature = split_num[3] & mask_temp_decimal;
    float temperature = (float)integer_part_temperature + (float)decimal_part_temperature / 10;
    if ((split_num[4] & mask_temp_sign) == 0)
    {
      temperature = temperature * -1;
    }

    const uint32_t mask_humid = 0x7F; //127
    float humidity = split_num[5] & mask_humid;

    if (temperature_service)
    {
      homekit_characteristic_t *ch_current_temperature = homekit_service_characteristic_by_type(temperature_service, HOMEKIT_CHARACTERISTIC_CURRENT_TEMPERATURE);
      if (ch_current_temperature && ch_current_temperature->value.float_value != temperature)
      {
        ch_current_temperature->value.float_value = temperature;
        homekit_characteristic_notify(ch_current_temperature, ch_current_temperature->value);
      }
    }
    if (humidity_service)
    {
      homekit_characteristic_t *ch_current_humidity = homekit_service_characteristic_by_type(humidity_service, HOMEKIT_CHARACTERISTIC_CURRENT_RELATIVE_HUMIDITY);
      if (ch_current_humidity && ch_current_humidity->value.float_value != humidity)
      {
        ch_current_humidity->value.float_value = humidity;
        homekit_characteristic_notify(ch_current_humidity, ch_current_humidity->value);
      }
    }
  }
};

void setup()
{
  Serial.begin(115200);
  delay(10);

  // Mount SPIFFS file system
  if (!SPIFFS.begin(true))
  {
    Serial.print("SPIFFS mount failed");
  }

  // Connect to WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  uint32_t retry_count = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    retry_count++;
    if (retry_count > 10)
    {
      esp_restart();
    }
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  // setup homekit device
  init_hap_storage();
  set_callback_storage_change(storage_changed);
  hap_setbase_accessorytype(homekit_accessory_category_air_conditioner);
  hap_initbase_accessory_service("host", "manufacture", "0", "EspHap_AirConditioner", "1.0");
  thermostat_service = hap_add_thermostat_service("Temperature", temperature_callback, NULL);
  temperature_service = hap_add_temp_as_accessory(homekit_accessory_category_thermostat, "Temperature");
  humidity_service = hap_add_hum_as_accessory(homekit_accessory_category_thermostat, "Humidity");
  relaydim_service = hap_add_relaydim_service_as_accessory(homekit_accessory_category_lightbulb, "Light", relaydim_callback, NULL);
  hap_init_homekit_server();

  irsend.begin();

  // setup BLE
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new BLEAdvertisedDevice_cb(), true);
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(1000);
  pBLEScan->setWindow(999);
}

void loop()
{
  delay(5000);

  if (hap_homekit_is_paired())
  {
    Serial.println("BLE start scan");
    pBLEScan->start(3, false);
    pBLEScan->clearResults();
  }
}

void init_hap_storage()
{
  Serial.print("init_hap_storage");
  File fsDAT = SPIFFS.open(pair_file_name, "r");
  if (!fsDAT)
  {
    Serial.println("Failed to read file pair.dat");
    return;
  }
  int size = hap_get_storage_size_ex();
  char *buf = new char[size];
  memset(buf, 0xff, size);
  int readed = fsDAT.readBytes(buf, size);
  Serial.print("Readed bytes ->");
  Serial.println(readed);
  hap_init_storage_ex(buf, size);
  fsDAT.close();
  delete[] buf;
}

void storage_changed(char *szstorage, int size)
{
  SPIFFS.remove(pair_file_name);
  File fsDAT = SPIFFS.open(pair_file_name, "w+");
  if (!fsDAT)
  {
    Serial.println("Failed to open pair.dat");
    return;
  }
  fsDAT.write((uint8_t *)szstorage, size);
  fsDAT.close();
}

void temperature_callback(homekit_characteristic_t *ch, homekit_value_t value, void *context)
{
  Serial.println("temperature_callback");
  if (!thermostat_service)
  {
    Serial.println("service not defined");
    return;
  }

  homekit_characteristic_t *ch_target_state = homekit_service_characteristic_by_type(thermostat_service, HOMEKIT_CHARACTERISTIC_TARGET_HEATING_COOLING_STATE);
  homekit_characteristic_t *ch_target_temperature = homekit_service_characteristic_by_type(thermostat_service, HOMEKIT_CHARACTERISTIC_TARGET_TEMPERATURE);
  homekit_characteristic_t *ch_current_state = homekit_service_characteristic_by_type(thermostat_service, HOMEKIT_CHARACTERISTIC_CURRENT_HEATING_COOLING_STATE);

  if (!ch_target_state || !ch_target_temperature)
  {
    Serial.println("characteristic wrong defined");
    return;
  }

  PanasonicDKEHeatpumpIR *heatpumpIR = new PanasonicDKEHeatpumpIR();
  IRSenderBitBang irSender(ir_gpio);
  uint8_t state = ch_target_state->value.int_value;
  uint8_t temperature = ch_target_temperature->value.float_value;
  Serial.printf("set state=%d, temperature=%d\n", state, temperature);
  switch (state)
  {
  case 0: // off
    heatpumpIR->send(irSender, POWER_OFF, MODE_HEAT, FAN_1, 26, VDIR_MIDDLE, HDIR_MIDDLE);
    break;
  case 1: // heat
    heatpumpIR->send(irSender, POWER_ON, MODE_HEAT, FAN_1, int(temperature), VDIR_MIDDLE, HDIR_MIDDLE);
    break;
  case 2: // cool
    heatpumpIR->send(irSender, POWER_ON, MODE_COOL, FAN_1, int(temperature), VDIR_MIDDLE, HDIR_MIDDLE);
    break;
  case 3: // dry (not auto)
    heatpumpIR->send(irSender, POWER_ON, MODE_DRY, FAN_1, int(temperature), VDIR_MIDDLE, HDIR_MIDDLE);
    break;
  }
  ch_current_state->value = HOMEKIT_UINT8_VALUE(state);
  homekit_characteristic_notify(ch_current_state, ch_current_state->value);
  delay(200);
}

void relaydim_callback(homekit_characteristic_t *ch, homekit_value_t value, void *context)
{
  Serial.println("relaydim_callback");
  if (!relaydim_service)
  {
    Serial.println("service not defined");
    return;
  }

  homekit_characteristic_t *ch_on = homekit_service_characteristic_by_type(relaydim_service, HOMEKIT_CHARACTERISTIC_ON);
  homekit_characteristic_t *ch_brightness = homekit_service_characteristic_by_type(relaydim_service, HOMEKIT_CHARACTERISTIC_BRIGHTNESS);
  if (strcmp(ch->type, HOMEKIT_CHARACTERISTIC_ON) == 0)
  {
    String relaydim_on = ch_on->value.bool_value ? "on" : "off";
    if (relaydim_on.compareTo(current_relaydim_on) != 0)
    {
      Serial.printf("set relaydim state=%s\n", relaydim_on);
      if (ch_on->value.bool_value)
      {
        irsend.sendNEC(irsend.encodeNEC(0xC580, 0x9B));
        current_relaydim_brightness = 100;
      }
      else
      {
        irsend.sendNEC(irsend.encodeNEC(0xC580, 0x9));
        current_relaydim_brightness = 0;
      }
      current_relaydim_on = relaydim_on;
      HAP_NOTIFY_CHANGES(bool, ch_on, ch_on->value.bool_value, 0);
      HAP_NOTIFY_CHANGES(int, ch_brightness, current_relaydim_brightness, 0);
      delay(200);
    }
  }
  else if (strcmp(ch->type, HOMEKIT_CHARACTERISTIC_BRIGHTNESS) == 0)
  {
    int brightness = ch_brightness->value.int_value;
    if ((brightness / 10) != (current_relaydim_brightness / 10))
    {
      Serial.printf("set relaydim brightness=%d, current brightness=%d\n", brightness, current_relaydim_brightness);
      switch (brightness)
      {
      case 100:
        irsend.sendNEC(irsend.encodeNEC(0xC580, 0x9B));
        current_relaydim_on = "on";
        HAP_NOTIFY_CHANGES(bool, ch_on, true, 0);
        break;
      case 0:
        irsend.sendNEC(irsend.encodeNEC(0xC580, 0x9));
        current_relaydim_on = "off";
        HAP_NOTIFY_CHANGES(bool, ch_on, false, 0);
        break;
      default:
        if (current_relaydim_brightness < brightness)
        {
          for (int i = (current_relaydim_brightness / 10); i < (brightness / 10); i += 1)
          {
            Serial.println("increase relaydim brightness");
            irsend.sendNEC(irsend.encodeNEC(0xC580, 0x9F));
            delay(200);
          }
        }
        else
        {
          for (int i = (current_relaydim_brightness / 10); i > (brightness / 10); i -= 1)
          {
            Serial.println("decrease relaydim brightness");
            irsend.sendNEC(irsend.encodeNEC(0xC580, 0x97));
            delay(200);
          }
        }
        break;
      }
      current_relaydim_brightness = brightness;
      HAP_NOTIFY_CHANGES(int, ch_brightness, current_relaydim_brightness, 0);
      delay(200);
    }
  }
}
