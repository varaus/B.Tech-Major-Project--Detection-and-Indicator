#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>
#include <FirebaseESP8266.h>

/* Defining all the GPIO pins. */
#define RESET_BUTTON_INPUT D0
#define IR_SENSOR_INPUT D1
#define WATER_FLOW_SENSOR_INPUT D2
#define RELAY_OUTPUT D3
#define NETWORK_STATUS_LED D4
#define LOOP_COMPLETION_STATUS_LED D5
#define RESET_BUTTON_STATUS_LED D6
#define IR_SENSOR_STATUS_LED D7
#define WATER_FLOW_SENSOR_STATUS_LED D8
#define RELAY_STATUS_LED D9

/* Defining all the variables. */
boolean resetButtonStatus;
boolean irSensorStatus;
boolean relayStatus;
boolean objectStatus;
boolean valveStatus;
boolean closeTheValve;
volatile byte sensorInterrupt;
volatile byte pulseCount;
float calibrationFactor;
float flowRate;
unsigned long address1;
unsigned long address2;
unsigned long address3;
unsigned long fraction;
unsigned long oldTime;
unsigned long flowMilliLitres;
unsigned long totalMilliLitres;
unsigned long currentFlowRateIntPart;
unsigned long currentFlowRateFracPart;
unsigned long lastGasOutputQuantity;
unsigned long currentGasOutputQuantity;
unsigned long oneLitreCrossed;
unsigned long twoLitreCrossed;

struct
{
  unsigned long volumeInMilliLitres = 0;
} volumeData;

boolean firstUsageWastageSend;
boolean secondUsageWastageSend;
boolean objectPresentStatusSend;
boolean objectAbsentStatusSend;
boolean valveOpenStatusSend;
boolean valveCloseStatusSend;

/* Interrupt service routine. */
void ICACHE_RAM_ATTR pulseCounter()
{
  pulseCount++;
}

/* Defining SSID & PASSWORD for the AP. */
boolean wifi_soft_ap_on = false;
boolean wifi_station_on = false;
const char *ap_ssid = "ESP8266-0001";
const char *ap_pass = "password";

/* Defining SSID & PASSWORD for the Station Mode. */
String ssid = "S.TS..2.4GHz";
String pass = "Bongaon:743235";

/* Defining firebase credentials */
#define FIREBASE_HOST "major-917e8.firebaseio.com"
#define FIREBASE_AUTH "kHPQpyR5ZBLRLcL4dzZh5lraHggxMEGPmhxpHZSZ"

/* Defining FirebaseESP8266 data object */
FirebaseData firebaseData;

/* Defining network details. */
IPAddress local_ip(192, 168, 1, 1);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

/* Defining the server. */
ESP8266WebServer server(80);

/* Function to setup network as access point mode. */
void setUpWiFiAsAccessPoint()
{
  WiFi.begin(ssid, pass);

  while (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(NETWORK_STATUS_LED, HIGH);
    Serial.println(".");
    delay(250);
    digitalWrite(NETWORK_STATUS_LED, LOW);
    delay(250);
  }

  digitalWrite(NETWORK_STATUS_LED, HIGH);
  Serial.println("");
  Serial.println("Connected to the WiFi...");
  Serial.print("localIP: ");
  Serial.println(WiFi.localIP());
  Serial.println("");
}

/* Function for connecting to the firebase */
void connectToFirebase()
{
  Firebase.begin(FIREBASE_HOST, FIREBASE_AUTH);
  Firebase.reconnectWiFi(true);

  //Setting the size of WiFi rx/tx buffers in the case where we want to work with large data.
  firebaseData.setBSSLBufferSize(1024, 1024);

  //Setting the size of HTTP response buffers in the case where we want to work with large data.
  firebaseData.setResponseSize(1024);

  //Setting database read timeout to 1 minute (max 15 minutes)
  Firebase.setReadTimeout(firebaseData, 1000 * 60);

  Serial.println("");
  Serial.println("Connected to Firebase Realtime Database...");
  Serial.println("");
}

