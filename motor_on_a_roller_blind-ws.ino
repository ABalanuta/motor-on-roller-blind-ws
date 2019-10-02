#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Stepper_28BYJ_48.h>
#include <WebSocketsServer.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include "FS.h"
#include "index_html.h"
#include "NidayandHelper.h"

//--------------- CHANGE PARAMETERS ------------------
//Configure Default Settings for Access Point logon
String APid = "BlindsConnectAP";    //Name of access point
String APpw = "Welcome123";           //Hardcoded password for access point

//Set up buttons
const uint8_t btnup = D1; //Up button
const uint8_t btndn = D2; //Down button

//Set up motor pins
const uint8_t motor_in1 = D5; //IN1
const uint8_t motor_in2 = D6; //IN2
const uint8_t motor_in3 = D7; //IN3
const uint8_t motor_in4 = D8; //IN4

//Set up heartbeat led
const uint8_t heartbeat = D4; //use embedded led

//----------------------------------------------------

// Version number for checking if there are new code releases and notifying the user
String version = "1.3.1";

NidayandHelper helper = NidayandHelper();

//Fixed settings for WIFI
WiFiClient espClient;
PubSubClient psclient(espClient);   //MQTT client
char mqtt_server[40];             //WIFI config: MQTT server config (optional)
char mqtt_port[6] = "1883";       //WIFI config: MQTT port config (optional)
char mqtt_uid[40];             //WIFI config: MQTT server username (optional)
char mqtt_pwd[40];             //WIFI config: MQTT server password (optional)

String outputTopic;               //MQTT topic for sending messages
String inputTopic;                //MQTT topic for listening
boolean mqttActive = true;
char config_name[40];             //WIFI config: Bonjour name of device
char config_rotation[40] = "false"; //WIFI config: Detault rotation is CCW

String action;                      //Action manual/auto
int path = 0;                       //Direction of blind (1 = down, 0 = stop, -1 = up)
int setPos = 0;                     //The set position 0-100% by the client
long currentPosition = 0;           //Current position of the blind
long maxPosition = 2000000;         //Max position of the blind. Initial value
boolean loadDataSuccess = false;
boolean saveItNow = false;          //If true will store positions to SPIFFS
bool shouldSaveConfig = false;      //Used for WIFI Manager callback to save parameters
boolean initLoop = true;            //To enable actions first time the loop is run
boolean ccw = true;                 //Turns counter clockwise to lower the curtain

boolean debug = false;              //Reduce logs
boolean supportOTA = false;         //Disable OTA if not needed

Stepper_28BYJ_48 small_stepper(motor_in1, motor_in2, motor_in3, motor_in4); //Initiate stepper driver

ESP8266WebServer server(80);              // TCP server at port 80 will respond to HTTP requests
WebSocketsServer webSocket = WebSocketsServer(81);  // WebSockets will respond on port 81

bool loadConfig() {
  if (!helper.loadconfig())
    return false;

  JsonVariant json = helper.getconfig();

  //Useful if you need to see why confing is read incorrectly
  json.printTo(Serial);

  //Store variables locally
  currentPosition = json["currentPosition"].as<long>();
  maxPosition = json["maxPosition"].as<long>();

  strcpy(config_name, json["config_name"]);
  strcpy(mqtt_server, json["mqtt_server"]);
  strcpy(mqtt_port, json["mqtt_port"]);
  strcpy(mqtt_uid, json["mqtt_uid"]);
  strcpy(mqtt_pwd, json["mqtt_pwd"]);
  strcpy(config_rotation, json["config_rotation"]);

  return true;
}

/**
   Save configuration data to a JSON file
   on SPIFFS
*/
bool saveConfig() {
  DynamicJsonBuffer jsonBuffer(300);
  JsonObject& json = jsonBuffer.createObject();
  json["currentPosition"] = currentPosition;
  json["maxPosition"] = maxPosition;
  json["config_name"] = config_name;
  json["mqtt_server"] = mqtt_server;
  json["mqtt_port"] = mqtt_port;
  json["mqtt_uid"] = mqtt_uid;
  json["mqtt_pwd"] = mqtt_pwd;
  json["config_rotation"] = config_rotation;

  return helper.saveconfig(json);
}

/*
   Connect to MQTT server and publish a message on the bus.
   Finally, close down the connection and radio
*/
void sendmsg(String topic, String payload) {
  if (!mqttActive)
    return;

  helper.mqtt_publish(psclient, topic, payload);
}

