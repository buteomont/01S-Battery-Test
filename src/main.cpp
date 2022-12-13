/**
 * This is an ESP8266 program to help test battery life.  It utilizes the ESP8266's  
 * sleep mode to minimize battery usage.  It will wake up at regular intervals and
 * report the battery voltage.
 *
 * Configuration is done via serial connection. */

 
#include <PubSubClient.h> 
#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include <math.h>
#include "batteryTest.h"

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

boolean stayAwake=false;

// These are the settings that get stored in EEPROM.  They are all in one struct which
// makes it easier to store and retrieve.
typedef struct 
  {
  unsigned int validConfig=0; 
  char ssid[SSID_SIZE] = "";
  char wifiPassword[PASSWORD_SIZE] = "";
  char mqttBrokerAddress[ADDRESS_SIZE]=""; //default
  int mqttBrokerPort=1883;
  char mqttUsername[USERNAME_SIZE]="";
  char mqttPassword[PASSWORD_SIZE]="";
  char mqttTopic[MQTT_TOPIC_SIZE]="";
  int sleepTime=10; //seconds to sleep between reports
  char mqttClientId[MQTT_CLIENTID_SIZE]=""; //will be the same across reboots
  bool debug=false;
  char address[ADDRESS_SIZE]=""; //static address for this device
  char netmask[ADDRESS_SIZE]=""; //size of network
  } conf;

conf settings; //all settings in one struct makes it easier to store in EEPROM
boolean settingsAreValid=false;

String commandString = "";     // a String to hold incoming commands from serial
bool commandComplete = false;  // goes true when enter is pressed

unsigned long doneTimestamp=0; //used to allow publishes to complete before sleeping

//This is true if a package is detected. It will be written to RTC memory 
// as "wasPresent" just before sleeping
bool isPresent=false;

//This is the distance measured on this pass. It will be written to RTC memory just before sleeping
int distance=0;

ADC_MODE(ADC_VCC); //so we can use the ADC to measure the battery voltage

IPAddress ip;
IPAddress mask;

void otaSetup()
  {
  // Port defaults to 3232
  // ArduinoOTA.setPort(3232);

  // Hostname defaults to esp3232-[MAC]
  // ArduinoOTA.setHostname("myesp32");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() 
    {
    stayAwake=true; //don't go to sleep during an update!
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
    });

    ArduinoOTA.onEnd([]() {
      Serial.println("\nEnd");
      stayAwake=false; //ok, you can sleep now 
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) 
      {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
      });

    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
      stayAwake=false; //no sense in running down the battery if failure
    });

  ArduinoOTA.begin();
  }

void setup() 
  {  
  wifi_status_led_uninstall(); //get rid of the blue LED to save power
  pinMode(LED_BLUE,INPUT);    // Still wants to flash on every wakeup
  digitalWrite(LED_BLUE,HIGH);// maybe this will help

  //system_update_cpu_freq(80);

  Serial.begin(9600);
  Serial.setTimeout(10000);
  Serial.println();
  
  while (!Serial); // wait here for serial port to connect.
  Serial.println("Serial line initialized.");

  EEPROM.begin(sizeof(settings)); //fire up the eeprom section of flash
  commandString.reserve(200); // reserve 200 bytes of serial buffer space for incoming command string

  loadSettings(); //set the values from eeprom
  if (settings.mqttBrokerPort < 0) //then this must be the first powerup
    {
    Serial.println("\n*********************** Resetting All EEPROM Values ************************");
    initializeSettings();
    saveSettings();
    delay(2000);
    ESP.restart();
    }

  if (settingsAreValid)
    {
    if (settings.sleepTime==0) //another way to keep it from sleeping
      stayAwake=true;

    if (!ip.fromString(settings.address))
      {
      Serial.println("IP Address "+String(settings.address)+" is not valid. Using dynamic addressing.");
      // settingsAreValid=false;
      // settings.validConfig=false;
      }
    else if (!mask.fromString(settings.netmask))
      {
      Serial.println("Network mask "+String(settings.netmask)+" is not valid.");
      // settingsAreValid=false;
      // settings.validConfig=false;
      }

    if (connectToWiFi()) // attempt to connect to Wifi network
      {
      otaSetup(); //initialize the OTA stuff
      reconnect();  // connect to the MQTT broker

      //Get a measurement. 
      int analog=readBattery();
      
      Serial.print("Analog input is ");
      Serial.println(analog);

      Serial.print("Battery voltage: ");
      Serial.println(convertToVoltage(analog));

      send(); //decide whether or not to send a report
      }
    }
  else
    {
    stayAwake=true; //can't enter settings if it's asleep
    showSettings();
    }
  }
 
