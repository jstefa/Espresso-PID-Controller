/*
  Control an espresso machine boiler using a PID controller
  Code samples used from the following:
  PID Library: https://github.com/br3ttb/Arduino-PID-Library
  PID Lab: https://www.pdx.edu/nanogroup/sites/www.pdx.edu.nanogroup/files/2013_Arduino%20PID%20Lab_0.pdf
  Smoothing: https://www.arduino.cc/en/Tutorial/Smoothing
  Arduino <> Wemos D1 Mini pins: https://github.com/esp8266/Arduino/blob/master/variants/d1_mini/pins_arduino.h
  ESP8266 file uploading https://tttapa.github.io/ESP8266/Chap12%20-%20Uploading%20to%20Server.html
  Hardware:
  Wemos D1 Mini ( https://wiki.wemos.cc/products:d1:d1_mini )
  128 x 64 OLED Display using Adafruit_SSD1306 library
  AD8495 Thermocouple Amplifier ( https://www.adafruit.com/product/1778 )
  ADS1115 16-Bit ADC using library from https://github.com/baruch/ADS1115
  Solid State Relay ( Crydom TD1225 )
*/

// Set to 1 if using the Adafruit TC board or similar boards with 1.25v reference
#define ADA_TC 1

#if ADA_TC == 1
const int ADSGAIN = 2;
const double Vref = 1.2362;
#else
const int ADSGAIN = 16;
const int Vref = 0;
#endif

// set to true for testing the code on the breadboard setup. This will set the hostname to xespresso
const bool breadboard = false;

// set to 0 if not using the OLED display
#define OLED_DISPLAY 1

#include <PID_v1.h>

// Needed for the I2C ports
#include <Wire.h>

#if OLED_DISPLAY == 1
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSerifBold18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#endif

// Needed for ESP8266
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <FS.h>   //Include File System Headers

//needed for MQT
#include <PubSubClient.h>


// Needed for pushing new sketches over WiFi
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

// Needed for IotWebConf https://github.com/prampec/IotWebConf
#include <DNSServer.h>
#include <IotWebConf.h>

//Needed for MAX6675
#include <Thermocouple.h>
#include <MAX6675_Thermocouple.h>
#include <AverageThermocouple.h>



// *****************************************
// * Config options that you can customize *
// *****************************************

// MQT config
const char *ssid =  "D. Positive";  // Имя вайфай точки доступа
const char *pass =  "Electron78"; // Пароль от точки доступа

const char *mqtt_server = "hawk.rmq.cloudamqp.com"; // Имя сервера MQTT
const int mqtt_port = 1883; // Порт для подключения к серверу MQTT
const char *mqtt_user = "xzfqoiud:xzfqoiud"; // Логин от сервер
const char *mqtt_pass = "AL4vCjaT0bDCn0DwEnSsTQ5RlmQpIdoN"; // Пароль от сервера

#define BUFFER_SIZE 100

#define RelayPin 4 // Ardunio D4 = Wemos D1 Mini Pin D2

// After powering on, how many minutes until we force the boiler to power down
// Turning the machine off and on again will reset the timer
const int maxRunTime = 180;

// Turn the display off after 200 minutes
const int maxDisplayMins = 200;

// Default to being ON
bool operMode = true;

// Define the PID setpoint
double Setpoint;

// Define coffee temp
double coffeeTemp = 95;

// Define steam temp
double steamTemp = 145;

//name of the heating mode: "Coffee" or "Steam"
String heatingMode = "Coffee";

// Define the PID tuning Parameters
//double Kp = 3.5; working ok on 2018-09-14
double Kp = 4.5;
double Ki = 0.125;
double Kd = 0.2;

// PWM Window in milliseconds
const int WindowSize = 5000;


// ***********************************************************
// ***********************************************************
// * There should be no need to tweak many things below here *
// ***********************************************************
// ***********************************************************

// PID variables
// Using P_ON_M mode ( http://brettbeauregard.com/blog/2017/06/introducing-proportional-on-measurement/ )
double Input, Output;
PID myPID(&Input, &Output, &Setpoint, Kp, Ki, Kd, P_ON_M, DIRECT);
double PWMOutput;
uint32_t windowStartTime;

// Define the info needed for the temperature averaging
const int numReadings = 8;
int readings[numReadings];      // the readings from the analog input
int readIndex = 0;              // the index of the current reading
int total = 0;                  // the running total
int average = 0;                // the average

int wifiConnCounter = 0;
int mqttConnCounter = 0;


// All timers reference the value of now
uint32_t now = 0; //This variable is used to keep track of time

// OLED display timer
const int OLEDinterval = 250;           // interval at which to write new data to the OLED
uint32_t previousOLEDMillis = now;            // will store last time OLED was updated

// Serial output timer
const int serialPing = 500; //This determines how often we ping our loop
uint32_t lastMessage = now; //This keeps track of when our loop last spoke to serial

int runTimeMins;
long runTimeSecs;
uint32_t runTimeStart = now;