/****************************************************************************************
*/
void processMsg(String res, uint8_t clientnum) {
  /*
     Check if calibration is running and if stop is received. Store the location
  */
  if (action == "set" && res == "(0)") {
    maxPosition = currentPosition;
    saveItNow = true;
  }

  //Below are actions based on inbound MQTT payload
  if (res == "(start)") {

    //Store the current position as the start position
    currentPosition = 0;
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "(max)") {

    //Store the max position of a closed blind
    maxPosition = currentPosition;
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "(0)") {
    //Send position details to client
    int set = ceil((((float)setPos) * 100) / maxPosition);
    int pos = ceil((((float)currentPosition) * 100) / maxPosition);
    webSocket.broadcastTXT("{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
    sendmsg(outputTopic, "{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");

    //Stop
    if (action == "auto") {
      saveItNow = true;
    }
    path = 0;
    action = "manual";
  } else if (res == "(1)") {

    //Move down without limit to max position
    path = 1;
    action = "manual";
  } else if (res == "(-1)") {

    //Move up without limit to top position
    path = -1;
    action = "manual";
  } else if (res == "(update)") {
    //Send position details to client
    int set = ceil((((float)setPos) * 100) / maxPosition);
    int pos = ceil((((float)currentPosition) * 100) / maxPosition);
    sendmsg(outputTopic, "{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
    webSocket.sendTXT(clientnum, "{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
  } else if (res == "(ping)") {
    //Do nothing
  } else {
    /*
       Any other message will take the blind to a position
       Incoming value = 0-100
       path is now the position
    */

    path = maxPosition * res.toInt() / 100;
    setPos = path; //Copy path for responding to updates
    action = "auto";

    int set = ceil((((float)setPos) * 100) / maxPosition);
    int pos = ceil((((float)currentPosition) * 100) / maxPosition);
    sendmsg(outputTopic, "{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
    webSocket.broadcastTXT("{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");

    if (set == pos) {
      Serial.print(F("Position already set - ignore command"));
      //Stop
      path = 0;
      action = "manual";
    }
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_TEXT:
      Serial.printf("[%u] get Text: %s\n", num, payload);

      String res = (char*)payload;

      //Send to common MQTT and websocket function
      processMsg(res, num);
      break;
  }
}
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print(F("Message arrived ["));
  Serial.print(topic);
  Serial.print(F("] "));
  String res = "";
  for (int i = 0; i < length; i++) {
    res += String((char) payload[i]);
  }
  processMsg(res, NULL);
}

/**
  Turn of power to coils whenever the blind
  is not moving
*/
void stopPowerToCoils() {
  digitalWrite(motor_in1, LOW);
  digitalWrite(motor_in2, LOW);
  digitalWrite(motor_in3, LOW);
  digitalWrite(motor_in4, LOW);
  Serial.println(F("Motor stopped"));
}

/*
   Callback from WIFI Manager for saving configuration
*/
void saveConfigCallback () {
  shouldSaveConfig = true;
}

void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}
void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

void led_blinking(uint8_t blinks, uint32_t delay_led_on, uint32_t delay_led_off) {
  for (uint8_t i = 0; i < blinks; i++) {
    digitalWrite(heartbeat, LOW);  //LED on
    ESP.wdtFeed();
    yield();
    delay(delay_led_on);

    digitalWrite(heartbeat, HIGH); //LED off
    ESP.wdtFeed();
    yield();
    delay(delay_led_off);
  }
}

void setup(void)
{
  Serial.begin(115200);
  delay(100);
  Serial.print(F("Starting now\n"));

  pinMode(btnup, INPUT_PULLUP);
  pinMode(btndn, INPUT_PULLUP);
  //pinMode(btncfg, INPUT_PULLUP);

  pinMode(heartbeat, OUTPUT);
  digitalWrite(heartbeat, HIGH);

  //starting signal on heartbeat LED
  led_blinking(3, 100, 100);

  //Reset the action
  action = "";

  //Set the WIFI hostname
  WiFi.hostname(config_name);

  //Define customer parameters for WIFI Manager
  WiFiManagerParameter custom_text1("<br><b>Required device name:</b>");
  WiFiManagerParameter custom_config_name("Name", "Device name", config_name, 40);
  WiFiManagerParameter custom_rotation("Rotation", "Clockwise rotation", config_rotation, 40);
  WiFiManagerParameter custom_text2("<p><b>Optional MQTT server parameters:</b></p>");
  WiFiManagerParameter custom_mqtt_server("server", "MQTT server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "MQTT port", mqtt_port, 6);
  WiFiManagerParameter custom_mqtt_uid("uid", "MQTT username", mqtt_uid, 40);
  WiFiManagerParameter custom_mqtt_pwd("pwd", "MQTT password", mqtt_pwd, 40);
  WiFiManagerParameter custom_text3("<script>t = document.createElement('div');t2 = document.createElement('input');t2.setAttribute('type', 'checkbox');t2.setAttribute('id', 'tmpcheck');t2.setAttribute('style', 'width:10%');t2.setAttribute('onclick', \"if(document.getElementById('Rotation').value == 'false'){document.getElementById('Rotation').value = 'true'} else {document.getElementById('Rotation').value = 'false'}\");t3 = document.createElement('label');tn = document.createTextNode('Clockwise rotation');t3.appendChild(t2);t3.appendChild(tn);t.appendChild(t3);document.getElementById('Rotation').style.display='none';document.getElementById(\"Rotation\").parentNode.insertBefore(t, document.getElementById(\"Rotation\"));</script>");
  //Setup WIFI Manager
  WiFiManager wifiManager;

  //reset settings on startup
  delay(1000);
  if (!(digitalRead(btnup) || digitalRead(btndn))) {  
    Serial.println(F("Hold to reset on startup"));
    uint32_t restime = millis();
    while (!(digitalRead(btnup) || digitalRead(btndn))) {
      if (millis() - restime >= 5000) {
        //turn on the lED
        digitalWrite(heartbeat, LOW);
      }
      yield(); //Prevent watchdog trigger
    }
    if (millis() - restime >= 5000) {
      stopPowerToCoils();
      //starting signal on heartbeat LED
      led_blinking(10, 100, 100);
      Serial.println(F("Removing configs..."));
      helper.resetsettings(wifiManager);
    }
  }

  //turn on embedded LED
  digitalWrite(heartbeat, LOW);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  //add all your parameters here
  wifiManager.addParameter(&custom_text1);
  wifiManager.addParameter(&custom_config_name);
  wifiManager.addParameter(&custom_rotation);
  wifiManager.addParameter(&custom_text2);
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_uid);
  wifiManager.addParameter(&custom_mqtt_pwd);
  wifiManager.addParameter(&custom_text3);

  wifiManager.autoConnect(APid.c_str(), APpw.c_str());

  //turn off embedded LED
  digitalWrite(heartbeat, HIGH);
  ESP.wdtFeed();
  yield();
  delay(1000);

  //Load config upon start
  if (!SPIFFS.begin()) {
    Serial.println(F("Failed to mount file system"));
    return;
  }

  /* Save the config back from WIFI Manager.
      This is only called after configuration
      when in AP mode
  */
  if (shouldSaveConfig) {
    //read updated parameters
    strcpy(config_name, custom_config_name.getValue());
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());
    strcpy(mqtt_uid, custom_mqtt_uid.getValue());
    strcpy(mqtt_pwd, custom_mqtt_pwd.getValue());
    strcpy(config_rotation, custom_rotation.getValue());

    //Save the data
    saveConfig();
  }

  /*
     Try to load FS data configuration every time when
     booting up. If loading does not work, set the default
     positions
  */
  loadDataSuccess = loadConfig();
  if (!loadDataSuccess) {
    Serial.println(F("Unable to load saved data"));
    currentPosition = 0;
    maxPosition = 2000000;
  }
  /*
    Setup multi DNS (Bonjour)
  */
  if (MDNS.begin(config_name)) {
    Serial.println(F("MDNS responder started"));
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("ws", "tcp", 81);

  } else {
    Serial.println(F("Error setting up MDNS responder!"));
    while (1) {
      delay(1000);
    }
  }
  Serial.print("Connect to http://" + String(config_name) + ".local or http://");
  Serial.println(WiFi.localIP());

  //Start HTTP server
  server.on("/", handleRoot);
  server.onNotFound(handleNotFound);
  server.begin();

  //Start websocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);


  /* Setup connection for MQTT and for subscribed
    messages IF a server address has been entered
  */
  //Set MQTT properties
  outputTopic = helper.mqtt_gettopic("out", config_name);
  inputTopic = helper.mqtt_gettopic("in", config_name);

  if (String(mqtt_server) != "") {
    Serial.println(F("Registering MQTT server"));
    psclient.setServer(mqtt_server, String(mqtt_port).toInt());
    psclient.setCallback(mqttCallback);

  } else {
    mqttActive = false;
    Serial.println(F("NOTE: No MQTT server address has been registered. Only using websockets"));
  }

  //Set rotation direction of the blinds
  if (String(config_rotation) == "false")
    ccw = true;
  else
    ccw = false;

  //Update webpage
  INDEX_HTML.replace("{VERSION}", "V" + version);
  INDEX_HTML.replace("{NAME}", String(config_name));

  if (supportOTA) {
    //Setup OTA
    //helper.ota_setup(config_name);
    {
      // Authentication to avoid unauthorized updates
      //ArduinoOTA.setPassword(OTA_PWD);
        ArduinoOTA.setHostname(config_name);
          ArduinoOTA.onStart([]() {
          Serial.println(F("Start"));
        });
        ArduinoOTA.onEnd([]() {
          Serial.println(F("\nEnd"));
        });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
          Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
        });
        ArduinoOTA.onError([](ota_error_t error) {
          Serial.printf("Error[%u]: ", error);
          if (error == OTA_AUTH_ERROR) Serial.println(F("Auth Failed"));
          else if (error == OTA_BEGIN_ERROR) Serial.println(F("Begin Failed"));
          else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
          else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
          else if (error == OTA_END_ERROR) Serial.println(F("End Failed"));
        });
        ArduinoOTA.begin();
    }
  }

  //connected to network and ready to use signal on hearbeat LED
  led_blinking(3, 600, 500);
}