void loop()
  {
  static unsigned long nextReport=millis()+max(settings.sleepTime*1000,1000); //one second minimum between reports

  if (settingsAreValid)
    {
    ArduinoOTA.handle(); //Check for new version
    mqttClient.loop(); //This has to happen every so often or we get disconnected for some reason
    }

  checkForCommand(); // Check for input in case something needs to be changed to work

  if (!stayAwake && settingsAreValid           //setup has been done and
      && millis()-doneTimestamp>PUBLISH_DELAY) //waited long enough for report to finish
    {
    Serial.print("Sleeping for ");
    Serial.print(settings.sleepTime);
    Serial.println(" seconds");

    WiFi.disconnect(true);
    yield();  
    ESP.deepSleep(settings.sleepTime*1000000, WAKE_RF_DEFAULT); //tried WAKE_RF_DISABLED but can't wake it back up
    }
  else if (millis() > nextReport) 
    {
    send();
    nextReport=millis()+max(settings.sleepTime*1000,1000); //one second minimum between reports
    }
  }

boolean send()
  {
  boolean ok=true;  //in case settings are not valid
  if (settingsAreValid)
    {
    if (connectToWiFi())// ********************* attempt to connect to Wifi network
      {
      ok=reconnect();  // connect to the MQTT broker  
      if (ok)
        report();
      }
    else 
      ok=false;
    }

  /////// idea: change this to send a sleep command to ourself via mqtt. That way the previous
  /////// report will surely have been delivered by the time we sleep.
  doneTimestamp=millis(); //this is to allow the publish to complete before sleeping
  return ok;
  }

  
/*
 * If not connected to wifi, connect.
 */
boolean connectToWiFi()
  {
  boolean connected=true; // assume all ok 
  if (WiFi.status() != WL_CONNECTED)
    {
    Serial.print("Attempting to connect to WPA SSID \"");
    Serial.print(settings.ssid);
    Serial.println("\"");

//    WiFi.forceSleepWake(); //turn on the radio
//    delay(1);              //return control to let it come on
    
    WiFi.mode(WIFI_STA); //station mode, we are only a client in the wifi world

    if (ip.isSet()) //Go with a dynamic address if no valid IP has been entered
      {
      if (!WiFi.config(ip,ip,mask))
        {
        Serial.println("STA Failed to configure");
        }
      }
    WiFi.begin(settings.ssid, settings.wifiPassword);
    int8 wifiTries=WIFI_ATTEMPTS;
    while (WiFi.status() != WL_CONNECTED && wifiTries-- > 0) 
      {
      // not yet connected
      Serial.print(".");
      checkForCommand(); // Check for input in case something needs to be changed to work
      delay(500);
      }
    connected=wifiTries>0;
  
    if (connected)
      {
      Serial.print("Connected to network with address ");
      Serial.println(WiFi.localIP());
      Serial.println();
      }
    else
      {
      Serial.println("Failed to connect to network.");
      }
    }
  else if (settings.debug)
    {
    Serial.print("Actual network address is ");
    Serial.println(WiFi.localIP());
    }
  return connected;
  }

/**
 * Handler for incoming MQTT messages.  The payload is the command to perform. 
 * The MQTT message topic sent is the topic root plus the command.
 * Implemented commands are: 
 * MQTT_PAYLOAD_SETTINGS_COMMAND: sends a JSON payload of all user-specified settings
 * MQTT_PAYLOAD_REBOOT_COMMAND: Reboot the controller
 * MQTT_PAYLOAD_VERSION_COMMAND Show the version number
 * MQTT_PAYLOAD_STATUS_COMMAND Show the most recent flow values
 */
