/*
 * WIP sketch for logging particle sensor data for ES6
 * 
 * Creates a file called "data.csv" on the SD card with columns for a time stamp and calculated particle concentration
 * 
 * Time stamp fwill be in seconds since the sensor was turned on
 * 
 * Code partially based on example here: https://www.mouser.com/datasheet/2/744/Seeed_101020012-1217636.pdf
 * 
 * will bulk update to ThingSpeak when button is pressed
 * 
 * NEW VERSION FOR LASER, I2C SENSOR
 * 
 */

#include "ThingSpeak.h"
#include <U8g2lib.h>
#include <WiFi101.h>
#include <SPI.h>
#include <SD.h>
#include <TinyGPS++.h>
#include <Wire.h>
#include <TimeLib.h>

#define SAMP_TIME 5000 // in milliseconds, sensor updates every 1 second, read it every 5
#define WIFI_TIME 30000 // wifi updates every 30 seconds
#define BLINK_TIME 300 // time in ms between LED blinks on successful write to SD
#define BLINK_CNT 3 // number of times to blink LED on successful write
#define SENSE_PIN 0
#define BUTTON_PIN 7
#define SD_CS_PIN 4
#define SENSOR_ADDR 0x40

char ssid[] = "Landfall";
char password[] = "slosilo!";
WiFiClient client;

File dataFile;
char dataFileName[] = "data.csv";

TinyGPSPlus gps;
time_t prevTimeStamp;
bool staleFlag = false;

U8G2_SSD1306_128X64_ALT0_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const uint32_t channelNumber = 1186416;
const char writeApiKey[] = "IRCA839MQSQUAH59";
const char server[] = "api.thingspeak.com";

unsigned long prevSampMillis;
unsigned long prevLedMillis;
unsigned long prevWiFiMillis;
unsigned long curMillis;

volatile bool wifiFlag = false;
volatile bool buttonISREn = false;

bool ledFlag = false;
uint8_t ledCount = 0;

// particleData[0]  = PM1.0, standard
// particleData[1]  = PM2.5, standard
// particleData[2]  = PM10.0, standard
// particleData[3]  = PM1.0, atmo
// particleData[4]  = PM2.5, atmo
// particleData[5]  = PM10.0, atmo
// particleData[6]  = >0.3um
// particleData[7]  = >0.5um
// particleData[8]  = >1.0um
// particleData[9]  = >2.5um
// particleData[10] = >5.0um
// particleData[11] = >10.0um
uint16_t particleData[12];

uint8_t i2c_buf[30]; // data buffer for I2C comms

bool firstLineDone = false; // flag for first line (titles) having been read
uint32_t lastLineRead = 0; // latest line that SD card was updated from
char sd_buf[200]; // buffer to store single SD card line
char tspeak_buf[2000]; // There appears to be a maximum of 1400 bytes that can be sent a time, either to ThingSpeak or with WiFi library
char status_buf[20]; // buffer for status text
uint8_t colPositions[17] = {0}; // array to store indices of commas in sd_buf, indicating column delineations