// Temp read interval
const int TempInterval = 5;
uint32_t currentTempMillis;
uint32_t previousTempMillis = now;

// Server tasks interval
const int serverInterval = 200;
uint32_t currentServerMillis;
uint32_t previousServerMillis = now;

// Setup I2C pins
#define ESP_SDA 14 // Arduino 14 = ESP8266 Pin 5
#define ESP_SCL 12 // Arduino 12 = ESP8266 Pin 6

//Setup MAX6675 pins
#define SCK_PIN 15 //D8
#define CS_PIN 13 //D7
#define SO_PIN 2 //D4

#define READINGS_NUMBER 10
#define DELAY_TIME 10

Thermocouple* thermocouple = NULL;

#if OLED_DISPLAY == 1

// OLED Display setup
#define OLED_RESET 16
#define OLED_I2C 0x3C
Adafruit_SSD1306 display(OLED_RESET);
#if (SSD1306_LCDHEIGHT != 64)
#error("Height incorrect, please fix Adafruit_SSD1306.h");
// You will need to modify the Adafruit_SSD1306.h file
// Step 1: uncomment this line: #define SSD1306_128_64
// Step 2: add a comment to this line: #define SSD1306_128_32
#endif
#endif

uint32_t prevLoopMillis;
uint32_t numLoops = 0;
uint32_t currLoops = 0;

WiFiClient wclient;      
PubSubClient client(wclient, mqtt_server, mqtt_port);
int tm=300;

byte key()
{  
    int val = analogRead(0); // считываем значение с аналогового входа и записываем в переменную val
        if (val < 50) return 1; // сверяем переменную, если val меньше 50 возвращаем 1 (первая кнопка)
        else if (val < 120) return 2; // если val меньше 150 вторая кнопка
        else return 0;  
}


void setTemp() 
{
    
  int a = key();
  int sensorValue = analogRead(A0);
   
    if (a == 1)
{
  Setpoint = steamTemp;
  heatingMode = "Steam";
}
else
{
  Setpoint = coffeeTemp;
  heatingMode = "Coffee";
}
}




void keepTime(void)
{
  //Keep track of time
  now = millis();
  runTimeSecs = (now - runTimeStart) / 1000;
  runTimeMins = (now - runTimeStart) / 60000;
}


void readTemps(void)
{
  const double max6675val = thermocouple->readCelsius();
  Input = max6675val;
}


void relayControl(void)
{
  // Calculate the number of running minutes

  // If more than maxRunTime minutes has elapsed, turn the boiler off
  // and dont perform any other PID functions
  if ( (runTimeMins >= maxRunTime) || (operMode == false) )
  {
    digitalWrite(RelayPin, LOW);
    myPID.SetMode(MANUAL);
    Output = 0;
    operMode = false;
  } else {
    // Compute the PID values
    myPID.SetMode(AUTOMATIC);
    myPID.Compute();
  }

  PWMOutput = Output * (WindowSize / 100.00);
  // Starts a new PWM cycle every WindowSize milliseconds
  if (now - windowStartTime > WindowSize)
  { //time to shift the Relay Window
    windowStartTime += WindowSize;
  }

  // Calculate the number of milliseconds that have passed in the current PWM cycle.
  // If that is less than the Output value, the relay is turned ON
  // If that is greater than (or equal to) the Output value, the relay is turned OFF.
  if (PWMOutput > (now - windowStartTime))
  {
    digitalWrite(RelayPin, HIGH);  // Wemos BUILTIN_LED LOW = ON
  } else {
    digitalWrite(RelayPin, LOW); // Wemos BUILTIN_LED HIGH = OFF
  }
}


// Track how many loops per second are executed.
void trackloop() {
  if ( now - prevLoopMillis >= 1000) {
    currLoops = numLoops;
    numLoops = 0;
    prevLoopMillis = now;
  }
  numLoops++;
}

#if OLED_DISPLAY == 1
void displayOLED(void)
{
  uint32_t currentOLEDMillis = now;

  if (currentOLEDMillis - previousOLEDMillis > OLEDinterval) {
    // save the last time you wrote to the OLED display
    previousOLEDMillis = currentOLEDMillis;

    // have to wipe the buffer before writing anything new
    display.clearDisplay();

    if ( operMode == true )
    {
      // TOP HALF = Temp + Input Temp
      display.setFont(&FreeSans9pt7b);
      display.setCursor(0, 22);
      display.print("Temp");

      display.setFont(&FreeSerifBold18pt7b);
      display.setCursor(48, 26);
      if ( Input >= 100 )
        display.print(Input, 1);
      else
        display.print(Input);

      // BOTTOM HALF = heatingMode + Setpoint
      display.setFont(&FreeSans9pt7b);
      display.setCursor(0, 56);
      display.print(heatingMode);

      display.setFont(&FreeSerifBold18pt7b);
      display.setCursor(60, 60);
      display.print(Setpoint);
    }
    // Do the needful!
    display.display();
  }
}
#endif