void incomingMqttHandler(char* reqTopic, byte* payload, unsigned int length) 
  {
  if (settings.debug)
    {
    Serial.println("====================================> Callback works.");
    }
  payload[length]='\0'; //this should have been done in the caller code, shouldn't have to do it here
  boolean rebootScheduled=false; //so we can reboot after sending the reboot response
  char charbuf[100];
  sprintf(charbuf,"%s",payload);
  const char* response;
  
  
  //if the command is MQTT_PAYLOAD_SETTINGS_COMMAND, send all of the settings
  if (strcmp(charbuf,MQTT_PAYLOAD_SETTINGS_COMMAND)==0)
    {
    char tempbuf[35]; //for converting numbers to strings
    char jsonStatus[JSON_STATUS_SIZE];
    
    strcpy(jsonStatus,"{");
    strcat(jsonStatus,"\"broker\":\"");
    strcat(jsonStatus,settings.mqttBrokerAddress);
    strcat(jsonStatus,"\", \"port\":");
    sprintf(tempbuf,"%d",settings.mqttBrokerPort);
    strcat(jsonStatus,tempbuf);
    strcat(jsonStatus,", \"mqttTopic\":\"");
    strcat(jsonStatus,settings.mqttTopic);
    strcat(jsonStatus,"\", \"user\":\"");
    strcat(jsonStatus,settings.mqttUsername);
    strcat(jsonStatus,"\", \"pass\":\"");
    strcat(jsonStatus,settings.mqttPassword);
    strcat(jsonStatus,"\", \"ssid\":\"");
    strcat(jsonStatus,settings.ssid);
    strcat(jsonStatus,"\", \"wifipass\":\"");
    strcat(jsonStatus,settings.wifiPassword);
    strcat(jsonStatus,"\", \"sleepTime\":\"");
    sprintf(tempbuf,"%d",settings.sleepTime);
    strcat(jsonStatus,tempbuf);
    strcat(jsonStatus,"\", \"mqttClientId\":\"");
    strcat(jsonStatus,settings.mqttClientId);
    strcat(jsonStatus,"\", \"address\":\"");
    strcat(jsonStatus,settings.address);
    strcat(jsonStatus,"\", \"netmask\":\"");
    strcat(jsonStatus,settings.netmask);
    strcat(jsonStatus,"\",\"IP Address\":\"");
    strcat(jsonStatus,WiFi.localIP().toString().c_str());
    
    strcat(jsonStatus,"\"}");
    response=jsonStatus;
    }
  else if (strcmp(charbuf,MQTT_PAYLOAD_STATUS_COMMAND)==0) //show the latest value
    {
    report();
    
    char tmp[25];
    strcpy(tmp,"Status report complete");
    response=tmp;
    }
  else if (strcmp(charbuf,MQTT_PAYLOAD_REBOOT_COMMAND)==0) //reboot the controller
    {
    char tmp[10];
    strcpy(tmp,"REBOOTING");
    response=tmp;
    rebootScheduled=true;
    }
  else if (processCommand(charbuf))
    {
    response="OK";
    }
  else
    {
    char badCmd[18];
    strcpy(badCmd,"(empty)");
    response=badCmd;
    }
    
  char topic[MQTT_TOPIC_SIZE];
  strcpy(topic,settings.mqttTopic);
  strcat(topic,charbuf); //the incoming command becomes the topic suffix

  if (!publish(topic,response,false)) //do not retain
    Serial.println("************ Failure when publishing status response!");
  
  if (rebootScheduled)
    {
    delay(2000); //give publish time to complete
    ESP.restart();
    }
  }


