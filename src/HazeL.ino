/*
 * HazeL
 * Benjamin Y. Brown
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
// #include <SD.h>
#include "SdFat.h"
#include <Wire.h>
#include "secrets.h"
#include "HM3301.h"

// Make sure you have these four libraries installed in Documents/Arduino/libraries
#include <U8g2lib.h>
#include <TinyGPS++.h>
#include <TimeLib.h>
#include <WiFi101.h>
#include "Seeed_BMP280.h"

#define SAMP_TIME 2500 // number of ms between sensor readings
#define BLINK_TIME 30 // time in ms between LED blinks on successful write to SD
#define GPS_TIME 10000 // time between GPS reads
#define BLINK_CNT 3 // number of times to blink LED on successful write
#define BUTTON_PIN A2 // pin for button that triggers ThingSpeak updates
#define SWITCH_PIN A3 // pin for switch that sets continual update mode
#define SD_CS_PIN 4 // CS pin of SD card, 4 on SD MKR proto shield
#define CUR_YEAR 2021 // for GPS first fix error checking
#define DEBUG_PRINT

HM3301 dustSensor;
BMP280 TPSensor;

SdFat SD;
File dataFile;
char dataFileName[] = "data.txt";

TinyGPSPlus gps;
bool firstGpsRead = true;
bool gpsFlag = false;

int localYear;
int localMonth;
int localDay;
int localHour;
int localMinute;
int localSecond;
double latitude;
double longitude;
double altitude;

time_t prevTimeStamp;

U8G2_SSD1306_128X64_ALT0_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

unsigned long prevSampMillis = 0;
unsigned long prevLedMillis = 0;
unsigned long prevGpsMillis = 0;
unsigned long curMillis;

volatile bool buttonFlag = false;
volatile bool buttonISREn = false;

bool ledFlag = false;
uint8_t ledCount = 0;

bool firstLineDone = false; // flag for first line (titles) having been read
bool newDataFile = false;
uint32_t lastLinePosition = 0;
char sd_buf[200]; // buffer to store single SD card line

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
#ifdef DEBUG_PRINT
  Serial.println("Initializing...");
#endif

  // Set relevant pin modes
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(SWITCH_PIN, INPUT_PULLUP);
  pinMode(SD_CS_PIN, OUTPUT);

#ifdef DEBUG_PRINT
  Serial.println("Initialize SD");
#endif
  display("Checking SD", 20, true, true);
  delay(2500);

  // Initialize SD card communication
  if(!SD.begin(SD_CS_PIN))
  {
#ifdef DEBUG_PRINT
    Serial.println("Card failed");
#endif
    display("SD card failed", 16, true, false);
    display("Reset device", 24, false, true);
    while(true);
  }
  else
  {
#ifdef DEBUG_PRINT
    Serial.println("Card initialized successfully");
#endif
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
#ifdef DEBUG_PRINT
    Serial.print("Creating ");
    Serial.println(dataFileName);
#endif
    // newDataFile = true;
    dataFile = SD.open(dataFileName, FILE_WRITE);
    if(dataFile)
    {
      dataFile.println('x'); // mark first line with an x, will use aline with an 'x' to indicate the location of the last line read
      dataFile.close();  
    }
    else
    {
#ifdef DEBUG_PRINT
      Serial.println("Couldn't open file");
#endif
      display("Unable to open file", 16, true, false);
      display("Check SD, reset device", 24, false, true);
    }

  }

  // Initialize dust sensor
  if(!dustSensor.begin())
  {
#ifdef DEBUG_PRINT
    Serial.println("Failed to initialize dust sensor");
#endif
    display("Dust sensor init failed", 16, true, false);
    display("Reset device", 24, false, true);
    while(true);
  }

  TPSensor.init();

  // Attach ISR for flipping buttonFlag when button is pressed
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

  // Read sensor and update SD card every SAMP_TIME milliseconds
  if(curMillis - prevSampMillis >= SAMP_TIME)
  {
    prevSampMillis = curMillis;
    if(curMillis - prevGpsMillis >= GPS_TIME)
    {
      gpsFlag = true;
      prevGpsMillis = curMillis;
    }
    updateSampleSD();
  }

  // Upload data.txt to serial monitor if buttonFlag has been set (inside buttonISR)
  if(buttonFlag)
  {
    buttonFlag = false;
    buttonISREn = true;
    uploadSerial();
  }

}

// ISR for button being pressed
void buttonISR()
{
  if(buttonISREn == true)
  {
    buttonFlag = true;
    buttonISREn = false;
  }
}

// update samples in SD card
void updateSampleSD()
{
  bool staleFlag = false;
  time_t localTime;
  time_t utcTime;

  BMP280_temp_t temp;
  BMP280_press_t press;

  if(gpsFlag)
  {
    if(firstGpsRead)
    {
      display("Reading GPS...", 16, true, false);
      display("(GPS warming up)", 24, false, true);
      firstGpsRead = false;
    }
    else{
      display("Reading GPS...", 20, true, true);
    }

    // wake up GPS module
    wakeGps();

    // Read GPS data until it's valid
    do
    {
      readGps();
    } while (!(gps.date.isValid() && gps.time.isValid() && gps.location.isValid() && gps.altitude.isValid() && gps.date.year() == CUR_YEAR));
    
    // set time for now()
    setTime(gps.time.hour(), gps.time.minute(), gps.time.second(), gps.date.day(), gps.date.month(), gps.date.year());

    latitude = gps.location.lat();
    longitude = gps.location.lng();
    altitude = gps.altitude.meters();

    // This is a band-aid for a bug where the the GPS sleep command would sometimes fail
    // TODO: figure out a way to verify if the GPS is asleep
    for (int i = 0; i < 10; i++)
    {
      sleepGps();
      delay(10);
    }

    // store UTC time
    utcTime = now();

    // convert to local time
    localTime = utcTime;
    localYear = year(localTime);
    localMonth = month(localTime);
    localDay = day(localTime);
    localHour = hour(localTime);
    localMinute = minute(localTime);
    localSecond = second(localTime);

    // read temperature and pressure
    temp = TPSensor.getTemperature();
    press = TPSensor.getPressure();
  }
  
  // Read dust sensor
  while(!dustSensor.read())
  {
#ifdef DEBUG_PRINT
    Serial.println("Sensor reading didn't work, trying again");
#endif
  }

  uint16_t PM1p0_std = dustSensor.data.PM1p0_std;
  uint16_t PM2p5_std = dustSensor.data.PM2p5_std;
  uint16_t PM10p0_std = dustSensor.data.PM10p0_std;
  uint16_t PM1p0_atm = dustSensor.data.PM1p0_atm;
  uint16_t PM2p5_atm = dustSensor.data.PM2p5_atm;
  uint16_t PM10p0_atm = dustSensor.data.PM10p0_atm;
  uint16_t count_0p3um = dustSensor.data.count_0p3um;
  uint16_t count_0p5um = dustSensor.data.count_0p5um;
  uint16_t count_1p0um = dustSensor.data.count_1p0um;
  uint16_t count_2p5um = dustSensor.data.count_2p5um;
  uint16_t count_5p0um = dustSensor.data.count_5p0um;
  uint16_t count_10p0um = dustSensor.data.count_10p0um;

  // Display data to serial monitor and OLED display
  // Store data on SD card
  dataFile = SD.open(dataFileName, FILE_WRITE);
  if(dataFile)
  {
    // Display time stamp and data in the serial monitor

    // Print out timestamp and temperature when gpsFlag is set
    if(gpsFlag)
    {
      Serial.print("# ");
      Serial.print(curMillis);
      Serial.print(',');
      Serial.print(localYear);
      Serial.print('-');
      Serial.print(localMonth);
      Serial.print('-');
      Serial.print(localDay);
      Serial.print('T');
      if(localHour < 10) Serial.print('0');
      Serial.print(localHour);
      Serial.print(':') ;
      if(localMinute < 10) Serial.print('0');
      Serial.print(localMinute);
      Serial.print(':');
      if(localSecond < 10) Serial.print('0');
      Serial.print(localSecond);
      Serial.print("+00:00");

      Serial.print(',');
      Serial.print(latitude, 5);
      Serial.print(',');
      Serial.print(longitude, 5);
      Serial.print(',');
      Serial.print(altitude, 2);

      Serial.print(',');
      Serial.print(temp.integral); Serial.print('.'); Serial.print(temp.fractional);
      Serial.print(',');
      Serial.print(press.integral); Serial.print('.'); Serial.println(press.fractional);
    }
    
    Serial.print(PM1p0_std);
    Serial.print(',');
    Serial.print(PM2p5_std);
    Serial.print(',');
    Serial.print(PM10p0_std);
    Serial.print(',');
    Serial.print(PM1p0_atm);
    Serial.print(',');
    Serial.print(PM2p5_atm);
    Serial.print(',');
    Serial.print(PM10p0_atm);
    Serial.print(',');
    Serial.print(count_0p3um);
    Serial.print(',');
    Serial.print(count_0p5um);
    Serial.print(',');
    Serial.print(count_1p0um);
    Serial.print(',');
    Serial.print(count_2p5um);
    Serial.print(',');
    Serial.print(count_5p0um);
    Serial.print(',');
    Serial.println(count_10p0um);

    // Update data.txt with the same information
    // char offsetString[5];
    // itoa(abs(timeZone), offsetString, 10);

  if(gpsFlag)
  {
    dataFile.print("# ");
    dataFile.print(curMillis);
    dataFile.print(',');
    dataFile.print(localYear);
    dataFile.print('-');
    dataFile.print(localMonth);
    dataFile.print('-');
    dataFile.print(localDay);
    dataFile.print('T');
    if(localHour < 10) dataFile.print('0');
    dataFile.print(localHour);
    dataFile.print(':') ;
    if(localMinute < 10) dataFile.print('0');
    dataFile.print(localMinute);
    dataFile.print(':');
    if(localSecond < 10) dataFile.print('0');
    dataFile.print(localSecond);
    dataFile.print("+00:00");

    dataFile.print(',');
    dataFile.print(latitude, 5);
    dataFile.print(',');
    dataFile.print(longitude, 5);
    dataFile.print(',');
    dataFile.print(altitude, 2);

    dataFile.print(',');
    dataFile.print(temp.integral); dataFile.print('.'); dataFile.print(temp.fractional);
    dataFile.print(',');
    dataFile.print(press.integral); dataFile.print('.'); dataFile.println(press.fractional);
  }

    dataFile.print(PM1p0_std); // PM1.0 (standard)
    dataFile.print(",");
    dataFile.print(PM2p5_std); // PM2.5 (standard)
    dataFile.print(",");
    dataFile.print(PM10p0_std); // PM10.0 (standard)
    dataFile.print(",");
    dataFile.print(PM1p0_atm); // PM1.0 (atmo)
    dataFile.print(",");
    dataFile.print(PM2p5_atm); // PM2.5 (atmo)
    dataFile.print(",");
    dataFile.print(PM10p0_atm); // PM10.0 (atmo)
    dataFile.print(",");
    dataFile.print(count_0p3um); // >0.3um 
    dataFile.print(",");
    dataFile.print(count_0p5um); // >0.5um
    dataFile.print(",");
    dataFile.print(count_1p0um); // >1.0um
    dataFile.print(",");
    dataFile.print(count_2p5um); // >2.5um
    dataFile.print(",");
    dataFile.print(count_5p0um); // >5.0um
    dataFile.print(",");
    dataFile.println(count_10p0um); // >10.0um
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

    itoa(PM1p0_atm, pm1p0Text, 10);
    itoa(PM2p5_atm, pm2p5Text, 10);
    itoa(PM10p0_atm, pm10p0Text, 10);

    itoa(localHour, hourText, 10);
    itoa(localMinute, minuteText, 10);
    itoa(localMonth, monthText, 10);
    itoa(localDay, dayText, 10);
    itoa(localYear, yearText, 10);

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
    if(localHour < 10)
    {
      strcat(timeText, "0");
    }
    strcat(timeText, hourText);
    strcat(timeText, ":");
    if(localMinute < 10)
    {
      strcat(timeText, "0");
    }
    strcat(timeText, minuteText);
    display(timeText, 32, false, true);

    ledFlag = true;
  }
  else
  {
#ifdef DEBUG_PRINT
    Serial.println("Couldn't open file");
#endif
    display("Couldn't open file", 20, true, true);
  }

  // Put GPS module to sleep
  if(gpsFlag)
  {
    gpsFlag = false;
  }

}

// upload SD card data over serial port
void uploadSerial()
{
  buttonISREn = false; // disable button ISR
  // First check is USB is connected
  if(!Serial)
  {
    display("USB not connected", 20, true, true);
    delay(5000);
    buttonISREn = true;
    return;
  }
  else
  {
    display("Uploading data", 16, true, false);
    display("via serial port", 24, false, true);
#ifdef DEBUG_PRINT
    Serial.println("Serial upload initiated");
#endif
    delay(5000);
  }
  
  // if switch is high (to the left), upload entire file
  if(digitalRead(SWITCH_PIN))
  {
#ifdef DEBUG_PRINT
    Serial.println("Mode 2, upload entire file");
#endif
    int i = 0;
    dataFile = SD.open(dataFileName, FILE_READ);
    while(dataFile.available())
    {
      char c = dataFile.read();
      if (c == 'x')
      {
        dataFile.read(); // read '\r'
        dataFile.read(); // read '\n'
        continue;
      }
      else
      {
        Serial.write(c);
      }
    }
  }
  else // if switch is low (to the right), only upload from the location in the file where last update ended
  {
#ifdef DEBUG_PRINT
    Serial.println("Mode 3, incremental upload");
#endif
    bool xFound = false; // find the x, indicating last line read
    uint32_t xPosition;  // store position of x to delete it later
    int i = 0;
    dataFile = SD.open(dataFileName, FILE_READ);
    if(dataFile)
    {
      while(dataFile.available())
      {
        char c = dataFile.read();
        
        // first loop through to find the x
        if(!xFound)
        {
          if(c == 'x')
          {
            xFound = true;
            xPosition = dataFile.position();
            dataFile.read(); // read '\r'
            dataFile.read(); // read '\n'
          }
          continue; // read next character, don't do any of the rest of this loop
        }
        else
        {
          if(c == '\n')
          {
            i = 0; // reset index
            Serial.println(sd_buf); // print line to serial port
            memset(sd_buf, 0, sizeof(sd_buf)); // reset buffer holding line to 0
          }
          else
          {
            sd_buf[i++] = c; // add character to buffer holding current line
          }
        }
      }
      dataFile.close();
    }
    else
    {
#ifdef DEBUG_PRINT
      Serial.println("Couldn't open file");
#endif
    }
    // Now remove x, add it to the end of the file  
    // Open temporary file and data.txt
    File tmpFile = SD.open("tmp.txt", FILE_WRITE);
    dataFile = SD.open(dataFileName, FILE_READ);
    if(dataFile && tmpFile)
    {
      // Move data, minus the x and following CR and NL, into tmp.txt
      while(dataFile.available())
      {
        char c = dataFile.read();
        if (c == 'x')
        {
          // ignore x and following '\r' and '\n'
          dataFile.read();
          dataFile.read();
          continue;
        }
        tmpFile.write(c);
      }

      // Rename tmp.txt to data.txt, delete data.txt
      dataFile.rename("datatmp.txt");
      tmpFile.rename(dataFileName);
      dataFile.close();
      tmpFile.close();
      SD.remove("datatmp.txt");
      
      tmpFile = SD.open(dataFileName, O_RDWR);

      tmpFile.seek(tmpFile.size()-1); // go to end of file
      tmpFile.println('x'); // an 'x' line
      tmpFile.close();
    }
    else
    {
#ifdef DEBUG_PRINT
      Serial.println("Couldn't open tmp and data files");
#endif
    }
  }
  buttonISREn = true;
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

// read GPS module and encode data
void readGps()
{
  while(Serial1.available() > 0)
  {
    if(gps.encode(Serial1.read()))
    {
#ifdef DEBUG_PRINT
      Serial.println("GPS data successfully encoded");
#endif
    }
  }
}

// sleep the GPS module
void sleepGps()
{
  sendGpsCommand("051,1");
}

// wake up the GPS module
void wakeGps()
{
  sendGpsCommand("051,0");
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

#ifdef DEBUG_PRINT
  Serial.print("Command sent to GPS: ");
  Serial.write('$');
  Serial.write(finalCmd);
  Serial.write('*');
  Serial.print(checksum, HEX);
  Serial.write("\r\n");
#endif
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

/*
 * Functions and variables below this point are used for updating to ThingSpeak, not used in this version
 */

WiFiClient client;
const char server[] = "api.thingspeak.com";

char tspeak_buf[5000]; // buffer for storing multiple rows of data from the CSV for ThingSpeak bulk updates
char status_buf[20]; // buffer for status text
uint8_t colPositions[17] = {0}; // array to store indices of commas in sd_buf, indicating column delineations
unsigned long prevFileSize = 0; // last recorded file size
unsigned long bytesLeft = 0; // bytes left in file to update to ThingSpeak

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