/*
 * WIP sketch for logging particle sensor data for ES6
 * 
 * Creates a file called "data.csv" on the SD card with columns for a time stamp and calculated particle concentration
 * 
 * Time stamp will be in seconds since the sensor was turned on
 * 
 * Code partially based on example here: https://www.mouser.com/datasheet/2/744/Seeed_101020012-1217636.pdf
 * 
 * will bulk update to ThingSpeak when button is pressed
 * 
 */

#include "ThingSpeak.h"
#include <WiFiNINA.h>
#include <SPI.h>
#include <SD.h>
#include <TinyGPS++.h>

#define SAMP_TIME 30000 // in milliseconds, 30s window
#define BLINK_TIME 300 // time in ms between LED blinks on successful write to SD
#define BLINK_CNT 3 // number of times to blink LED on successful write
#define SENSE_PIN 0
#define BUTTON_PIN 7
#define SD_CS_PIN 4

char ssid[] = "Landfall";
char password[] = "slosilo!";
WiFiClient client;

File dataFile;
String dataFileName = "datageo2.csv";

TinyGPSPlus gps;

const uint32_t channelNumber = 1186416;
const char writeApiKey[] = "IRCA839MQSQUAH59";

unsigned long prevSampMillis;
unsigned long prevLedMillis;
unsigned long curMillis;

float concentration;
float LPO;

volatile bool pulseStartFlag = false;
volatile bool pulseEndFlag = false;
volatile bool wifiFlag = false;
volatile bool buttonISREn = false;
bool pulseISREn = false;

unsigned long pulseStart; // start time of current pulse, in us
unsigned long pulseEnd; 
unsigned long pulseTime; // duration of latest low pulse, in us
unsigned long totalPulseTime = 0; // total low pulse occupancy of 30s window, in us

bool ledFlag = false;
uint8_t ledCount = 0;

void pulseISR() {
  if(pulseISREn)
  {
    if (digitalRead(SENSE_PIN)) // rising edge, store end of pulse time, flag that a pulse has ended
    {
      pulseEndFlag = true; // indicates the end of a pulse
    }
    else // falling edge, store start of pulse time
    {
      pulseStartFlag = true;
    }
  }
}

void buttonISR()
{
  if(buttonISREn = true)
  {
    wifiFlag = true;
    buttonISREn = false;
  }
}

void setup() {
  // initialize Serial port
  Serial.begin(115200);

  // intialize comms with GPS object
  Serial1.begin(9600);

  // Set sensor pin as input
  pinMode(SENSE_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT);
  pinMode(SD_CS_PIN, OUTPUT);

  Serial.println("Initialize SD");

  // Initialize SD card communication
  if(!SD.begin(SD_CS_PIN))
  {
    Serial.println("Card failed");
    while(true);
  }
  else
  {
    Serial.println("Card initialized successfully");
  }

  // Create column titles in CSV if creating it
  //    - column titles are time stamp, raw low pulse occupancy value, and calculated particle concentration in pcs/0.01cf
  // If CSV already exists, data will just be appended
  if(!SD.exists(dataFileName))
  {
    dataFile = SD.open(dataFileName, FILE_WRITE);
    if(dataFile)
    {
      dataFile.print("Timestamp,");
      dataFile.print("Low pulse occupancy,");
      dataFile.print("Particle concentration,");
      dataFile.print("Latitude,");
      dataFile.print("Longitude,");
      dataFile.print("Elevation,");
      dataFile.println("Status,");
      dataFile.close();  
    }
    else
    {
      Serial.println("Couldn't open file");
    }

  }

  // Set SENSE_PIN as an interrupt
  attachInterrupt(digitalPinToInterrupt(SENSE_PIN), pulseISR, CHANGE);

  // Button interrupt
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, RISING);

  pulseISREn = true;
  buttonISREn = true;
}

void loop() {
  curMillis = millis();

  if (pulseStartFlag)
  {
    pulseStart = micros();
    pulseStartFlag = false;
  }
  
  if (pulseEndFlag) // the end of a pulse has occurred
  { 
    pulseEnd = micros();
    if(pulseStart > pulseEnd)
    {
      pulseTime = 0;
      // addresses the rare occurrence of the start of a pulse happening just before
      // the ~70 minute rollover of us
    }
    else
    {
      pulseTime = pulseEnd - pulseStart;
    }
    totalPulseTime += pulseTime;
    pulseEndFlag = false;
//    Serial.print("pulseStart: ");
//    Serial.println(pulseStart);
//    Serial.print("pulseEnd: ");
//    Serial.println(pulseEnd);
//    Serial.print("pulseTime: ");
//    Serial.print(pulseTime);
//    Serial.println(" us");
//    Serial.print("totalPulseTime: ");
//    Serial.print(totalPulseTime);
//    Serial.println(" us\n");
  }

  // Blink LED upon successful SD write
  if(ledFlag)
  {
    if(curMillis - prevLedMillis >= BLINK_TIME)
    {
      blinkLed();
    }
  }

  if(curMillis - prevSampMillis >= SAMP_TIME)
  {
    updateSampleSD();
    Serial.println("Updated sample in SD card");
    Serial.println(micros());
    Serial.println();
  }

  if(wifiFlag)
  {
    updateThingSpeak();
    Serial.println("Updated to ThingSpeak");
    Serial.println();
    wifiFlag = false;
    buttonISREn = true;
  }

}