void showSettings()
  {
  Serial.print("ssid=<wifi ssid> (");
  Serial.print(settings.ssid);
  Serial.println(")");
  Serial.print("wifipass=<wifi password> (");
  Serial.print(settings.wifiPassword);
  Serial.println(")");
  Serial.print("broker=<MQTT broker host name or address> (");
  Serial.print(settings.mqttBrokerAddress);
  Serial.println(")");
  Serial.print("port=<port number>   (");
  Serial.print(settings.mqttBrokerPort);
  Serial.println(")");
  Serial.print("user=<mqtt user> (");
  Serial.print(settings.mqttUsername);
  Serial.println(")");
  Serial.print("pass=<mqtt password> (");
  Serial.print(settings.mqttPassword);
  Serial.println(")");
  Serial.print("mqttTopic=<topic root> (");
  Serial.print(settings.mqttTopic);
  Serial.println(")  Note: must end with \"/\"");  
  Serial.print("sleeptime=<seconds to sleep between measurements> (");
  Serial.print(settings.sleepTime);
  Serial.println(")");
  Serial.print("address=<Static IP address if so desired> (");
  Serial.print(settings.address);
  Serial.println(")");
  Serial.print("netmask=<Network mask to be used with static IP> (");
  Serial.print(settings.netmask);
  Serial.println(")");
  Serial.print("debug=1|0 (");
  Serial.print(settings.debug);
  Serial.println(")");
  Serial.print("MQTT Client ID is ");
  Serial.println(settings.mqttClientId);
  Serial.print("Device actual address is ");
  Serial.println(WiFi.localIP());
  Serial.print("CPU frequency is ");
  Serial.print(system_get_cpu_freq());
  Serial.println(" MHz.");
  Serial.println("\n*** Use NULL to reset a setting to its default value ***");
  Serial.println("*** Use \"factorydefaults=yes\" to reset all settings  ***");
  Serial.println("*** Use \"reset=yes\" to restart the processor  ***");
  Serial.println("*** Use a simple \"w\" to prevent sleep until restart  ***");
  
  Serial.print("\nSettings are ");
  Serial.println(settingsAreValid?"complete.":"incomplete.");
  }

/*
 * Reconnect to the MQTT broker
 */
boolean reconnect() 
  {
  // Loop until we're reconnected or times out
  uint8 tries=MQTT_RECONNECT_TRIES;
  while (!mqttClient.connected() && tries-- > 0) 
    {      
    Serial.print("Attempting MQTT connection...");

    mqttClient.setBufferSize(JSON_STATUS_SIZE); //default (256) isn't big enough
    mqttClient.setServer(settings.mqttBrokerAddress, settings.mqttBrokerPort);
    mqttClient.setCallback(incomingMqttHandler);
    
    // Attempt to connect
    if (mqttClient.connect(settings.mqttClientId,settings.mqttUsername,settings.mqttPassword))
      {
      Serial.println("connected to MQTT broker.");

      //resubscribe to the incoming message topic
      char topic[MQTT_TOPIC_SIZE];
      strcpy(topic,settings.mqttTopic);
      strcat(topic,MQTT_TOPIC_COMMAND_REQUEST);
      bool subgood=mqttClient.subscribe(topic);
      showSub(topic,subgood);
      }
    else 
      {
      Serial.print("failed, rc=");
      Serial.println(mqttClient.state());
      Serial.println("Will try again in a second");
      
      // Wait a second before retrying
      // In the meantime check for input in case something needs to be changed to make it work
      checkForCommand(); 
      
      delay(1000);
      }
    }
  mqttClient.loop(); //This has to happen every so often or we get disconnected for some reason
  return mqttClient.connected();
  }

void showSub(char* topic, bool subgood)
  {
  Serial.print("++++++Subscribing to ");
  Serial.print(topic);
  Serial.print(":");
  Serial.println(subgood);
  }

  
/*
 * Check for configuration input via the serial port.  Return a null string 
 * if no input is available or return the complete line otherwise.
 */
String getConfigCommand()
  {
  if (commandComplete) 
    {
    Serial.println(commandString);
    String newCommand=commandString;

    commandString = "";
    commandComplete = false;
    return newCommand;
    }
  else return "";
  }