void setup() {
  // initialize Serial port
  Serial.begin(115200);

  // intialize comms with GPS object
  Serial1.begin(9600);

  Wire.begin();

  u8g2.begin();

  display("Initializing...", 20, true, true);
  delay(2500);

  // Set sensor pin as input
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT);
  pinMode(SD_CS_PIN, OUTPUT);

  Serial.println("Initialize SD");
  display("Checking SD", 20, true, true);
  delay(2500);

  // Initialize SD card communication
  if(!SD.begin(SD_CS_PIN))
  {
    Serial.println("Card failed");
    display("SD card failed", 16, true, false);
    display("Reset device", 24, false, true);
    while(true);
  }
  else
  {
    Serial.println("Card initialized successfully");
    display("SD card detected", 20, true, true);
  }
  delay(2500);

  char displayBuffer[25];
  // Create column titles in CSV if creating it
  // If CSV already exists, data will just be appended
  if(!SD.exists(dataFileName))
  {
    strcpy(displayBuffer, "Creating ");
    strcat(displayBuffer, dataFileName);
    display(displayBuffer, 20, true, true);
    delay(2500);
    dataFile = SD.open(dataFileName, FILE_WRITE);
    if(dataFile)
    {
      dataFile.print("Timestamp,");
      dataFile.print("Particle concentration (PM1.0 standard) (ug/m^3),");
      dataFile.print("Particle concentration (PM2.5 standard) (ug/m^3),");
      dataFile.print("Particle concentration (PM10.0 standard) (ug/m^3),");
      dataFile.print("Particle concentration (PM1.0 atmospheric) (ug/m^3),");
      dataFile.print("Particle concentration (PM2.5 atmospheric) (ug/m^3),");
      dataFile.print("Particle concentration (PM10.0 atmospheric) (ug/m^3),");
      dataFile.print("Particle concentration (>=0.3um) (pcs/L),");
      dataFile.print("Particle concentration (>=0.5um) (pcs/L),");
      dataFile.print("Particle concentration (>=1.0um) (pcs/L),");
      dataFile.print("Particle concentration (>=2.5um) (pcs/L),");
      dataFile.print("Particle concentration (>=5.0um) (pcs/L),");
      dataFile.print("Particle concentration (>=10.0um) (pcs/L),");
      dataFile.print("Latitude,");
      dataFile.print("Longitude,");
      dataFile.print("Elevation,");
      dataFile.println("Status,");
      dataFile.close();  
    }
    else
    {
      Serial.println("Couldn't open file");
      display("Unable to open file", 16, true, false);
      display("Check SD, reset device", 24, false, true);
    }

  }

  if(!initDustSensor())
  {
    Serial.println("Failed to initialize dust sensor");
    display("Dust sensor init failed", 16, true, false);
    display("Reset device", 24, false, true);
    while(true);
  }

  // Button interrupt
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, RISING);

  sleepGps();

  buttonISREn = true;
}

void loop() {
  curMillis = millis();

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
    Serial.println();
  }

  // if(curMillis - prevWiFiMillis >= WIFI_TIME)
  if(wifiFlag)
  {
    updateThingSpeak();
    Serial.println("Updated to ThingSpeak");
    Serial.println();
    wifiFlag = false;
    buttonISREn = true;
  }

}

// ISR for button being pressed
void buttonISR()
{
  if(buttonISREn == true)
  {
    wifiFlag = true;
    buttonISREn = false;
  }
}

// sends initial command to dust sensor 
bool initDustSensor()
{
  bool initSuccess;
  Wire.beginTransmission(SENSOR_ADDR);
  Wire.write(0x88); // select command
  initSuccess = Wire.endTransmission();
  // endTransmission() returns 0 on a success

  return !initSuccess;
}

// return false on a timeout of 10ms
bool readDustSensor(uint8_t *data, uint32_t data_len)
{
  uint32_t timeOutCnt = 0;
  Wire.requestFrom(SENSOR_ADDR, 29);
  while(data_len != Wire.available())
  {
    timeOutCnt++;
    if(timeOutCnt > 10) return false;
    delay(1);
  }
  for(int i = 0; i < data_len; i++)
  {
    data[i] = Wire.read();
  }
  return true;
}

// moves raw data read from I2C bus into decoded buffer (particleData in this sketch)
// returns false if checksum is invalid (and doesn't parse data, i.e. data_out will not change)
bool parseSensorData(uint16_t *data_out, uint8_t *data_raw)
{
  int j = 0;
  byte sum = 0;

  for(int i = 0; i < 28; i++)
  {
    sum += data_raw[i];
  }

  // wrong checksum
  if(sum != data_raw[28])
  {
    return false;
  }
  
  for(int i = 4; i <=26 ; i += 2)
  {
    data_out[j] = (data_raw[i] << 8) | (data_raw[i+1]);
    j++;
  }

  return true;
}