/* Function to call after every 1litre of gas usage or wastage. */
void updateUsageOrWastageVolume()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    String configuredVolume;
    String currentVolume;
    String updatedVolume;

    if (Firebase.getString(firebaseData, "/Devices/device0001/gasUsageWastageVolume"))
    {
      currentVolume = firebaseData.stringData();
    }

    if (Firebase.getString(firebaseData, "/Devices/device0001/configurations/gasVolume"))
    {
      configuredVolume = firebaseData.stringData();
    }

    int configured = configuredVolume.toInt();
    int current = currentVolume.toInt();

    if (current < configured)
    {
      current++;
    }
    else if (current >= configured)
    {
      current = current;
    }

    Serial.println(current);
    Firebase.setString(firebaseData, "/Devices/device0001/gasUsageWastageVolume", String(current));
  }
  else
  {
    Serial.println("Not connected to WiFi...");
  }
}

/* Function for updating the IRSensor status in the database. */
void updateIRSensorStaus(String state)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    if (state == "0")
    {
      // Object present
      Firebase.setBool(firebaseData, "/Devices/device0001/objectIsPresent", true);
    }
    else if (state == "1")
    {
      // Object not present
      Firebase.setBool(firebaseData, "/Devices/device0001/objectIsPresent", false);
    }
  }
  else
  {
    Serial.println("Not connected to WiFi...");
  }
}

/* Function for updating the valve status in the database. */
void updateValveStaus(String state)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    if (state == "0")
    {
      // Valve Closed
      Firebase.setBool(firebaseData, "/Devices/device0001/valveIsOpen", false);
    }
    else if (state == "1")
    {
      // Valve Closed
      Firebase.setBool(firebaseData, "/Devices/device0001/valveIsOpen", true);
    }
  }
  else
  {
    Serial.println("Not connected to WiFi...");
  }
}

/* Function for resetting the usageWastageVolume in the database when the reset button is pressed */
void resetUsageOrWastageVolume()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    Firebase.setString(firebaseData, "/Devices/device0001/gasUsageWastageVolume", "0");
  }
  else
  {
    Serial.println("Not connected to WiFi...");
  }
}

/* Function to turn off the valve when the closeValve instruction is received from the client side */
void closeValve()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    if (relayStatus == true)
    {
      if (Firebase.getBool(firebaseData, "/Devices/device0001/valveIsOpen"))
      {
        boolean valveState = firebaseData.boolData();

        if (valveState == false)
        {
          digitalWrite(RELAY_OUTPUT, LOW);
          digitalWrite(RELAY_STATUS_LED, LOW);
          relayStatus = false;
        }
      }
    }
  }
  else
  {
    Serial.println("Not connected to WiFi...");
  }
}

/* Function to check whether the irSensor status & valve status is send */
void checkStatusSend()
{
  if (objectPresentStatusSend == false && valveOpenStatusSend == false)
  {
    updateIRSensorStaus("0");
    updateValveStaus("1");
    objectPresentStatusSend = true;
    valveOpenStatusSend = true;
    objectAbsentStatusSend = false;
    valveCloseStatusSend = false;
  }
  else if (objectPresentStatusSend == false && valveOpenStatusSend == true)
  {
    updateIRSensorStaus("0");
    objectPresentStatusSend = true;
    valveOpenStatusSend = true;
    objectAbsentStatusSend = false;
    valveCloseStatusSend = false;
  }
  else if (objectPresentStatusSend == true && !valveOpenStatusSend == false)
  {
    updateValveStaus("1");
    objectPresentStatusSend = true;
    valveOpenStatusSend = true;
    objectAbsentStatusSend = false;
    valveCloseStatusSend = false;
  }
  else if (objectPresentStatusSend == true && valveOpenStatusSend == true)
  {
    objectPresentStatusSend = true;
    valveOpenStatusSend = true;
    objectAbsentStatusSend = false;
    valveCloseStatusSend = false;
  }
}