void loop(void)
{
  static int btnup_state_old = digitalRead(btnup);
  static int btndn_state_old = digitalRead(btndn);
  int btnup_state_new;
  int btndn_state_new;

  //OTA client code
  if (supportOTA) {
    ArduinoOTA.handle();
  }

  //Websocket listner
  webSocket.loop();

  //Serving the webpage
  server.handleClient();

  //MQTT client
  if (mqttActive)
    helper.mqtt_reconnect(psclient, mqtt_uid, mqtt_pwd, { inputTopic.c_str() });

  //Do nothing if 2 buttons are pressed
  btndn_state_new = digitalRead(btndn);
  btnup_state_new = digitalRead(btnup);
  if (!(!btnup_state_new && !btndn_state_new)) {
    bool pres_cont = false;

    //Button Down
    if (!btndn_state_new && btndn_state_old) {
       Serial.println(F("BtnDn has been pressed"));

      if (action == "auto") {
         //break "auto" command
         Serial.println(F("Auto move is broken by BtnDn"));
         processMsg("(0)", 0);
      } else {
        if (currentPosition > 0) {
          Serial.println(F("Set 0 by BtnDn"));
          processMsg("0", 0);
        }
      }
    }
    if (btndn_state_new && !btndn_state_old) {
       Serial.println(F("BtnDn has been released"));
       ESP.wdtFeed();
       yield();
       delay(250);
    }
    btndn_state_old = btndn_state_new;

    //Button Up
    if (!btnup_state_new && btnup_state_old) {
       Serial.println(F("BtnUp has been pressed"));

      if (action == "auto") {
         //break "auto" command
         Serial.println(F("Auto move is broken by BtnUp"));
         processMsg("(0)", 0);
      } else {
        if (currentPosition < maxPosition) {
          Serial.println(F("Set 100 by BtnUp "));
          processMsg("100", 0);
        }
      }
    }
    if (btnup_state_new && !btnup_state_old) {
       Serial.println(F("BtnUp has been released"));
       ESP.wdtFeed();
       yield();
       delay(250);
    }
    btnup_state_old = btnup_state_new;
  }

  //Storing positioning data and turns off the power to the coils
  if (saveItNow) {
    saveConfig();
    saveItNow = false;
    /*
      If no action is required by the motor make sure to
      turn off all coils to avoid overheating and less energy
      consumption
    */
    stopPowerToCoils();
  }

  //Manage actions. Steering of the blind
  if (action == "auto") {
    //Automatically open or close blind
    if (currentPosition > path) {
      if (debug) {
        Serial.println(F("Moving down"));
      }
      small_stepper.step(ccw ? -1 : 1);
      currentPosition = currentPosition - 1;
    } else if (currentPosition < path) {
      if (debug) {
        Serial.println(F("Moving up"));
      }
      small_stepper.step(ccw ? 1 : -1);
      currentPosition = currentPosition + 1;
    } else {
      path = 0;
      action = "";
      int set = ceil((((float)setPos) * 100) / maxPosition);
      int pos = ceil((((float)currentPosition) * 100) / maxPosition);
      webSocket.broadcastTXT("{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
      sendmsg(outputTopic, "{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }");
      Serial.println(F("Stopped. Reached wanted position"));
      saveItNow = true;
    }

  } else if (action == "manual" && path != 0) {

    //Manually running the blind
    small_stepper.step(ccw ? path : -path);
    currentPosition = currentPosition + path;
    if (debug) {
      Serial.println(F("Moving motor manually"));
    }
  }

  /*
    After running setup() the motor might still have
    power on some of the coils. This is making sure that
    power is off the first time loop() has been executed
    to avoid heating the stepper motor draining
    unnecessary current
  */
  if (initLoop) {
    initLoop = false;
    stopPowerToCoils();
  }
}
