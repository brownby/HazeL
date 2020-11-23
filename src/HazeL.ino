/*
 * ES6_sensor
 * Benjamin Y. Brown
 * Last updated: 11/23/20
 * 
 * Creates a file called "data.csv" on the SD card with columns for time stamp, PM concentrations, raw particle concentrations, and latitude and longitude
 *
 * Device will bulk update to ThingSpeak when button is pressed (may take a few minutes depending on amount of data)
 * For automatic ThingSpeak updates every 30 seconds, pull SWITCH_PIN low
 * 
 * Before uploading, enter the information for your WiFi SSID and password, 
 * as well as your ThingSpeak channel ID and write API key in "secrets.h"
 * 
 * GPS module may return stale time stamps depending on signal strength 
 * All data will be stored on SD card, regardless of staleness of time stamps
 * but only data points with unique time stamps will be updated to ThingSpeak
 */

#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include "secrets.h"

// Make sure you have these four libraries installed in Documents/Arduino/libraries
#include <U8g2lib.h>
#include <WiFi101.h>
#include <TinyGPS++.h>
#include <TimeLib.h>

#define SAMP_TIME 5000 // number of ms between sensor readings
#define BLINK_TIME 300 // time in ms between LED blinks on successful write to SD
#define WIFI_TIME 30000 // number of ms between WiFi updates in continual update mode
#define BLINK_CNT 3 // number of times to blink LED on successful write
#define BUTTON_PIN A2 // pin for button that triggers ThingSpeak updates
#define SWITCH_PIN A3 // pin for switch that sets continual update mode
#define SD_CS_PIN 4 // CS pin of SD card, 4 on SD MKR proto shield
#define SENSOR_ADDR 0x40 // I2C address of dust sensor
#define CUR_YEAR 2020 // for GPS first fix error checking
// #define SECS_PER_HOUR 3600

WiFiClient client;

File dataFile;
char dataFileName[] = "data.csv";

TinyGPSPlus gps;
bool firstGpsRead = true;

time_t prevTimeStamp;
time_t localTime;

U8G2_SSD1306_128X64_ALT0_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const char server[] = "api.thingspeak.com";

unsigned long prevSampMillis = 0;
unsigned long prevLedMillis = 0;
unsigned long prevWiFiMillis = 0;
unsigned long curMillis;

volatile bool wifiFlag = false;
volatile bool buttonISREn = false;

bool ledFlag = false;
uint8_t ledCount = 0;

/* 
Array for parsed dust sensor data
particleData[0]  = PM1.0, standard  (ug/m^3)
particleData[1]  = PM2.5, standard  (ug/m^3)
particleData[2]  = PM10.0, standard (ug/m^3)
particleData[3]  = PM1.0, atmo  (ug/m^3)
particleData[4]  = PM2.5, atmo  (ug/m^3)
particleData[5]  = PM10.0, atmo (ug/m^3)
particleData[6]  = >0.3um  (pcs/L)
particleData[7]  = >0.5um  (pcs/L)
particleData[8]  = >1.0um  (pcs/L)
particleData[9]  = >2.5um  (pcs/L)
particleData[10] = >5.0um  (pcs/L)
particleData[11] = >10.0um (pcs/L)
*/
uint16_t particleData[12];

uint8_t i2c_buf[30]; // data buffer for pulling data from dust sensor

bool firstLineDone = false; // flag for first line (titles) having been read
uint32_t lastLinePosition = 0;
char sd_buf[200]; // buffer to store single SD card line
char tspeak_buf[5000]; // buffer for storing multiple rows of data from the CSV for ThingSpeak bulk updates
char status_buf[20]; // buffer for status text
uint8_t colPositions[17] = {0}; // array to store indices of commas in sd_buf, indicating column delineations
unsigned long prevFileSize = 0; // last recorded file size
unsigned long bytesLeft = 0; // bytes left in file to update to ThingSpeak