/* Setup code, to run once. */
void setup()
{
  // Initializing the serial monitor
  Serial.begin(9600);
  delay(250);

  // Initializing EEPROM
  EEPROM.begin(512);

  // Initializing all the GPIO pins
  pinMode(RESET_BUTTON_INPUT, INPUT);
  pinMode(IR_SENSOR_INPUT, INPUT);
  pinMode(WATER_FLOW_SENSOR_INPUT, INPUT);
  pinMode(RELAY_OUTPUT, OUTPUT);
  pinMode(NETWORK_STATUS_LED, OUTPUT);
  pinMode(LOOP_COMPLETION_STATUS_LED, OUTPUT);
  pinMode(RESET_BUTTON_STATUS_LED, OUTPUT);
  pinMode(IR_SENSOR_STATUS_LED, OUTPUT);
  pinMode(WATER_FLOW_SENSOR_STATUS_LED, OUTPUT);
  pinMode(RELAY_STATUS_LED, OUTPUT);

  // Initializing all the variables
  sensorInterrupt = 0;
  pulseCount = 0;
  calibrationFactor = 4.5;
  flowRate = 0.0;
  address1 = 0;
  address2 = 10;
  address3 = 15;
  oldTime = 0;
  flowMilliLitres = 0;
  totalMilliLitres = 0;
  relayStatus = false;
  firstUsageWastageSend = false;
  secondUsageWastageSend = false;
  objectPresentStatusSend = false;
  objectAbsentStatusSend = false;
  valveOpenStatusSend = false;
  valveCloseStatusSend = false;

  // Making oneLitreCrossed value false in the EEPROM if it's previous value is anything other than true or false.
  // [1 = false] and [0 = true]
  if ((EEPROM.read(address2) != 1) && (EEPROM.read(address2) != 0))
  {
    EEPROM.write(address2, 1);
  }

  // Making twoLitreCrossed value false in the EEPROM if it's previous value is anything other than true or false.
  // [1 = false] and [0 = true]
  if ((EEPROM.read(address3) != 1) && (EEPROM.read(address3) != 0))
  {
    EEPROM.write(address3, 1);
  }

  // Initializing the network connection
  setUpWiFiAsAccessPoint();

  // Initializing the firebase connection
  connectToFirebase();

  // Making object status & valve status to false after first boot up (like after a power cut)
  updateIRSensorStaus("1");
  updateValveStaus("0");

  // Attaching the interrupt routine
  digitalWrite(WATER_FLOW_SENSOR_INPUT, HIGH);
  attachInterrupt(digitalPinToInterrupt(WATER_FLOW_SENSOR_INPUT), pulseCounter, RISING);
}