void displaySerial(void)
{
  // Output some data to serial to see what's happening
  if (now - lastMessage > serialPing)
  {
    if ( operMode == true )
    {
      Serial.print("Time: ");
      Serial.println(runTimeSecs);
    } else {
      Serial.print("Input: ");
      Serial.print(Input, 1);
      Serial.print(", ");
      Serial.print("Mode: off");
      Serial.print("\n");
    }
    lastMessage = now; //update the time stamp.
  }
}


//get data from server
void callback(const MQTT::Publish& pub)     // Функция получения данных от сервера
{
    Serial.print(pub.topic());                // выводим в сериал порт название топика
    Serial.print(" => ");
    Serial.println(pub.payload_string());     // выводим в сериал порт значение полученных данных
    
    String payload = pub.payload_string();
    
    if(String(pub.topic()) == "gaggia/coffeeTemp")    //  проверяем из нужного ли нам топика пришли данные 
    {
        double setCoffeeTempVal = payload.toFloat(); //  преобразуем полученные данные в тип Float
        if ( setCoffeeTempVal <= 105.11 || setCoffeeTempVal > 0.1 ) {
      coffeeTemp = setCoffeeTempVal;
      }
    
    }

    if(String(pub.topic()) == "gaggia/steamTemp")    //  проверяем из нужного ли нам топика пришли данные 
    {
        double setSteamTempVal = payload.toFloat(); //  преобразуем полученные данные в тип Float
        if ( setSteamTempVal <= 105.11 || setSteamTempVal > 0.1 ) {
      steamTemp = setSteamTempVal;
      }
    
    }

}

//send data to server
void dataSend()
{
    client.publish("gaggia/temp", String(Input)); // отправляем в топик для термодатчика значение температуры
    client.publish("gaggia/heatingMode", String(heatingMode));
    //client.publish("gaggia/coffeeTemp", String(coffeeTemp));
    //client.publish("gaggia/steamTemp", String(steamTemp));
    
        tm = 1000;  // пауза меду отправками значений температуры  коло 3 секунд  
}

void revercedataSend()
{
    client.publish("gaggia/coffeeTemp", String(coffeeTemp));
     tm = 1000;
     
    client.publish("gaggia/steamTemp", String(steamTemp));
    
        tm = 1000;  // пауза меду отправками значений температуры  коло 3 секунд  
}

void wifiConnect(){
  if (WiFi.status() != WL_CONNECTED) {
        Serial.print("Connecting to ");
        Serial.print(ssid);
        Serial.println("...");
        WiFi.begin(ssid, pass);
        wifiConnCounter++;
        //wifiConnCounter = wifiConnCounter + 1;
        
        if (WiFi.waitForConnectResult() != WL_CONNECTED)
            return;
        Serial.println("WiFi connected");   
    }
  }

void mqttConnect(){
  if (WiFi.status() == WL_CONNECTED) {
        if (!client.connected()) {
            Serial.println("Connecting to MQTT server");
            if (client.connect(MQTT::Connect("arduinoClient2")
                                 .set_auth(mqtt_user, mqtt_pass))) {
                Serial.println("Connected to MQTT server");
                client.set_callback(callback);
                client.subscribe("gaggia/coffeeTemp");
                client.subscribe("gaggia/steamTemp");
            } else {
                Serial.println("Could not connect to MQTT server");
                mqttConnCounter++;   
            }
        }
        
      if (client.connected()){
            client.loop();
            dataSend();
        }
    }
  }


void setup()
{
  Serial.begin(115200); //Start a serial session
  lastMessage = now; // timestamp

  //revercedataSend();
  
  // Set the Relay to output mode and ensure the relay is off
  pinMode(RelayPin, OUTPUT);
  digitalWrite(RelayPin, LOW);

  // PID settings
  windowStartTime = now;

  myPID.SetOutputLimits(0, 100);
  myPID.SetSampleTime(100);

  // initialize all the readings to 0:
  for (int thisReading = 0; thisReading < numReadings; thisReading++)
  {
    readings[thisReading] = 0;
  }

  // Enable I2C communication
  Wire.setClock(400000L); // ESP8266 Only
  Wire.begin(ESP_SDA, ESP_SCL);


#if OLED_DISPLAY == 1
  // Setup the OLED display
  display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C);  // initialize with the I2C addr 0x3C (for the 128x32)
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.display();
#endif

  //MAX6675 setup
  Thermocouple* originThermocouple = new MAX6675_Thermocouple(SCK_PIN, CS_PIN, SO_PIN);
  thermocouple = new AverageThermocouple(
    originThermocouple,
    READINGS_NUMBER,
    DELAY_TIME
  );

  SPIFFS.begin();


} // end of setup()


void loop()
{
  keepTime();
  readTemps();
  setTemp();
  relayControl();
  trackloop();
#if OLED_DISPLAY == 1
  displayOLED();
#endif
if (wifiConnCounter <= 2){
  wifiConnect();}

if (mqttConnCounter <= 2){
  mqttConnect();}

} // End of loop()