bool processCommand(String cmd)
  {
  const char *str=cmd.c_str();
  char *val=NULL;
  char *nme=strtok((char *)str,"=");
  if (nme!=NULL)
    val=strtok(NULL,"=");

  char zero[]=""; //zero length string

  //Get rid of the carriage return and/or linefeed. Twice because could have both.
  if (val!=NULL && strlen(val)>0 && (val[strlen(val)-1]==13 || val[strlen(val)-1]==10))
    val[strlen(val)-1]=0; 
  if (val!=NULL && strlen(val)>0 && (val[strlen(val)-1]==13 || val[strlen(val)-1]==10))
    val[strlen(val)-1]=0; 

  //do it for the command as well.  Might not even have a value.
  if (nme!=NULL && strlen(nme)>0 && (nme[strlen(nme)-1]==13 || nme[strlen(nme)-1]==10))
    nme[strlen(nme)-1]=0; 
  if (nme!=NULL && strlen(nme)>0 && (nme[strlen(nme)-1]==13 || nme[strlen(nme)-1]==10))
    nme[strlen(nme)-1]=0; 

  if (settings.debug)
    {
    Serial.print("Processing command \"");
    Serial.print(nme);
    Serial.println("\"");
    Serial.print("Length:");
    Serial.println(strlen(nme));
    Serial.print("Hex:");
    Serial.println(nme[0],HEX);
    Serial.print("Value is \"");
    Serial.print(val);
    Serial.println("\"\n");
    }

  bool needRestart=true; //most changes will need a restart

  if (val==NULL)
    val=zero;

  if (nme==NULL || val==NULL || strlen(nme)==0) //empty string is a valid val value
    {
    showSettings();
    return false;   //not a valid command, or it's missing
    }
  else if (strcmp(val,"NULL")==0) //to nullify a value, you have to really mean it
    {
    strcpy(val,"");
    }
  
  if (strcmp(nme,"w")==0)
    {
    stayAwake=true;
    needRestart=false;
    Serial.println("Staying awake until next reset.");
    }
  else if (strcmp(nme,"broker")==0)
    {
    strcpy(settings.mqttBrokerAddress,val);
    saveSettings();
    }
  else if (strcmp(nme,"port")==0)
    {
    if (!val)
      strcpy(val,"0");
    settings.mqttBrokerPort=atoi(val);
    saveSettings();
    }
  else if (strcmp(nme,"mqttTopic")==0)
    {
    strcpy(settings.mqttTopic,val);
    saveSettings();
    }
  else if (strcmp(nme,"user")==0)
    {
    strcpy(settings.mqttUsername,val);
    saveSettings();
    }
  else if (strcmp(nme,"pass")==0)
    {
    strcpy(settings.mqttPassword,val);
    saveSettings();
    }
  else if (strcmp(nme,"ssid")==0)
    {
    strcpy(settings.ssid,val);
    saveSettings();
    }
  else if (strcmp(nme,"wifipass")==0)
    {
    strcpy(settings.wifiPassword,val);
    saveSettings();
    }
  else if (strcmp(nme,"address")==0)
    {
    strcpy(settings.address,val);
    saveSettings();
    }
  else if (strcmp(nme,"netmask")==0)
    {
    strcpy(settings.netmask,val);
    saveSettings();
    }
  else if (strcmp(nme,"sleepTime")==0)
    {
    if (!val)
      strcpy(val,"0");
    settings.sleepTime=atoi(val);
    saveSettings();
    needRestart=false;
    }
  else if (strcmp(nme,"debug")==0)
    {
    if (!val)
      strcpy(val,"0");
    settings.debug=atoi(val)==1?true:false;
    saveSettings();
    needRestart=false;
    }
  else if ((strcmp(nme,"resetmqttid")==0)&& (strcmp(val,"yes")==0))
    {
    generateMqttClientId(settings.mqttClientId);
    saveSettings();
    }
 else if ((strcmp(nme,"factorydefaults")==0) && (strcmp(val,"yes")==0)) //reset all eeprom settings
    {
    Serial.println("\n*********************** Resetting EEPROM Values ************************");
    initializeSettings();
    saveSettings();
    }
  else if ((strcmp(nme,"reset")==0) && (strcmp(val,"yes")==0)) //reset the device
    {
    Serial.println("\n*********************** Resetting Device ************************");
    }
  else
    {
    showSettings();
    return false; //command not found
    }

  if (needRestart)
    {
    Serial.println("Restarting processor.");
    delay(2000);
    ESP.restart();
    }
  return true;
  }

void initializeSettings()
  {
  settings.validConfig=0; 
  strcpy(settings.ssid,"");
  strcpy(settings.wifiPassword,"");
  strcpy(settings.mqttBrokerAddress,""); //default
  settings.mqttBrokerPort=1883;
  strcpy(settings.mqttUsername,"");
  strcpy(settings.mqttPassword,"");
  strcpy(settings.mqttTopic,"");
  strcpy(settings.address,"");
  strcpy(settings.netmask,"255.255.255.0");
  settings.sleepTime=10;
  generateMqttClientId(settings.mqttClientId);
  }


void checkForCommand()
  {
  serialEvent();
  String cmd=getConfigCommand();
  if (cmd.length()>0)
    {
    processCommand(cmd);
    }
  }