/* Main code, to run repeatedly. */
void loop()
{
  // Main logic
  if ((millis() - oldTime) > 1000)
  {
    digitalWrite(LOOP_COMPLETION_STATUS_LED, HIGH);

    detachInterrupt(sensorInterrupt);
    EEPROM.get(address1, volumeData);
    oneLitreCrossed = EEPROM.read(address2);
    twoLitreCrossed = EEPROM.read(address3);

    flowRate = (((1000.00 / (millis() - oldTime)) * pulseCount) / calibrationFactor);
    oldTime = millis();

    flowMilliLitres = ((flowRate / 60) * 1000);
    totalMilliLitres += flowMilliLitres;
    fraction = ((flowRate - int(flowRate)) * 10);

    volumeData.volumeInMilliLitres += flowMilliLitres;

    // Water flow status output
    if (flowMilliLitres > 0)
    {
      digitalWrite(WATER_FLOW_SENSOR_STATUS_LED, HIGH);
    }
    else
    {
      digitalWrite(WATER_FLOW_SENSOR_STATUS_LED, LOW);
    }

    // Reading the status of the resetButton & irSensor
    resetButtonStatus = digitalRead(RESET_BUTTON_INPUT);
    irSensorStatus = digitalRead(IR_SENSOR_INPUT);

    // Resetting the EEPROM on resetButton press
    if (resetButtonStatus == HIGH)
    {
      digitalWrite(RESET_BUTTON_STATUS_LED, HIGH);
      volumeData.volumeInMilliLitres = 0;
      oneLitreCrossed = 1;
      twoLitreCrossed = 1;
      resetUsageOrWastageVolume();
      Serial.println("");
      Serial.println("Reset button was pressed & the EEPROM has been reset.");
      delay(500);
      digitalWrite(RESET_BUTTON_STATUS_LED, LOW);
    }

    // Handling object presence
    if (irSensorStatus == LOW)
    {
      // Object is present
      digitalWrite(IR_SENSOR_STATUS_LED, HIGH);

      if (relayStatus == false)
      {
        digitalWrite(RELAY_OUTPUT, HIGH);
        digitalWrite(RELAY_STATUS_LED, HIGH);
        relayStatus = true;
      }

      checkStatusSend();

      Serial.println("Object detected & valve is open.");

      if ((volumeData.volumeInMilliLitres >= 1000) && (volumeData.volumeInMilliLitres < 2000))
      {
        digitalWrite(RESET_BUTTON_STATUS_LED, HIGH);

        if (firstUsageWastageSend != true)
        {
          if (oneLitreCrossed == 1)
          {
            updateUsageOrWastageVolume();
            oneLitreCrossed = 0;
            twoLitreCrossed = 1;
          }
          else if (oneLitreCrossed == 0)
          {
            oneLitreCrossed = 0;
            twoLitreCrossed = 1;
          }

          firstUsageWastageSend = true;
          secondUsageWastageSend = false;
        }
      }

      if ((volumeData.volumeInMilliLitres >= 2000))
      {
        digitalWrite(RESET_BUTTON_STATUS_LED, LOW);
        volumeData.volumeInMilliLitres = 0;

        if (secondUsageWastageSend != true)
        {
          if (twoLitreCrossed == 1)
          {
            updateUsageOrWastageVolume();
            oneLitreCrossed = 1;
            twoLitreCrossed = 0;
          }
          else if (twoLitreCrossed == 0)
          {
            oneLitreCrossed = 1;
            twoLitreCrossed = 0;
          }

          firstUsageWastageSend = false;
          secondUsageWastageSend = true;
        }
      }
    }
    else if (irSensorStatus == HIGH)
    {
      // No object is present
      digitalWrite(IR_SENSOR_STATUS_LED, LOW);

      if (!objectAbsentStatusSend)
      {
        updateIRSensorStaus("1");
        objectPresentStatusSend = false;
        objectAbsentStatusSend = true;
      }
      else
      {
        objectPresentStatusSend = false;
        objectAbsentStatusSend = true;
      }

      Serial.println("No object is detected.");

      // Closing the valve if valve state if false in the database
      closeValve();

      if ((volumeData.volumeInMilliLitres >= 1000) && (volumeData.volumeInMilliLitres < 2000))
      {
        digitalWrite(RESET_BUTTON_STATUS_LED, HIGH);

        if (firstUsageWastageSend != true)
        {
          if (oneLitreCrossed == 1)
          {
            updateUsageOrWastageVolume();
            oneLitreCrossed = 0;
            twoLitreCrossed = 1;
          }
          else if (oneLitreCrossed == 0)
          {
            oneLitreCrossed = 0;
            twoLitreCrossed = 1;
          }

          firstUsageWastageSend = true;
          secondUsageWastageSend = false;
        }
      }

      if ((volumeData.volumeInMilliLitres >= 2000))
      {
        if (relayStatus == true)
        {
          digitalWrite(RELAY_OUTPUT, LOW);
          digitalWrite(RELAY_STATUS_LED, LOW);
          relayStatus = false;
        }

        digitalWrite(RESET_BUTTON_STATUS_LED, LOW);
        volumeData.volumeInMilliLitres = 0;

        if (secondUsageWastageSend != true)
        {
          if (twoLitreCrossed == 1)
          {
            updateUsageOrWastageVolume();
            oneLitreCrossed = 1;
            twoLitreCrossed = 0;
          }
          else if (twoLitreCrossed == 0)
          {
            oneLitreCrossed = 1;
            twoLitreCrossed = 0;
          }

          firstUsageWastageSend = false;
          secondUsageWastageSend = true;
        }

        // Checking whether the valveOpen status is already updated in the database, if not then updating it & if yes then skipping it
        if (!valveCloseStatusSend)
        {
          updateValveStaus("0");
          valveOpenStatusSend = false;
          valveCloseStatusSend = true;
        }
        else
        {
          valveOpenStatusSend = false;
          valveCloseStatusSend = true;
        }
      }
    }

    // Commiting data to EEPROM
    EEPROM.write(address2, oneLitreCrossed);
    EEPROM.write(address3, twoLitreCrossed);
    EEPROM.put(address1, volumeData);
    EEPROM.commit();
    pulseCount = 0;

    digitalWrite(LOOP_COMPLETION_STATUS_LED, LOW);
  }
}