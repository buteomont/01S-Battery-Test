// Pins
#define LED_BLUE 2

#define LED_ON LOW
#define LED_OFF HIGH

#define VALID_SETTINGS_FLAG 0xDAB0
#define SSID_SIZE 100
#define PASSWORD_SIZE 50
#define ADDRESS_SIZE 30
#define USERNAME_SIZE 50
#define MQTT_CLIENTID_SIZE 25
#define MQTT_TOPIC_SIZE 150
#define WIFI_ATTEMPTS 25
#define MQTT_TOPIC_BATTERY "battery"
#define MQTT_TOPIC_ANALOG "analog"
#define MQTT_TOPIC_RSSI "rssi"
#define MQTT_CLIENT_ID_ROOT "BatteryTest"
#define MQTT_TOPIC_COMMAND_REQUEST "command"
#define MQTT_PAYLOAD_SETTINGS_COMMAND "settings" //show all user accessable settings
#define MQTT_PAYLOAD_RESET_PULSE_COMMAND "resetPulseCounter" //reset the pulse counter to zero
#define MQTT_PAYLOAD_REBOOT_COMMAND "reboot" //reboot the controller
#define MQTT_PAYLOAD_VERSION_COMMAND "version" //show the version number
#define MQTT_PAYLOAD_STATUS_COMMAND "status" //show the most recent flow values
#define MQTT_RECONNECT_TRIES 3 // Give up if can't connect to broker in this many tries
#define JSON_STATUS_SIZE SSID_SIZE+PASSWORD_SIZE+USERNAME_SIZE+MQTT_TOPIC_SIZE+50 //+50 for associated field names, etc
#define PUBLISH_DELAY 400 //milliseconds to wait after publishing to MQTT to allow transaction to finish
//#define MAX_CHANGE_PCT 2 //percent distance change must be greater than this before reporting
#define FULL_BATTERY 3178 //raw A0 count with two alkaline batteries 
#define FULL_VOLTAGE 318  //Actual voltage when two fresh alkaline batteries are connected
#define ONE_HOUR 3600000 //milliseconds
#define SAMPLE_COUNT 5 //number of samples to take per measurement 

// Error codes copied from the MQTT library
// #define MQTT_CONNECTION_REFUSED            -2
// #define MQTT_CONNECTION_TIMEOUT            -1
// #define MQTT_SUCCESS                        0
// #define MQTT_UNACCEPTABLE_PROTOCOL_VERSION  1
// #define MQTT_IDENTIFIER_REJECTED            2
// #define MQTT_SERVER_UNAVAILABLE             3
// #define MQTT_BAD_USER_NAME_OR_PASSWORD      4
// #define MQTT_NOT_AUTHORIZED                 5

//WiFi status codes
//0 : WL_IDLE_STATUS when Wi-Fi is in process of changing between statuses
//1 : WL_NO_SSID_AVAILin case configured SSID cannot be reached
//3 : WL_CONNECTED after successful connection is established
//4 : WL_CONNECT_FAILED if password is incorrect
//6 : WL_DISCONNECTED if module is not configured in station mode

//prototypes

//PubSubClient callback function header.  This must appear before the PubSubClient constructor.
void incomingMqttHandler(char* reqTopic, byte* payload, unsigned int length);
unsigned long myMillis();
bool processCommand(String cmd);
void checkForCommand();
boolean connectToWiFi();
void incomingMqttHandler(char* reqTopic, byte* payload, unsigned int length); 
int measure();
void showSettings();
boolean reconnect(); 
void showSub(char* topic, bool subgood);
void initializeSettings();
int readBattery();
void report();
boolean publish(char* topic, const char* reading, bool retain);
void loadSettings();
boolean saveSettings();
void saveRTC();
void serialEvent(); 
boolean send();
char* generateMqttClientId(char* mqttId);
float convertToVoltage(int raw);
void setup(); 
void loop();