// updates to ThingSpeak in bulk updates of 2kB of data
void updateThingSpeak()
{
  // connectWiFi();

  memset(tspeak_buf, 0, sizeof(tspeak_buf));
  strcpy(tspeak_buf, "write_api_key=");
  strcat(tspeak_buf, writeApiKey);
  strcat(tspeak_buf, "&time_format=absolute&updates=");

  // Open file on SD card
  display("Updating to", 16, true, false);
  display("ThingSpeak...", 24, false, true);
  dataFile = SD.open(dataFileName, FILE_READ);
  if(dataFile)
  {
    uint32_t lineCount = 0;
    // keep track of total number of characters read, use to cut off at 2000 bytes
    // starts at 60 because of initial body parameters
    uint32_t charCount = 60;
    uint32_t lineCharCount = 0;
    uint32_t colCount = 0; 
    uint32_t i = 0;
    uint32_t linePosition = 0; // location in file of most recent line

    Serial.print("charCount: ");
    Serial.println(charCount);
    
    while(dataFile.available())
    {
      char c = dataFile.read();

      // Every time you fill up tspeak_buf, send an update to thing speak
      if((charCount >= sizeof(tspeak_buf) - 100) && (c != '\n'))
      {
        Serial.println("Buffer full!");
        Serial.println(charCount);
        tspeak_buf[strlen(tspeak_buf) - 1] = 0; // remove last pipe character
        httpRequest(tspeak_buf);

        // reset byte counter and thingspeak buffer
        i = 0;
        colCount = 0;
        charCount = 60;
        memset(tspeak_buf, 0, sizeof(tspeak_buf));
        strcpy(tspeak_buf, "write_api_key=");
        strcat(tspeak_buf, writeApiKey);
        strcat(tspeak_buf, "&");
        strcat(tspeak_buf, "time_format=absolute&updates=");

        // clear SD line buffer and move back to start of latest line
        memset(sd_buf, 0, sizeof(sd_buf));
        dataFile.seek(linePosition);

        delay(20000); // can only update to Thingspeak every 15s
        Serial.println("Back to updating");
      }
      else if(c == '\n')
      {
        if(!firstLineDone)
        {
          firstLineDone = true;
        }
        // on a new line, process data on line
        i = 0;
        colCount = 0;

        // store position in file of the start of next line
        linePosition = dataFile.position();

        // first loop to line after the last from last update
        if(lineCount++ <= lastLineRead)
        {
          // clear line buffer
          memset(sd_buf, 0, sizeof(sd_buf));

          // reset charCounter (only want to count data we're actually sending)
          charCount = 60;
          continue;
        }

        // Serial.print("current row: ");
        // Serial.println(lineCount);

        // only update line if GPS data isn't stale
        memset(status_buf, 0, sizeof(status_buf));
        int j = 0;
        for(int k = colPositions[16]; k < sizeof(sd_buf); k++)
        {
          if(sd_buf[k] == 0)
          {
            break;
          }
          status_buf[j++] = sd_buf[k];
        }

        // if GPS time stamp isn't stale, include in tspeak_buf (data to be sent to thingspeak)
        if(status_buf[0] == 'g')
        {
          Serial.println("Updating thingspeak buffer");
          // SD data is already formatted for ThingSpeak updates, just concatenate each line with pipe character in between
          
          // Remove columns for standard PM data and 0.5um particles
          uint8_t pmStIndex = colPositions[1]; // index of first standard PM column
          uint8_t pmAtIndex = colPositions[4]; // index of first atmo PM column
          uint8_t zerop5umIndex = colPositions[8]; // index of 0.5um particles
          uint8_t oneumIndex = colPositions[9]; // index of 1um particles
          uint8_t pmColsDel = pmAtIndex - pmStIndex; // PM columns to delete
          uint8_t cntsColDel = oneumIndex - zerop5umIndex; // Particle count columns to be deleted
          uint8_t lastIndex;

          // move indices since we'll be deleting characters
          zerop5umIndex -= pmColsDel;
          oneumIndex -= pmColsDel;
          
          for(int k = pmStIndex; k < sizeof(sd_buf) - pmColsDel; k++)
          {
            sd_buf[k] = sd_buf[k + pmColsDel]; // move all data back by pmColsDel
          }
          for(int k = zerop5umIndex; k < sizeof(sd_buf) - cntsColDel; k++)
          {
            sd_buf[k] = sd_buf[k + cntsColDel]; // do the same to get rid of the 0.5um column
          }
          // Serial.println(sd_buf);
          // remove newline from the end of the line
          sd_buf[strlen(sd_buf) - 1] = 0;
          Serial.println(sd_buf);
          strcat(tspeak_buf, sd_buf);
          strcat(tspeak_buf, "|"); // add pipe character between updates
          charCount += lineCharCount;
        }

        lineCharCount = 0;
        memset(sd_buf, 0, sizeof(sd_buf));
      }
      else if(firstLineDone)
      {
        // fill up buffer with characters read
        if(c == ',')
        {
          colPositions[++colCount] = i+1;
        }
        sd_buf[i++] = c;
        lineCharCount++;
      }
    }

    // close file
    dataFile.close();

    // store the latest line read for next update
    // minus 1 because lineCount has been incremented, so at the end of the loop it's one line ahead
    lastLineRead = lineCount - 1;

    // update with the latest data
    httpRequest(tspeak_buf);
    firstLineDone = false;
  }
  else
  {
    Serial.println("unable to open file");
  }
}