void setup() {
  // initialize Serial port
  Serial.begin(115200);

  // intialize comms with GPS object
  Serial1.begin(9600);

  // Initialize I2C bus
  Wire.begin();

  // Initialize comms with OLED display
  u8g2.begin();

  display("Initializing...", 20, true, true);
  delay(2500);

  // Set relevant pin modes
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(SWITCH_PIN, INPUT_PULLUP);
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
  // If CSV already exists, data will just be appended at the end
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

  // Initialize dust sensor
  if(!initDustSensor())
  {
    Serial.println("Failed to initialize dust sensor");
    display("Dust sensor init failed", 16, true, false);
    display("Reset device", 24, false, true);
    while(true);
  }

  // Attach ISR for flipping wifiFlag when button is pressed
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, RISING);

  // put the GPS module to sleep
  sleepGps();

  // enable button ISR
  buttonISREn = true;
}

void loop() {
  // check number of milliseconds since Arduino was turned on
  curMillis = millis();

  // Blink LED upon successful SD write
  if(ledFlag)
  {
    if(curMillis - prevLedMillis >= BLINK_TIME)
    {
      prevLedMillis = curMillis;
      blinkLed();
    }
  }

  // Set WiFi flag to update to ThingSpeak if switch is set to continual update mode
  if((curMillis - prevWiFiMillis >= WIFI_TIME) && (!digitalRead(SWITCH_PIN)))
  {
    prevWiFiMillis = curMillis;
    wifiFlag = true;
  }


  // Read sensor and update SD card every SAMP_TIME milliseconds
  if(curMillis - prevSampMillis >= SAMP_TIME)
  {
    prevSampMillis = curMillis;
    updateSampleSD();
    Serial.println("Updated sample in SD card");
    Serial.println();
  }

  // Update to ThingSpeak if wifiFlag has been set (inside buttonISR)
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

// moves raw data read from I2C bus into parsed buffer (particleData in this sketch)
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

// updates to ThingSpeak in bulk updates of 5kB of data
void updateThingSpeak()
{
  // Initialize tspeak_buf
  memset(tspeak_buf, 0, sizeof(tspeak_buf));
  strcpy(tspeak_buf, "write_api_key=");
  strcat(tspeak_buf, writeApiKey);
  strcat(tspeak_buf, "&time_format=absolute&updates=");

  // Open file on SD card
  display("Updating to", 16, true, false);
  display("ThingSpeak...00%", 24, false, true);
  dataFile = SD.open(dataFileName, FILE_READ);
  if(dataFile)
  {
    uint32_t charCount = strlen(tspeak_buf);
    uint32 rawByteCount = 0;
    uint32_t colCount = 0; 
    uint32_t i = 0;
    uint32_t linePosition = 0; // location in file of most recent line
    int percentComplete = 0;

    dataFile.seek(lastLinePosition);
    bytesLeft = dataFile.size() - prevFileSize;
    prevFileSize = dataFile.size();
    
    while(dataFile.available())
    {
      char c = dataFile.read();
      rawByteCount++;

      // Every time you fill up tspeak_buf, send an update to thing speak
      if((charCount >= sizeof(tspeak_buf) - 200) && (c != '\n'))
      {
        Serial.println("Buffer full!");
        Serial.println(charCount);
        tspeak_buf[strlen(tspeak_buf) - 1] = 0; // remove last pipe character

        while(!httpRequest(tspeak_buf)); // keep trying until ThingSpeak/WiFi connection works
        percentComplete = (rawByteCount*100)/bytesLeft;

        // reset byte counter and thingspeak buffer
        memset(tspeak_buf, 0, sizeof(tspeak_buf));
        strcpy(tspeak_buf, "write_api_key=");
        strcat(tspeak_buf, writeApiKey);
        strcat(tspeak_buf, "&time_format=absolute&updates=");

        i = 0;
        colCount = 0;
        charCount = strlen(tspeak_buf);

        char tspeakDisplay[20];
        char percentDisplay[10];
        itoa(percentComplete, percentDisplay, 10);
        strcpy(tspeakDisplay, "ThingSpeak...");
        if(percentComplete < 10)
        {
          strcat(tspeakDisplay, "0");
        }
        strcat(tspeakDisplay, percentDisplay);
        strcat(tspeakDisplay, "%");

        Serial.print("Update "); Serial.print(percentComplete); Serial.println("% done");

        // clear SD line buffer and move back to start of latest line
        memset(sd_buf, 0, sizeof(sd_buf));
        dataFile.seek(linePosition);

        display("Updating to", 16, true, false);
        display(tspeakDisplay, 24, false, true);
        delay(15000); // can only update to Thingspeak every 15s

        Serial.println("Back to updating");
      }
      else if(c == '\n')
      {
        // on a new line, process data on line
        i = 0;
        colCount = 0;

        // store position in file of the start of next line
        linePosition = dataFile.position();

        // One time if statement to avoid reading the first row, which has titles of columns
        if(!firstLineDone)
        {
          firstLineDone = true;
          continue;
        }

        // only update line if GPS data isn't stale
        memset(status_buf, 0, sizeof(status_buf));
        int j = 0;
        for(int k = colPositions[16]; k < strlen(sd_buf); k++)
        {
          status_buf[j++] = sd_buf[k];
        }

        // if GPS time stamp isn't stale, include in tspeak_buf (data to be sent to thingspeak)
        if(status_buf[0] == 'g')
        {
          Serial.println("Updating thingspeak buffer");

          // Remove columns for standard PM data and 0.5um particles
          uint8_t pmStIndex = colPositions[1]; // index of first standard PM column
          uint8_t pmAtIndex = colPositions[4]; // index of first atmo PM column
          uint8_t zerop5umIndex = colPositions[8]; // index of 0.5um particles
          uint8_t oneumIndex = colPositions[9]; // index of 1um particles
          uint8_t pmColsDel = pmAtIndex - pmStIndex; // PM columns to delete
          uint8_t cntsColDel = oneumIndex - zerop5umIndex; // Particle count columns to be deleted

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
          
          // remove newline from the end of the line
          sd_buf[strlen(sd_buf) - 1] = 0;
          Serial.println(sd_buf);

          // SD data is already formatted for ThingSpeak updates, just concatenate each line with pipe character in between
          strcat(tspeak_buf, sd_buf);
          strcat(tspeak_buf, "|"); // add pipe character between updates
          charCount += strlen(sd_buf)+1;
        }

        memset(sd_buf, 0, sizeof(sd_buf));
      }
      else if(firstLineDone)
      {
        // fill up buffer with characters read
        if(c == ',')
        {
          colPositions[++colCount] = i+1; // store start of data in column, so character following a comma
        }
        sd_buf[i++] = c;
      }
    }

    // close file
    dataFile.close();

    // store the position of the latest line that was read
    lastLinePosition = linePosition;

    // update with the latest data
    tspeak_buf[strlen(tspeak_buf) - 1] = 0; // remove last pipe character
    
    // Only update to ThingSpeak if there's data leftover
    if(strlen(tspeak_buf) >= 60)
    {
      while(!httpRequest(tspeak_buf));
    }

    display("All data updated", 16, true, false);
    display("to ThingSpeak!", 24, false, true);
    delay(5000);
  }
  else
  {
    Serial.println("unable to open file");
    display("Couldn't open file", 20, true, false);
    delay(5000);
  }
}

// update samples in SD card
void updateSampleSD()
{
  bool staleFlag = false;

  // disable button ISR (do I still want to do this?)
  buttonISREn = false;

  // wake up GPS module
  wakeGps();

  if(firstGpsRead)
  {
    display("Reading GPS...", 16, true, false);
    display("(GPS warming up)", 24, false, true);
    firstGpsRead = false;
  }
  else{
    display("Reading GPS...", 20, true, true);
  }

  // Read GPS data until it's valid
  do
  {
    readGps();
  } while (!(gps.date.isValid() && gps.time.isValid() && gps.location.isValid() && gps.altitude.isValid() && gps.date.year() == CUR_YEAR));
  
  // set time for now()
  setTime(gps.time.hour(), gps.time.minute(), gps.time.second(), gps.date.day(), gps.date.month(), gps.date.year());

  // convert to local time
  localTime = now() - (time_t)SECS_PER_HOUR*TIME_ZONE;

  // only consider time stamp fresh if it occurs after the previous
  if((now() > prevTimeStamp) && (gps.time.age() < 2500))
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
  
  // Read dust sensor
  while(!readDustSensor(i2c_buf, 29))
  {
    Serial.println("Sensor reading didn't work, trying again");
  }

  // Parse dust sensor data
  if(!parseSensorData(particleData, i2c_buf))
  {
    Serial.println("checksum incorrect, data will be stale");
  }

  // Display data to serial monitor and OLED display
  // Store data on SD card
  dataFile = SD.open(dataFileName, FILE_WRITE);
  if(dataFile)
  {
    // Display time stamp and data in the serial monitor
    Serial.print(month(localTime));
    Serial.print('/');
    Serial.print(day(localTime));
    Serial.print('/');
    Serial.print(year(localTime));
    Serial.print(' ');
    if(hour(localTime) < 10) Serial.print('0');
    Serial.print(hour(localTime));
    Serial.print(':') ;
    if(minute(localTime) < 10) Serial.print('0');
    Serial.print(minute(localTime));
    Serial.print(':');
    if(second(localTime) < 10) Serial.print('0');
    Serial.print(second(localTime));
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
    char offsetString[5];
    itoa((int)abs(TIME_ZONE), offsetString, 10);

    // use ISO 8601 format for timestamp
    dataFile.print(year(localTime));
    dataFile.print('-');
    dataFile.print(month(localTime));
    dataFile.print('-');
    dataFile.print(day(localTime));
    dataFile.print('T');
    if(hour(localTime) < 10) dataFile.print('0');
    dataFile.print(hour(localTime));
    dataFile.print(':');
    if(minute(localTime) < 10) dataFile.print('0');
    dataFile.print(minute(localTime));
    dataFile.print(":");
    if(second(localTime) < 10) dataFile.print('0');
    dataFile.print(second(localTime));
    if(TIME_ZONE < 0)
    {
      dataFile.print('-');
    }
    else if(TIME_ZONE >= 0)
    {
      dataFile.print('+');
    }
    if(abs(TIME_ZONE) < 10)
    {
      dataFile.print('0');
    }
    dataFile.print(offsetString);
    dataFile.print(":00,");
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

    itoa(hour(localTime), hourText, 10);
    itoa(minute(localTime), minuteText, 10);
    itoa(month(localTime), monthText, 10);
    itoa(day(localTime), dayText, 10);
    itoa(year(localTime), yearText, 10);

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
    if(hour(localTime) < 10)
    {
      strcat(timeText, "0");
    }
    strcat(timeText, hourText);
    strcat(timeText, ":");
    if(minute(localTime) < 10)
    {
      strcat(timeText, "0");
    }
    strcat(timeText, minuteText);
    if(staleFlag) // if data point has stale time stamp, display (!) next to time
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

  // Put GPS module to sleep
  sleepGps();

  // Re-enable buttonISR
  buttonISREn = true;
}

// form HTTP request for bulk updates
bool httpRequest(char* buffer)
{
  bool success = false;

  // Connect to WiFi
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
  }
  else
  {
    Serial.println("Failed to connect to ThingSpeak");
    return false;
  }

  delay(250);
  char displayBuffer[20] = "Code: ";
  char codeBuffer[10];
  client.parseFloat();
  int resp = client.parseInt();
  itoa(resp, codeBuffer, 10);
  strcat(displayBuffer, codeBuffer);
  if(resp == 202)
  {
    Serial.print("Successful update, code: ");
    Serial.println(resp);
    display("Successful update", 16, true, false);
    display(displayBuffer, 24, false, true);
    success = true;
    delay(5000);
  }
  else
  {
    Serial.print("Failed update, code: ");
    Serial.println(resp);
    display("Failed update", 16, true, false);
    display(displayBuffer, 24, false, true);
    success = false;
    delay(5000);
  }

    client.stop();
    Serial.println("Client stopped");
    WiFi.end();
    Serial.println("WiFi off");

    return success;
}

// blink LED
void blinkLed()
{
  
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
  unsigned long startTime = millis();
  display("Connecting to WiFi", 20, true, true);

  WiFi.begin(ssid, password);
  
  while(WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    if(millis() - startTime > 20000) // time out after a 20s, then retry
    {
      Serial.println("\nTime out, reconnecting");
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