int readBattery()
  {
  int raw=ESP.getVcc(); //This commandeers the ADC port
  if (settings.debug)
    {
    Serial.print("Raw voltage count:");
    Serial.println(raw);
    }
  return raw;
  }

float convertToVoltage(int raw)
  {
  int vcc=map(raw,0,FULL_BATTERY,0,FULL_VOLTAGE);
  float f=((float)vcc)/100.0;
  return f;
  }


/************************
 * Do the MQTT thing
 ************************/
void report()
  {  
  char topic[MQTT_TOPIC_SIZE];
  char reading[18];
  boolean success=false;
  int analog=readBattery();

  Serial.print("Publishing from address ");
  Serial.println(WiFi.localIP());

  //publish the raw battery reading
  strcpy(topic,settings.mqttTopic);
  strcat(topic,MQTT_TOPIC_ANALOG);
  sprintf(reading,"%d",analog); 
  success=publish(topic,reading,true); //retain
  if (!success)
    Serial.println("************ Failed publishing raw battery reading!");

  //publish the battery voltage
  strcpy(topic,settings.mqttTopic);
  strcat(topic,MQTT_TOPIC_BATTERY);
  sprintf(reading,"%.2f",convertToVoltage(analog)); 
  success=publish(topic,reading,true); //retain
  if (!success)
    Serial.println("************ Failed publishing battery voltage!");

  if (stayAwake)
    Serial.println("Staying awake until next reset.");
  }

boolean publish(char* topic, const char* reading, boolean retain)
  {
  Serial.print(topic);
  Serial.print(" ");
  Serial.println(reading);
  return mqttClient.publish(topic,reading,retain); 
  }

  
/*
*  Initialize the settings from eeprom and determine if they are valid
*/
void loadSettings()
  {
  EEPROM.get(0,settings);
  if (settings.validConfig==VALID_SETTINGS_FLAG)    //skip loading stuff if it's never been written
    {
    settingsAreValid=true;
    if (settings.debug)
      {
      Serial.println("Loaded configuration values from EEPROM");
      }
    }
  else
    {
    Serial.println("Skipping load from EEPROM, device not configured.");    
    settingsAreValid=false;
    }
  }

/*
 * Save the settings to EEPROM. Set the valid flag if everything is filled in.
 */
boolean saveSettings()
  {
  if (strlen(settings.ssid)>0 &&
    strlen(settings.wifiPassword)>0 &&
    strlen(settings.mqttBrokerAddress)>0 &&
    settings.mqttBrokerPort!=0 &&
    strlen(settings.mqttTopic)>0 &&
    strlen(settings.mqttClientId)>0)
    {
    Serial.println("Settings deemed complete");
    settings.validConfig=VALID_SETTINGS_FLAG;
    settingsAreValid=true;
    }
  else
    {
    Serial.println("Settings still incomplete");
    settings.validConfig=0;
    settingsAreValid=false;
    }
    
  //The mqttClientId is not set by the user, but we need to make sure it's set  
  if (strlen(settings.mqttClientId)==0)
    {
    generateMqttClientId(settings.mqttClientId);
    }
    
  EEPROM.put(0,settings);
  return EEPROM.commit();
  }


//Generate an MQTT client ID.  This should not be necessary very often
char* generateMqttClientId(char* mqttId)
  {
  char mcir[]=MQTT_CLIENT_ID_ROOT;
  strcpy(mqttId,strcat(mcir,String(random(0xffff), HEX).c_str()));
  if (settings.debug)
    {
    Serial.print("New MQTT userid is ");
    Serial.println(mqttId);
    }
  return mqttId;
  }
  
/*
  SerialEvent occurs whenever a new data comes in the hardware serial RX. This
  routine is run between each time loop() runs, so using delay inside loop can
  delay response. Multiple bytes of data may be available.
*/
void serialEvent() 
  {
  while (Serial.available()) 
    {
    // get the new byte
    char inChar = (char)Serial.read();
    Serial.print(inChar); //echo it back to the terminal

    // if the incoming character is a newline, set a flag so the main loop can
    // do something about it 
    if (inChar == '\n') 
      {
      commandComplete = true;
      }
    else
      {
      // add it to the inputString 
      commandString += inChar;
      }
    }
  }
