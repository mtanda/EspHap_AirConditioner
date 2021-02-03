#include <WiFi.h>
#include <FS.h>
#include <SPIFFS.h>
#include <PanasonicHeatpumpIR.h>

const char *ssid = "your ssid";
const char *password = "pwd to ssid";

const int ir_gpio = 26;

extern "C"
{
#include "homeintegration.h"
}
homekit_service_t *thermostat_service = {0};
String pair_file_name = "/pair.dat";

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

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
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
  hap_init_homekit_server();
}

void loop()
{
  // put your main code here, to run repeatedly:
  delay(10);
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
}
