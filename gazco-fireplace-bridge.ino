

#include <DNSServer.h>
#include <IotWebConf.h>
#include <MQTT.h>

// TODO Overflow protection on micros()
// TODO Support secure mqtt
// TODO Fix timeout hack in IotWebConf lib
// TODO Present a better index.html, with fireplace info/controls
// TODO Support reset button
// TODO Use Homie mqqt topic structure



// ***** Fireplace Config

#define TX D2
#define CLK_PERIOD_US 1023


#define LEVEL_0_AUTO  0x381e34c41
#define LEVEL_3_AUTO  0x143234c41
#define LEVEL_0_STD 0x340224c41
#define LEVEL_1_STD 0x40124c41
#define LEVEL_2_STD 0x240324c41
#define LEVEL_3_STD 0x1c00a4c41

#define CMD_REPEAT_N 3
#define CMD_REPEAT_DELAY 50

#define CTRL_MODE_OFF     0
#define CTRL_MODE_MANUAL  1
#define CTRL_MODE_AUTO    2

#define FP_STATE_UNKNOWN  0
#define FP_STATE_0        1
#define FP_STATE_1        2
#define FP_STATE_2        3
#define FP_STATE_3        4

int8_t fp_state = FP_STATE_UNKNOWN;
int8_t ctrl_mode = CTRL_MODE_MANUAL;

uint32_t us_start;
uint32_t us_up;


// ***** WiFi/MQTT Config

const char thingName[] = "Fireplace";
const char wifiInitialApPassword[] = "Fireplace";

#define STRING_LEN 128
#define CONFIG_VERSION "mqt1"

// Reset pin at bootup
#define CONFIG_PIN D5

// -- Callback method declarations.
void wifiConnected();
void configSaved();
boolean formValidator();
void mqttMessageReceived(String &topic, String &payload);

DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;
WiFiClient net;
MQTTClient mqttClient;

char mqttServerValue[STRING_LEN];
char mqttUserNameValue[STRING_LEN];
char mqttUserPasswordValue[STRING_LEN];

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
IotWebConfParameter mqttServerParam = IotWebConfParameter("MQTT server", "mqttServer", mqttServerValue, STRING_LEN);
IotWebConfParameter mqttUserNameParam = IotWebConfParameter("MQTT user", "mqttUser", mqttUserNameValue, STRING_LEN);
IotWebConfParameter mqttUserPasswordParam = IotWebConfParameter("MQTT password", "mqttPass", mqttUserPasswordValue, STRING_LEN, "password");

boolean needMqttConnect = false;
boolean needReset = false;
unsigned long lastMqttConnectionAttempt = 0;

int8_t last_fp_state = -1;
int8_t last_ctrl_mode = -1;
bool schedule_sync = false;




void setup() 
{
  Serial.begin(115200);
  Serial.println();
  Serial.println("Starting up...");


  // ***** Fireplace setup

  pinMode(TX, OUTPUT);
  digitalWrite(TX, LOW);


  // ***** WiFi/MQTT Setup
  
  //iotWebConf.setStatusPin(STATUS_PIN);
  //iotWebConf.setConfigPin(CONFIG_PIN);
  iotWebConf.setApTimeoutMs(1000);
  iotWebConf.addParameter(&mqttServerParam);
  iotWebConf.addParameter(&mqttUserNameParam);
  iotWebConf.addParameter(&mqttUserPasswordParam);
  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setWifiConnectionCallback(&wifiConnected);
  iotWebConf.setupUpdateServer(&httpUpdater);

  // -- Initializing the configuration.
  boolean validConfig = iotWebConf.init();
  if (!validConfig)
  {
    mqttServerValue[0] = '\0';
    mqttUserNameValue[0] = '\0';
    mqttUserPasswordValue[0] = '\0';
  }

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", []{ iotWebConf.handleConfig(); });
  server.onNotFound([](){ iotWebConf.handleNotFound(); });

  mqttClient.begin(mqttServerValue, net);
  mqttClient.onMessage(mqttMessageReceived);

  
  Serial.println("Ready.");
}