// update samples in SD card
void updateSampleSD()
{
  buttonISREn = false;

  wakeGps();

  prevSampMillis = curMillis;

  readGps();

  // re-read GPS until data is valid
  while(!(gps.date.isValid() && gps.time.isValid() && gps.location.isValid()))
  {
    readGps();
  }

  setTime(gps.time.hour(), gps.time.minute(), gps.time.second(), gps.date.day(), gps.date.month(), gps.date.year());

  if(now() != prevTimeStamp)
  {
    Serial.println("Fresh timestamp");
    prevTimeStamp = now();
    staleFlag = false;
  }
  else
  {
    Serial.println("Stale timestamp");
    staleFlag = true;
  }
  

  while(!readDustSensor(i2c_buf, 29))
  {
    Serial.println("Sensor reading didn't work, trying again");
  }

  if(!parseSensorData(particleData, i2c_buf))
  {
    Serial.println("checksum incorrect, data will be stale");
  }

  // Display time stamp and concentration in the serial monitor
  // Format TIME: LPO%, CONCENTRATION pcs/0.01cf
  // only updates with fresh timestamps
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
    Serial.println(": ");
    
    Serial.print("PM1.0 (standard): "); Serial.print(particleData[0]); Serial.println(" ug/m^3");
    Serial.print("PM2.5 (standard): "); Serial.print(particleData[1]); Serial.println(" ug/m^3");
    Serial.print("PM10.0 (standard): "); Serial.print(particleData[2]); Serial.println(" ug/m^3");
    Serial.print("PM1.0 (atmospheric): "); Serial.print(particleData[3]); Serial.println(" ug/m^3");
    Serial.print("PM2.5 (atmospheric): "); Serial.print(particleData[4]); Serial.println(" ug/m^3");
    Serial.print("PM10.0 (atmospheric): "); Serial.print(particleData[5]); Serial.println(" ug/m^3");
    Serial.print("Particle concentration (>=0.3um): "); Serial.print(particleData[6]); Serial.println(" pcs/L");
    Serial.print("Particle concentration (>=0.5um): "); Serial.print(particleData[7]); Serial.println(" pcs/L");
    Serial.print("Particle concentration (>=1.0um): "); Serial.print(particleData[8]); Serial.println(" pcs/L");
    Serial.print("Particle concentration (>=2.5um): "); Serial.print(particleData[9]); Serial.println(" pcs/L");
    Serial.print("Particle concentration (>=5.0um): "); Serial.print(particleData[10]); Serial.println(" pcs/L");
    Serial.print("Particle concentration (>=10.0um): "); Serial.print(particleData[11]); Serial.println(" pcs/L");
    
    Serial.print("lat: ");
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
    dataFile.print(particleData[0]); // PM1.0 (standard)
    dataFile.print(",");
    dataFile.print(particleData[1]); // PM2.5 (standard)
    dataFile.print(",");
    dataFile.print(particleData[2]); // PM10.0 (standard)
    dataFile.print(",");
    dataFile.print(particleData[3]); // PM1.0 (atmo)
    dataFile.print(",");
    dataFile.print(particleData[4]); // PM2.5 (atmo)
    dataFile.print(",");
    dataFile.print(particleData[5]); // PM10.0 (atmo)
    dataFile.print(",");
    dataFile.print(particleData[6]); // >0.3um 
    dataFile.print(",");
    dataFile.print(particleData[7]); // >0.5um
    dataFile.print(",");
    dataFile.print(particleData[8]); // >1.0um
    dataFile.print(",");
    dataFile.print(particleData[9]); // >2.5um
    dataFile.print(",");
    dataFile.print(particleData[10]); // >5.0um
    dataFile.print(",");
    dataFile.print(particleData[11]); // >10.0um
    dataFile.print(",");
    dataFile.print(gps.location.lat(), 1);
    dataFile.print(",");
    dataFile.print(gps.location.lng(), 1);
    dataFile.print(",");
    dataFile.print(int(gps.altitude.meters()));
    dataFile.print(",");
    if(staleFlag)
    {
      dataFile.println("stale gps");
    }
    else
    {
      dataFile.println("good");
    }
    dataFile.close();

    char displayText[50];
    char timeText[50];
    char pm1p0Text[10];
    char pm2p5Text[10];
    char pm10p0Text[10];
    char hourText[10];
    char minuteText[10];
    char monthText[10];
    char dayText[10];
    char yearText[10];

    itoa(particleData[3], pm1p0Text, 10);
    itoa(particleData[4], pm2p5Text, 10);
    itoa(particleData[5], pm10p0Text, 10);

    itoa(gps.time.hour(), hourText, 10);
    itoa(gps.time.minute(), minuteText, 10);
    itoa(gps.date.month(), monthText, 10);
    itoa(gps.date.day(), dayText, 10);
    itoa(gps.date.year(), yearText, 10);

    strcpy(displayText, "PM1.0:  ");
    strcat(displayText, pm1p0Text);
    strcat(displayText, " ug/m\xb3");
    display(displayText, 8, true, false);

    strcpy(displayText, "PM2.5:  ");
    strcat(displayText, pm2p5Text);
    strcat(displayText, " ug/m\xb3");
    display(displayText, 16, false, false);

    strcpy(displayText, "PM10.0: ");
    strcat(displayText, pm10p0Text);
    strcat(displayText, " ug/m\xb3");
    display(displayText, 24, false, false);

    strcpy(timeText, monthText);
    strcat(timeText, "/");
    strcat(timeText, dayText);
    strcat(timeText, "/");
    strcat(timeText, yearText);
    strcat(timeText, " ");
    if(gps.time.hour() < 10)
    {
      strcat(timeText, "0");
    }
    strcat(timeText, hourText);
    strcat(timeText, ":");
    if(gps.time.minute() < 10)
    {
      strcat(timeText, "0");
    }
    strcat(timeText, minuteText);
    if(gps.time.age() >= 1500)
    {
      strcat(timeText, "(!)");
    }
    display(timeText, 32, false, true);

    ledFlag = true;
  }
  else
  {
    Serial.println("Couldn't open file");
    display("Couldn't open file", 20, true, true);
  }

  sleepGps();

  buttonISREn = true;
}