void updateThingSpeak()
{
  // TODO: write function here to update ThingSpeak with all the new values in CSV
  // Need to note the timestamp when this submission happens, then store that time stamp and only update data that is after that time to ThingSpeak
  
}

void updateSampleSD()
{
  pulseISREn = false;
  buttonISREn = false;

  prevSampMillis = curMillis;

  // for pulse time in micros
  LPO = (float)totalPulseTime/(SAMP_TIME*10);

  // from datasheet curve
  // TODO: update these calculations with calibration constants to be tuned later instead of hard-coded values
  // may also want to convert to float later, will see
  concentration = 1.1*pow(LPO, 3) - 3.8*pow(LPO, 2) + 520*LPO + 0.62; // in pcs/0.01cf

  // Reset total pulse duration time
  totalPulseTime = 0;

  readGps();

  // re-read GPS until data is valid
  while(!(gps.date.isValid() && gps.time.isValid() && gps.location.isValid()))
  {
    readGps();
  }

  // Display time stamp and concentration in the serial monitor
  // Format TIME: LPO%, CONCENTRATION pcs/0.01cf
  dataFile = SD.open(dataFileName, FILE_WRITE);
  if(dataFile)
  {
    // Display time stamp and concentration in the serial monitor
    // Format TIME: LPO%, CONCENTRATION pcs/0.01cf
    Serial.print(gps.date.month());
    Serial.print("/");
    Serial.print(gps.date.day());
    Serial.print("/");
    Serial.print(gps.date.year());
    Serial.print(" ");
    if(gps.time.hour() < 10) Serial.print("0");
    Serial.print(gps.time.hour());
    Serial.print(":");
    if(gps.time.minute() < 10) Serial.print("0");
    Serial.print(gps.time.minute());
    Serial.print(":");
    if(gps.time.second() < 10) Serial.print("0");
    Serial.print(gps.time.second());
    Serial.print(": ");
    Serial.print(LPO);
    Serial.print("%, ");
    Serial.print(concentration);
    Serial.print(" pcs/0.01cf, lat: ");
    Serial.print(gps.location.lat(), 2);
    Serial.print(", long: ");
    Serial.print(gps.location.lng(), 2);
    Serial.print(", alt: ");
    Serial.println(gps.altitude.meters(), 2);

    // Update data.csv with the same information
    
    // use ISO 8601 format for timestamp
    dataFile.print(gps.date.year());
    dataFile.print("-");
    dataFile.print(gps.date.month());
    dataFile.print("-");
    dataFile.print(gps.date.day());
    dataFile.print("T");
    if(gps.time.hour() < 10) dataFile.print("0");
    dataFile.print(gps.time.hour());
    dataFile.print(":");
    if(gps.time.minute() < 10) dataFile.print("0");
    dataFile.print(gps.time.minute());
    dataFile.print(":");
    if(gps.time.second() < 10) dataFile.print("0");
    dataFile.print(gps.time.second());
    dataFile.print("+00:00,");
    
    dataFile.print(LPO);
    dataFile.print(",");
    dataFile.print(concentration);
    dataFile.print(",");
    dataFile.print(gps.location.lat(), 1);
    dataFile.print(",");
    dataFile.print(gps.location.lng(), 1);
    dataFile.print(",");
    dataFile.print(int(gps.altitude.meters()));
    dataFile.print(",");
    dataFile.print("good");
    dataFile.println(",");
    dataFile.close();

    ledFlag = true;
  }
  else
  {
    Serial.println("Couldn't open file");
  }

  pulseStartFlag = false;
  pulseEndFlag = false;

  delay(1000);
  pulseISREn = true;
  buttonISREn = true;
}

void blinkLed()
{
  prevLedMillis = curMillis;
  
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  
  if(++ledCount >= BLINK_CNT*2)
  {
    ledFlag = false;
    ledCount = 0;
  }
}

void connectWiFi()
{
    // Initialize WiFi 
  WiFi.begin(ssid, password);
  
  while(WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
  delay(500);
}

void readGps()
{
  while(Serial1.available() > 0)
  {
    if(gps.encode(Serial1.read()))
    {
      Serial.println("GPS data successfully encoded");
    }
  }
}

void sleepGps()
{

}

void wakeGps()
{
  
}

byte createChecksum(char* cmd)
{
  byte checksum = 0;

  for(int i = 0; i < strlen(cmd); i++)
  {
    checksum = checksum ^ cmd[i];
  }

  return checksum;
}