void loop() 
{

  //Serial.println(iotWebConf.getApTimeoutMs());

  if(Serial.available()){
    int input = Serial.parseInt();
    pt("Got", input);
  
    executeCmd(input);
  }
  
  // -- doLoop should be called as frequently as possible.
  iotWebConf.doLoop();
  mqttClient.loop();
  
  if (needMqttConnect)
  {
    if (connectMqtt())
    {
      needMqttConnect = false;
    }
  }
  else if ((iotWebConf.getState() == IOTWEBCONF_STATE_ONLINE) && (!mqttClient.connected()))
  {
    Serial.println("MQTT reconnect");
    connectMqtt();
  }

  if (needReset)
  {
    Serial.println("Rebooting after 1 second.");
    iotWebConf.delay(1000);
    ESP.restart();
  }

  if ((iotWebConf.getState() == IOTWEBCONF_STATE_ONLINE) && (mqttClient.connected()))
  {

    if(last_fp_state != fp_state || schedule_sync){
      int level = 0;
      switch(fp_state){
        case FP_STATE_0:
          level = 0;
          break;
        case FP_STATE_1:
          level = 33;
          break;
        case FP_STATE_2:
          level = 67;
          break;
        case FP_STATE_3:
          level = 100;
          break;
      }
      
      Serial.print("Sending on MQTT channel '/fireplace/level_percent' :");
      Serial.println(level);
      Serial.print("Sending on MQTT channel '/fireplace/level' :");
      Serial.println(fp_state-1);
      
      mqttClient.publish("/fireplace/level_percent", String(level));
      mqttClient.publish("/fireplace/level", String(fp_state-1));
      
      last_fp_state = fp_state;
    }
    
    if(last_ctrl_mode != ctrl_mode || schedule_sync){      
      String ctrl_string = String(ctrl_mode);
      Serial.print("Sending on MQTT channel '/fireplace/mode' :");
      Serial.println(ctrl_string);
      mqttClient.publish("/fireplace/mode", ctrl_string);
      last_ctrl_mode = ctrl_mode;
    }

    schedule_sync = false;
  }
}

void executeCmd(int cmd){
  switch(cmd){
      case 0:
        set_level_n(LEVEL_0_STD);
        fp_state = FP_STATE_0;
        break;
      case 1:
        set_level_n(LEVEL_1_STD);
        fp_state = FP_STATE_1;
        break;
      case 2:
        set_level_n(LEVEL_2_STD);
        fp_state = FP_STATE_2;
        break;
      case 3:
        set_level_n(LEVEL_3_STD);
        fp_state = FP_STATE_3;
        break;
      case 4:
        set_level_n(LEVEL_0_AUTO);
        fp_state = FP_STATE_0;
        break;
      case 5:
        set_level_n(LEVEL_3_AUTO);
        fp_state = FP_STATE_3;
        break;
      default:
        Serial.print("Unknown command: ");
        Serial.println(cmd);
    }
}

void pt(char * str, uint32_t t){
  Serial.print(str);
  Serial.print(": ");
  Serial.println(t);
}


/**
 * Transmit the command.
 * Basically a bitbanging spi line.
 */
void set_level(uint64_t seq){

  us_start = micros();

  for(int i=0; i < 34; i++){

    uint8_t state = ((seq >> i) & 0x01);
    digitalWrite(TX, state);
    
    // Wait for next transition
    us_up = us_start + (i+1)*CLK_PERIOD_US;
    while(micros() < us_up){
      // nop
    }
  }
  
  digitalWrite(TX, LOW);

  schedule_sync = true;
}

void set_level_n(uint64_t seq){
  Serial.println("Executing command");
  
  for(int i=0; i < CMD_REPEAT_N; i++){
    set_level(seq);
    delay(CMD_REPEAT_DELAY);
  }

}






/**
 * Handle web requests to "/" path.
 */
void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>IotWebConf 06 MQTT App</title></head><body>MQTT App demo";
  s += "<ul>";
  s += "<li>MQTT server: ";
  s += mqttServerValue;
  s += "</ul>";
  s += "Go to <a href='config'>configure page</a> to change values.";
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}



void wifiConnected()
{
  needMqttConnect = true;
}

void configSaved()
{
  Serial.println("Configuration was updated.");
  needReset = true;
}

boolean formValidator()
{
  Serial.println("Validating form.");
  boolean valid = true;

  int l = server.arg(mqttServerParam.getId()).length();
  if (l < 3)
  {
    mqttServerParam.errorMessage = "Please provide at least 3 characters!";
    valid = false;
  }

  return valid;
}

boolean connectMqtt() {
  unsigned long now = millis();
  if (1000 > now - lastMqttConnectionAttempt)
  {
    // Do not repeat within 1 sec.
    return false;
  }
  Serial.println("Connecting to MQTT server...");
  if (!connectMqttOptions()) {
    lastMqttConnectionAttempt = now;
    return false;
  }
  Serial.println("Connected!");

  mqttClient.subscribe("/fireplace/cmd");
  return true;
}

boolean connectMqttOptions()
{
  boolean result;
  if (mqttUserPasswordValue[0] != '\0')
  {
    result = mqttClient.connect(iotWebConf.getThingName(), mqttUserNameValue, mqttUserPasswordValue);
  }
  else if (mqttUserNameValue[0] != '\0')
  {
    result = mqttClient.connect(iotWebConf.getThingName(), mqttUserNameValue);
  }
  else
  {
    result = mqttClient.connect(iotWebConf.getThingName());
  }
  return result;
}

void mqttMessageReceived(String &topic, String &payload)
{
  Serial.println("Incoming: " + topic + " - " + payload);
  int cmd = payload.toInt();

  executeCmd(cmd);
  
}