// form HTTP request for bulk updates
bool httpRequest(char* buffer)
{
  connectWiFi();

  client.stop();
  char data_length[10];
  char post[200];
  char channelID[10];
  itoa(channelNumber, channelID, 10);

  strcpy(post, "POST /channels/");
  strcat(post, channelID);
  strcat(post, "/bulk_update.csv HTTP/1.1");

  itoa(strlen(buffer), data_length, 10);

  if(client.connect(server, 80))
  {
    client.println(post);
    Serial.println(post);
    client.println("Host: api.thingspeak.com");
    Serial.println("Host: api.thingspeak.com");
    client.println("Content-Type: application/x-www-form-urlencoded");
    Serial.println("Content-Type: application/x-www-form-urlencoded");
    client.print("Content-Length: ");
    client.println(data_length);
    Serial.print("Content-Length: ");
    Serial.println(data_length);
    client.println();
    Serial.println();
    for(int i = 0; i < strlen(buffer); i++)
    {
      client.print(buffer[i]);
      Serial.print(buffer[i]);
    }
    client.println();
    Serial.println();
    // client.println(buffer);
    // Serial.println(buffer);
  }
  else
  {
    Serial.println("Failed to connect to ThingSpeak");
    return false;
  }

  delay(250);
  client.parseFloat();
  int resp = client.parseInt();
  if(resp == 202)
  {
    Serial.print("Successful update, code: ");
    Serial.println(resp);
    client.stop();
    Serial.println("Client stopped");
    WiFi.end();
    Serial.println("WiFi off");
    display("Updating to", 16, true, false);
    display("ThingSpeak...", 24, false, true);  
    return true;
  }
  else
  {
    Serial.print("Failed update, code: ");
    Serial.println(resp);
    client.stop();
    WiFi.end();
    return false;
  }
}

// blink LED
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

// connect to WiFi
void connectWiFi()
{
    // Initialize WiFi 
  WiFi.begin(ssid, password);
  
  unsigned long startTime = millis();
  display("Connecting to WiFi", 20, true, true);
  while(WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    if(millis() - startTime > 60000) // time out after a minute
    {
      Serial.println("Time out, reconnecting");
      WiFi.end();
      delay(250);
      WiFi.begin(ssid, password);
      startTime = millis();
    }
  }

  Serial.println("\nWiFi connected");
  display("WiFi connected", 20, true, true);
}

// read GPS module and encode data
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

// sleep the GPS module
void sleepGps()
{
  sendGpsCommand("105,8");
}

// wake up the GPS module
void wakeGps()
{
  sendGpsCommand("105,0");
}

// send GPS command
void sendGpsCommand(const char* cmd)
{
  char cmdBase[] = "PGKC";
  char* finalCmd = strcat(cmdBase, cmd); // data between the $ and * - on which checksum is based
  char checksum = createChecksum(finalCmd);

  Serial1.write('$');
  Serial1.write(finalCmd);
  Serial1.write('*');
  Serial1.print(checksum, HEX);
  Serial1.write("\r\n");
  
  Serial.print("Command sent to GPS module: ");
  Serial.write('$');
  Serial.write(finalCmd);
  Serial.write('*');
  Serial.print(checksum, HEX);
  Serial.write("\r\n");

}

// create a checksum for GPS command
char createChecksum(char* cmd)
{
  char checksum = 0;

  for(int i = 0; i < strlen(cmd); i++)
  {
    checksum = checksum ^ cmd[i];
  }

  return checksum;
}

// function for displaying characters to OLED 
void display(char* text, u8g2_uint_t height, bool clear, bool send)
{
  if(clear)
  {
    u8g2.clearBuffer();
  }
 
  // u8g2.setFont(u8g2_font_helvB08_tf); // TODO: look into other fonts
  u8g2.setFont(u8g2_font_synchronizer_nbp_tf);
  u8g2.drawStr(0, height, text);
  u8g2.sendBuffer();

  if(send)
  {
    u8g2.sendBuffer();
  }
}