/*
 * HazeL
 * Benjamin Y. Brown
 */

#include <SPI.h>
#include <Wire.h>
#include "HM3301.h"

// Make sure you have these five libraries installed in Documents/Arduino/libraries
#include <Adafruit_I2CDevice.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1327.h>
#include <Encoder.h>
#include "SdFat.h"
#include <TinyGPS++.h>
#include <TimeLib.h>
#include <RTCZero.h>
#include "Seeed_BMP280.h"

#define SAMP_TIME 2500 // number of ms between sensor readings
#define BLINK_TIME 30 // time in ms between LED blinks on successful write to SD
#define GPS_TIME 10000 // time between GPS reads
#define GPS_TIMEOUT 5000 // number of ms before GPS read times out
#define GPS_FIRST_TIMEOUT 600000 // number of ms before first GPS read times out
#define BLINK_CNT 3 // number of times to blink LED on successful write
#define SD_CS_PIN 4 // CS pin of SD card, 4 on SD MKR proto shield
#define CUR_YEAR 2022 // for GPS first fix error checking
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3D
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 128
#define ENC_RIGHT_BUTTON A1
#define ENC_RIGHT_A 0
#define ENC_RIGHT_B 1
#define ENC_LEFT_BUTTON A2
#define ENC_LEFT_A 5
#define ENC_LEFT_B 7
#define MENU_UPDATE_TIME 100 // milliseconds between menu updates
// #define DEBUG_PRINT

HM3301 dustSensor;
BMP280 TPSensor;

SdFat SD;
File dataFile;
File metaFile;
char dataFileName[23]; // YYMMDD_HHMMSS_data.txt
char metaFileName[23]; // YYMMDD_HHMMSS_meta.txt
char * fileList; // list of files on SD card
uint32_t fileCount = 0; // number of files on SD card
char fileToUpload[30];

TinyGPSPlus gps;
bool timestampFlag = false;
bool gpsAwake = true;
bool gpsDisplayFail = false;

int utcYear;
int utcMonth;
int utcDay;
int utcHour;
int utcMinute;
int utcSecond;
double latitude;
double longitude;
double altitude;

time_t prevTimeStamp = 0;
uint8_t manualMonth = 1;
uint8_t manualDay = 1;
uint16_t manualYear = CUR_YEAR;
uint8_t manualHour = 0;
uint8_t manualMinute = 0;
bool manualTimeEntry = false; // false means use GPS
bool rtcSet = false; // flag to indicate if RTC is set or not
RTCZero rtc;

Adafruit_SSD1327 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

Encoder encRight(ENC_RIGHT_B, ENC_RIGHT_A);
Encoder encLeft(ENC_LEFT_B, ENC_LEFT_A);
long encRightOldPosition = 0;
long encLeftOldPosition = 0;

unsigned long prevSampMillis = 0;
unsigned long prevLedMillis = 0;
unsigned long prevGpsMillis = 0;
unsigned long prevMenuMillis = 0;
unsigned long dataStartMillis = 0; // millis() when data collection began
unsigned long curMillis;

volatile bool encRightButtonFlag = false;
volatile bool encRightButtonISREn = false;
volatile bool encLeftButtonFlag = false;
volatile bool encLeftButtonISREn = false;

bool ledFlag = false;
uint8_t ledCount = 0;

bool firstMeasurementFlag = false;

// state = 0 navigating menu
// state = 2 collecting data
// state = 3 uploading data
uint8_t state = 0;
uint8_t prevState = 0;

// page = 0 two choice menu, collect data and upload data
// page = 1 time entry choice menu -> two choices enter timestamp or get from GPS
// page = 2 entering date
// page = 3 entering time
// page = 4 viewing SD card files
// page = 5 data collection screen
uint8_t page = 0;
uint8_t prevPage = 0;
int16_t currentVertMenuSelection = 0;
int16_t currentHoriMenuSelection = 0;
int16_t prevVertMenuSelection = 0;
uint8_t scroll = 0; // count number of times SD page has been scrolled

void setup() {
  // initialize Serial port
  Serial.begin(115200);

  // intialize comms with GPS module
  Serial1.begin(9600);

  // Initialize I2C bus
  Wire.begin();

  // Initialize comms with OLED display
  display.begin(SCREEN_ADDRESS);

  display.clearDisplay();
  updateDisplay("Initializing...", 40, false);
  display.display();
  delay(2500);
  #ifdef DEBUG_PRINT
  Serial.println("Initializing...");
  #endif

  // Set relevant pin modes
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(SD_CS_PIN, OUTPUT);
  pinMode(ENC_RIGHT_BUTTON, INPUT_PULLUP);
  pinMode(ENC_LEFT_BUTTON, INPUT_PULLUP);

  #ifdef DEBUG_PRINT
  Serial.println("Initialize SD");
  #endif
  display.clearDisplay();
  updateDisplay("Checking SD", 40, false);
  display.display();
  delay(2500);

  // Initialize SD card communication
  if(!SD.begin(SD_CS_PIN))
  {
    #ifdef DEBUG_PRINT
    Serial.println("Card failed");
    #endif
    display.clearDisplay();
    updateDisplay("SD card failed", 32, false);
    updateDisplay("Reset device", 48, false);
    display.display();
    while(true);
  }
  else
  {
    #ifdef DEBUG_PRINT
    Serial.println("Card initialized successfully");
    #endif
    display.clearDisplay();
    updateDisplay("SD card detected", 40, false);
    display.display();
  }
  delay(2500);

  // Initialize dust sensor
  if(!dustSensor.begin())
  {
    #ifdef DEBUG_PRINT
    Serial.println("Failed to initialize dust sensor");
    #endif
    display.clearDisplay();
    updateDisplay("Dust sensor init failed", 32, false);
    updateDisplay("Reset device", 48, false);
    display.display();
    while(true);
  }

  TPSensor.init();

  // Put GPS to sleep to start
  if(gpsAwake)
  {
    toggleGps();
  }

  // Begin RTC
  rtc.begin();

  // Attach ISR for flipping buttonFlag when button is pressed
  attachInterrupt(digitalPinToInterrupt(ENC_RIGHT_BUTTON), encRightButtonISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(ENC_LEFT_BUTTON), encLeftButtonISR, FALLING);

  // enable button ISR
  encRightButtonISREn = true;
  encLeftButtonISREn = true;

  // set initial state to menu navigation
  state = 0;
}

void loop() {
  // check number of milliseconds since Arduino was turned on
  curMillis = millis();

  // display the current page
  displayPage(page);

  if(state == 0) // Navigating menus
  {
    // update the current menu selection
    updateMenuSelection();

    // Check for serial commands
    String cmd = "";
    while (Serial.available() > 0)
    {
      char c = Serial.read();
      // End of command
      if (c == '\n')
      {
        // Send list of files
        if (cmd == "ls")
        {
          getFileList();
          char allFiles[fileCount][30];
          memcpy(allFiles, fileList, sizeof(allFiles));

          for (int i = 0; i < fileCount; i++)
          {
            Serial.print(allFiles[i]);
            Serial.print('\n');
          }
          Serial.print('\x04');

          free(fileList);
        }
      }
      else
      {
        cmd += c;
      }
    }

    if(encRightButtonFlag) // select button has been pressed
    {
      if(page == 0) // initial menu
      {
        if(currentVertMenuSelection == 0)
        {
          page = 1; // go to time entry choice page
        }
        else if(currentVertMenuSelection == 1) // upload data
        {
          prevState = state;
          page = 4; // go to page for viewing SD card files
          scroll = 0; // start at the beginning of the file list
          #ifdef DEBUG_PRINT
          Serial.println("\nSD card contents from SD.ls():");
          SD.ls(LS_R);
          Serial.println();
          #endif

          getFileList();
        }
      }
      else if (page == 1) // time entry method
      {
        if(currentVertMenuSelection == 0) // Use GPS for time stamp
        {
          manualTimeEntry = false;
          createDataFiles(); // create the names for the data and gps files for data collection
          // State and page are set from within createDataFiles() so that I can set it to different things on success and failure
        }
        else if(currentVertMenuSelection == 1) // Use manual entry + RTC
        {
          page = 2; // enter date
          if(rtcSet)
          {
            manualDay = rtc.getDay();
            manualHour = rtc.getHours();
            manualMinute = rtc.getMinutes();
            manualMonth = rtc.getMonth();
            manualYear = rtc.getYear();
          }
        }
      }
      else if (page == 2)
      {
        page = 3; // enter time
      }
      else if (page == 3)
      {
        #ifdef DEBUG_PRINT
        Serial.print("Timestamp set as: ");
        Serial.print(manualMonth);
        Serial.print('/');
        Serial.print(manualDay);
        Serial.print('/');
        Serial.print(manualYear);
        Serial.print(' ');
        if(manualHour < 10) Serial.print('0');
        Serial.print(manualHour);
        Serial.print(':');
        if(manualMinute < 10) Serial.print('0');
        Serial.println(manualMinute);
        #endif

        // set RTC
        rtc.setDate(manualDay, manualMonth, manualYear % 100); // year is saved as an offset from 2000
        rtc.setTime(manualHour, manualMinute, 0);

        if(!rtcSet) rtcSet = true;
        
        manualTimeEntry = true; // flag to indicate that RTC is being used for time stamps, not GPS
        createDataFiles();
        // State and page are set from within createDataFiles() so that I can set it to different things on success and failure
      }
      else if(page == 4)
      {
        // save fileToUpload before uploadSerial is called on it
        memcpy(fileToUpload, fileList + currentVertMenuSelection*30, sizeof(fileToUpload));
        prevState = state;
        state = 3;
      }

      // reset menus for next page
      if(page == 2) currentVertMenuSelection = manualMonth - 1;
      else if(page == 3) currentVertMenuSelection = manualHour;
      else if(page == 4) currentVertMenuSelection = currentVertMenuSelection; // known minor bug here where upon first entering this page the cursor will be on the second selection
      else currentVertMenuSelection = 0;
      currentHoriMenuSelection = 0;
      encRightButtonFlag = false;
      encRightButtonISREn = true;
    }
  }
  else if(state == 2) // Collecting data
  {
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
        timestampFlag = true;
        prevGpsMillis = curMillis;
      }
      updateSampleSD();
    }
  }
  else if(state == 3) // uploading data
  {
    uploadSerial(fileToUpload);
    state = prevState;
    prevState = 3;
    page = 4; // go back to file list
  }

  if(encLeftButtonFlag) // back button has been pressed
  {
    switch(page)
    {
      case 1: // time entry choice menu
        page = 0; // go back to initial menu
        prevState = state;
        state = 0;
        break;
      case 2: // date entry page
        page = 1; // go back to time entry choice menu
        prevState = state;
        state = 0;
        break;
      case 3: // time entry page
        page = 2; // go back to date entry page
        prevState = state;
        state = 0;
        break;
      case 4: // SD card file list menu
        page = 0; // go back to initial menu
        prevState = state;
        state = 0;
        #ifdef DEBUG_PRINT
        Serial.println("free()'ing fileList");
        #endif
        free(fileList); // deallocate memory for list of SD card files
        scroll = 0;
        break;
      case 5: // data collection screen
        page = 1; // go back to time entry choice menu
        prevState = state;
        state = 0;
        break;
    }
    // reset menus for next page
    if(page == 2) currentVertMenuSelection = manualMonth - 1;
    else if(page == 3) currentVertMenuSelection = manualHour;
    else currentVertMenuSelection = 0;
    encLeftButtonFlag = false;
    encLeftButtonISREn = true;
    #ifdef DEBUG_PRINT
    Serial.println("Back button pressed");
    Serial.print("Going to page: ");
    Serial.println(page);
    #endif
  }

}

// ISR for button being pressed
void encRightButtonISR()
{
  if(encRightButtonISREn == true)
  {
    encRightButtonFlag = true;
    encRightButtonISREn = false;
  }
}

void encLeftButtonISR()
{
  if(encLeftButtonISREn == true)
  {
    encLeftButtonFlag = true;
    encLeftButtonISREn = false;
  }
}

// update samples in SD card
void updateSampleSD()
{
  bool timeoutFlag = false;
  time_t utcTime;

  unsigned long msTimer;

  if(firstMeasurementFlag)
  {
    firstMeasurementFlag = false;
    msTimer = 0;
    dataStartMillis = millis();
  }
  else
  {
    msTimer = millis() - dataStartMillis;
  }

  BMP280_temp_t temp;
  BMP280_press_t press;

  if(timestampFlag) // if it is time to get a time stamp
  {
    // read temperature and pressure
    temp = TPSensor.getTemperature();
    press = TPSensor.getPressure();

    // If you chose to use GPS, keep getting time, lat, long, and alt fom GPS
    if(!manualTimeEntry)
    {
      display.clearDisplay();
      display.drawLine(0, display.height()-10, display.width()-1, display.height()-10, SSD1327_WHITE);
      display.drawLine(display.width()/2 - 1, display.height()-10, display.width()/2 - 1, display.height()-1, SSD1327_WHITE);
      display.setTextColor(SSD1327_WHITE);
      display.setCursor(10, display.height()-8);
      display.print("Back ");
      updateDisplay("Reading GPS...", 40, false);
      display.display();

      // wake up GPS module
      if (!gpsAwake)
      {
        toggleGps();
      }
      unsigned long gpsReadCurMillis;
      unsigned long gpsReadStartMillis = millis();
      unsigned long gpsTimeoutMillis = GPS_TIMEOUT;

      #ifdef DEBUG_PRINT
      unsigned long preRead = millis();
      #endif

      // Read GPS data until it's valid
      // 5 second timeout
      while (true)
      {
        gpsReadCurMillis = millis();

        readGps();

        if (gpsReadCurMillis - gpsReadStartMillis >= gpsTimeoutMillis)
        {
          timeoutFlag = true;
          gpsDisplayFail = true;
          #ifdef DEBUG_PRINT 
          Serial.println("GPS timeout");
          #endif
          break;
        }

        if (gps.date.isValid() && gps.time.isValid() && gps.location.isValid() && gps.altitude.isValid() && gps.date.year() == CUR_YEAR)
        {
          #ifdef DEBUG_PRINT
          Serial.println("GPS data valid");
          #endif

          // set time for now()
          setTime(gps.time.hour(), gps.time.minute(), gps.time.second(), gps.date.day(), gps.date.month(), gps.date.year());

          // check for stale GPS timestamps
          if (now() > prevTimeStamp)
          {
            prevTimeStamp = now();
            gpsDisplayFail = false;

            // resync RTC every successful GPS read
            rtc.setDate(gps.date.day(), gps.date.month(), gps.date.year() % 100);
            rtc.setTime(gps.time.hour(), gps.time.minute(), gps.time.second());

            if(!rtcSet) rtcSet = true;
            break;
          }
          #ifdef DEBUG_PRINT
          else
          {
            Serial.println("Stale timestamp, continuing GPS read");
          }
          #endif
        
        }

        // make GPS reads interruptible by the button being pressed
        if (encLeftButtonFlag)
        {
          // Put GPS to sleep
          if (gpsAwake)
          {
            toggleGps();
          }
          // TODO: test if this returns to the proper page
          return;
        }
      }
      
      #ifdef DEBUG_PRINT
      unsigned long postRead = millis();
      Serial.print("GPS read took: "); Serial.print(postRead - preRead); Serial.println(" ms");
      #endif

      // store UTC time
      utcTime = now();

      latitude = gps.location.lat();
      longitude = gps.location.lng();
      altitude = gps.altitude.meters();

      // put GPS to sleep
      if (gpsAwake)
      {
        toggleGps();
        if (timeoutFlag)
        {
          delay(500); // add extra delay after timeout to make sure sleep command is properly interpreted before next read
        }
      }

      // store time from GPS
      utcYear = year(utcTime);
      utcMonth = month(utcTime);
      utcDay = day(utcTime);
      utcHour = hour(utcTime);
      utcMinute = minute(utcTime);
      utcSecond = second(utcTime);
    }
    
    if(manualTimeEntry || timeoutFlag) // if manual time entry or GPS timed out, overwrite timestamp with RTC values
    {
      utcYear = rtc.getYear() + 2000; // RTC year is stored as an offset from 2000
      utcMonth = rtc.getMonth();
      utcDay = rtc.getDay();
      utcHour = rtc.getHours();
      utcMinute = rtc.getMinutes();
      utcSecond = rtc.getSeconds();
    }

    metaFile = SD.open(metaFileName, FILE_WRITE);
    if(metaFile)
    {
      Serial.print("# ");
      Serial.print(msTimer);
      Serial.print(',');
      Serial.print(utcYear);
      Serial.print('-');
      Serial.print(utcMonth);
      Serial.print('-');
      Serial.print(utcDay);
      Serial.print('T');
      if(utcHour < 10) Serial.print('0');
      Serial.print(utcHour);
      Serial.print(':') ;
      if(utcMinute < 10) Serial.print('0');
      Serial.print(utcMinute);
      Serial.print(':');
      if(utcSecond < 10) Serial.print('0');
      Serial.print(utcSecond);
      Serial.print("+00:00");

      metaFile.print(msTimer);
      metaFile.print(',');
      metaFile.print(utcYear);
      metaFile.print('-');
      metaFile.print(utcMonth);
      metaFile.print('-');
      metaFile.print(utcDay);
      metaFile.print('T');
      if(utcHour < 10) metaFile.print('0');
      metaFile.print(utcHour);
      metaFile.print(':') ;
      if(utcMinute < 10) metaFile.print('0');
      metaFile.print(utcMinute);
      metaFile.print(':');
      if(utcSecond < 10) metaFile.print('0');
      metaFile.print(utcSecond);
      metaFile.print("+00:00");

      // do not report lat, long, alt
      if(manualTimeEntry || timeoutFlag)
      {
        Serial.print(",,,");
        metaFile.print(",,,");
      }
      else
      {
        Serial.print(',');
        Serial.print(latitude);
        Serial.print(',');
        Serial.print(longitude);
        Serial.print(',');
        Serial.print(altitude);

        metaFile.print(',');
        metaFile.print(latitude);
        metaFile.print(',');
        metaFile.print(longitude);
        metaFile.print(',');
        metaFile.print(altitude);
      }

      Serial.print(',');
      Serial.print(temp.integral); Serial.print('.'); Serial.print(temp.fractional);
      Serial.print(',');
      Serial.print(press.integral); Serial.print('.'); Serial.println(press.fractional);

      metaFile.print(',');
      metaFile.print(temp.integral); metaFile.print('.'); metaFile.print(temp.fractional);
      metaFile.print(',');
      metaFile.print(press.integral); metaFile.print('.'); metaFile.print(press.fractional);
      metaFile.print('\n');
      metaFile.close();
    }
    else
    {
      #ifdef DEBUG_PRINT
      Serial.println("Couldn't open gps file");
      #endif
      display.clearDisplay();
      updateDisplay("Couldn't open GPS file", 40, false);
      display.display();
      delay(500);
    }
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

    // Get timestamp from RTC
    utcYear = rtc.getYear() + 2000; // RTC year is stored as an offset from 2000
    utcMonth = rtc.getMonth();
    utcDay = rtc.getDay();
    utcHour = rtc.getHours();
    utcMinute = rtc.getMinutes();
    utcSecond = rtc.getSeconds();

    // Display data in the serial monitor
    
    Serial.print(msTimer);
    Serial.print(',');
    Serial.print(utcYear);
    Serial.print('-');
    Serial.print(utcMonth);
    Serial.print('-');
    Serial.print(utcDay);
    Serial.print('T');
    if(utcHour < 10) Serial.print('0');
    Serial.print(utcHour);
    Serial.print(':') ;
    if(utcMinute < 10) Serial.print('0');
    Serial.print(utcMinute);
    Serial.print(':');
    if(utcSecond < 10) Serial.print('0');
    Serial.print(utcSecond);
    Serial.print("+00:00");
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

    dataFile.print(msTimer);
    dataFile.print(',');
    dataFile.print(utcYear);
    dataFile.print('-');
    dataFile.print(utcMonth);
    dataFile.print('-');
    dataFile.print(utcDay);
    dataFile.print('T');
    if(utcHour < 10) dataFile.print('0');
    dataFile.print(utcHour);
    dataFile.print(':') ;
    if(utcMinute < 10) dataFile.print('0');
    dataFile.print(utcMinute);
    dataFile.print(':');
    if(utcSecond < 10) dataFile.print('0');
    dataFile.print(utcSecond);
    dataFile.print("+00:00");
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
    dataFile.print(count_10p0um); // >10.0um
    dataFile.print('\n');
    dataFile.close();

    ledFlag = true;
  }
  else
  {
    #ifdef DEBUG_PRINT
    Serial.println("Couldn't open file");
    #endif
    display.clearDisplay();
    updateDisplay("Couldn't open data file", 40, false);
    display.display();
    delay(500);
  }

  if(timestampFlag)
  {
    timestampFlag = false;
  }

}

// upload SD card data over serial port
void uploadSerial(char * fileName)
{
  // disable button ISRs
  encRightButtonISREn = false; 
  encLeftButtonISREn = false;
  uint8_t buffer[512] = {0}; // buffer to read/write data 512 bytes at a time
  uint16_t writeLen = sizeof(buffer);
  display.clearDisplay();
  updateDisplay("Uploading ", 40, false);
  updateDisplay(fileName, 48, false);
  updateDisplay("via serial port", 56, false);
  display.display();

  char fileNameExtension[30];
  strcpy(fileNameExtension, fileName);
  strcat(fileNameExtension, ".txt");

  #ifdef DEBUG_PRINT
  Serial.println("Serial upload initiated");
  Serial.print("Uploading: ");
  #endif
  // Send file name as first line to be captured, download scripts will use this to name the file
  Serial.print(fileNameExtension); Serial.print('\n');

  File file = SD.open(fileNameExtension, FILE_READ);
  while(file.available())
  {
    if (file.available() > sizeof(buffer))
    {
      writeLen = sizeof(buffer);
      file.read(buffer, sizeof(buffer));
    }
    else
    {
      writeLen = file.available();  
      file.read(buffer, file.available());
    }
  
    Serial.write(buffer, writeLen);
    memset(buffer, 0, sizeof(buffer));
  }
  file.close();
  delay(2500);
  
  encRightButtonISREn = true;
  encLeftButtonISREn = true;
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
      // Serial.println("GPS data successfully encoded");
      #endif
    }
  }
}

// toggle GPS between sleep and wake
void toggleGps()
{
  sendGpsCommand("051,0");
  delay(100);
  gpsAwake = !gpsAwake;
  #ifdef DEBUG_PRINT
  if (gpsAwake)
  {
    Serial.println("GPS awake");
  }
  else
  {
    Serial.println("GPS asleep");
  }
  #endif
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

// Create two data files, one for GPS and one for dust data
void createDataFiles()
{
  // reset current file names
  memset(dataFileName, 0, sizeof(dataFileName));
  memset(metaFileName, 0, sizeof(metaFileName));

  int year;
  int month;
  int day;
  int hour;
  int minutes;
  int seconds;

  // strings to concatenate into file names
  char yearStr[3];
  char monthStr[3];
  char dayStr[3];
  char hourStr[3];
  char minutesStr[3];
  char secondsStr[3];
  char baseString[15]; // YYMMDD_HHMMSS_

  if(!manualTimeEntry)
  {
    // get time stamp from GPS, set RTC
    unsigned long gpsReadCurMillis;
    unsigned long gpsReadStartMillis = millis();
    unsigned long gpsTimeoutMillis = GPS_FIRST_TIMEOUT;

      display.clearDisplay();
      display.drawLine(0, display.height()-10, display.width()-1, display.height()-10, SSD1327_WHITE);
      display.drawLine(display.width()/2 - 1, display.height()-10, display.width()/2 - 1, display.height()-1, SSD1327_WHITE);
      display.setTextColor(SSD1327_WHITE);
      display.setCursor(10, display.height()-8);
      display.print("Back ");
      updateDisplay("Reading GPS...", 40, false);
      updateDisplay("(First GPS read", 56, false);
      updateDisplay("may take a", 64, false);
      updateDisplay("few minutes)", 72, false);
      display.display();

    if(!gpsAwake)
    {
      toggleGps();
    }

    while (true)
    {
      gpsReadCurMillis = millis();

      readGps();

      if (gpsReadCurMillis - gpsReadStartMillis >= gpsTimeoutMillis)
      {
        // timeoutFlag = true;
        gpsDisplayFail = true;
        #ifdef DEBUG_PRINT 
        Serial.println("GPS timeout");
        #endif
        display.clearDisplay();
        display.drawLine(0, display.height()-10, display.width()-1, display.height()-10, SSD1327_WHITE);
        display.drawLine(display.width()/2 - 1, display.height()-10, display.width()/2 - 1, display.height()-1, SSD1327_WHITE);
        display.setTextColor(SSD1327_WHITE);
        display.setCursor(10, display.height()-8);
        display.print("Back ");
        updateDisplay("GPS read failed", 40, false);
        updateDisplay("Please enter time", 56, false);
        updateDisplay("manually", 64, false);
        display.display();
        // turn off GPS
        if(gpsAwake)
        {
          toggleGps();
        }
        while(!encLeftButtonFlag); // wait for back button to be pressed
        encLeftButtonFlag = false;
        encLeftButtonISREn = true;
        prevState = state;
        state = 0;
        page = 1;
        return;
      }

      if (gps.date.isValid() && gps.time.isValid() && gps.location.isValid() && gps.altitude.isValid() && gps.date.year() == CUR_YEAR)
      {
        #ifdef DEBUG_PRINT
        Serial.println("GPS data valid");
        #endif

        // set time for now()
        // setTime(gps.time.hour(), gps.time.minute(), gps.time.second(), gps.date.day(), gps.date.month(), gps.date.year());
        // prevTimeStamp = now();

        // GPS data is valid, set RTC
        rtc.setDay(gps.date.day());
        rtc.setMonth(gps.date.month());
        rtc.setYear(gps.date.year() % 100);
        rtc.setHours(gps.time.hour());
        rtc.setMinutes(gps.time.minute());
        rtc.setSeconds(gps.time.second());

        if(!rtcSet) rtcSet = true;

        break;
      
      }

      // make GPS reads interruptible by the button being pressed
      if (encLeftButtonFlag)
      {
        // Put GPS to sleep
        if (gpsAwake)
        {
          toggleGps();
        }
        page = 1; // go back to time entry choice menu
        prevState = state;
        state = 0;
        encLeftButtonFlag = false;
        encLeftButtonISREn = true;
        return;
      }
    }

  }

  // put GPS to sleep if it's woken up
  if(gpsAwake)
  {
    toggleGps();
  }

  // RTC clock should be set (whether manually or with GPS)
  year = rtc.getYear();
  month = rtc.getMonth();
  day = rtc.getDay();
  hour = rtc.getHours();
  minutes = rtc.getMinutes();
  seconds = rtc.getSeconds();

  itoa(year, yearStr, 10);
  itoa(month, monthStr, 10);
  itoa(day, dayStr, 10);
  itoa(hour, hourStr, 10);
  itoa(minutes, minutesStr, 10);
  itoa(seconds, secondsStr, 10);

  #ifdef DEBUG_PRINT
  Serial.print(monthStr);
  Serial.print('/');
  Serial.print(dayStr);
  Serial.print('/');
  Serial.print(yearStr);
  Serial.print(' ');
  Serial.print(hourStr);
  Serial.print(':');
  Serial.print(minutesStr);
  Serial.print(':');
  Serial.println(secondsStr);
  #endif

  strcpy(baseString, yearStr);
  if(month < 10) strcat(baseString, "0");
  strcat(baseString, monthStr);
  if(day < 10) strcat(baseString, "0");
  strcat(baseString, dayStr);
  strcat(baseString, "_");
  if(hour < 10) strcat(baseString, "0");
  strcat(baseString, hourStr);
  if(minutes < 10) strcat(baseString, "0");
  strcat(baseString, minutesStr);
  if(seconds < 10) strcat(baseString, "0");
  strcat(baseString, secondsStr);

  strcpy(dataFileName, baseString);
  strcpy(metaFileName, baseString);
  strcat(dataFileName, "_data.txt");
  strcat(metaFileName, "_meta.txt");

  #ifdef DEBUG_PRINT
  Serial.print("dataFileName: ");
  Serial.println(dataFileName);
  Serial.print("metaFileName: ");
  Serial.println(metaFileName);
  #endif
  
  File newFile;

  // Create column headers for new data file
  if(!SD.exists(dataFileName))
  {
    #ifdef DEBUG_PRINT
    Serial.println("Writing column headers for new data file");
    #endif
    newFile = SD.open(dataFileName, FILE_WRITE);
    if(newFile)
    {
      #ifdef DEBUG_PRINT
      Serial.print("ms,UTC_timestamp,PM1.0,PM2.5,PM10.0,0.3um,0.5um,1.0um,2.5um,5.0um,10.0um");
      #endif
      newFile.print("ms,UTC_timestamp,PM1.0,PM2.5,PM10.0,0.3um,0.5um,1.0um,2.5um,5.0um,10.0um\n");
    }
    else 
    {
      #ifdef DEBUG_PRINT
      Serial.println("Couldn't open data file");
      #endif
    }
    newFile.close();
  }

  // Create column headers for new data file
  if(!SD.exists(metaFileName))
  {
    #ifdef DEBUG_PRINT
    Serial.println("Writing column headers for new data file");
    #endif
    newFile = SD.open(metaFileName, FILE_WRITE);
    if(newFile)
    {
      #ifdef DEBUG_PRINT
      Serial.print("ms,UTC_timestamp,PM1.0,PM2.5,PM10.0,0.3um,0.5um,1.0um,2.5um,5.0um,10.0um");
      #endif
      newFile.print("ms,UTC_timestamp,latitude,longitude,altitude,temperature,pressure\n");
    }
    else 
    {
      #ifdef DEBUG_PRINT
      Serial.println("Couldn't open data file");
      #endif
    }
    newFile.close();
  }

  // collect data and go to data collection screen
  prevState = state;
  state = 2;
  page = 5;
  firstMeasurementFlag = true;
}

// Update current menu selection based on encoders
void updateMenuSelection()
{
  long encRightPosition = encRight.read();
  long encLeftPosition = encLeft.read();
  #ifdef DEBUG_PRINT
  // Serial.print("Right encoder position: ");
  // Serial.println(encRightPosition);
  #endif
  if (encRightPosition > encRightOldPosition + 2) // clockwise, go down
  {
    #ifdef DEBUG_PRINT
    Serial.println("Right knob turned cw");
    #endif
    encRightOldPosition = encRightPosition;

    if(curMillis >= prevMenuMillis + MENU_UPDATE_TIME) // only update menu selection every 100ms
    {
      prevMenuMillis = curMillis;
      prevVertMenuSelection = currentVertMenuSelection;
      currentVertMenuSelection++;
      switch (page)
      {
        case 0: case 1: // initial two menus
          if(currentVertMenuSelection > 1) currentVertMenuSelection = 1; // only two choices on these pages
          break;
        case 2: // entering date
          if(currentHoriMenuSelection == 0) // month
          {
            if(currentVertMenuSelection > 11) currentVertMenuSelection = 0;
            manualMonth = currentVertMenuSelection + 1;
          }
          else if(currentHoriMenuSelection == 1) // day
          {
            switch(manualMonth)
            {
              case 4: case 6: case 9: case 11: // Apr, Jun, Sept, Nov
                if(currentVertMenuSelection > 29) currentVertMenuSelection = 0;
                break;
              case 1: case 3: case 5: case 7: case 8: case 10: case 12: // Jan, Mar, May, Jul, Aug, Oct, Dec
                if(currentVertMenuSelection > 30) currentVertMenuSelection = 0;
                break;
              case 2: // February
                if(manualYear % 4 == 0) 
                {
                  if(currentVertMenuSelection > 28) currentVertMenuSelection = 0;
                }
                else
                {
                  if(currentVertMenuSelection > 27) currentVertMenuSelection = 0;
                }
                break;
            }
            manualDay = currentVertMenuSelection + 1;
          }
          else if(currentHoriMenuSelection == 2) // year
          {
            if(currentVertMenuSelection > 2099) 
            {
              currentVertMenuSelection = CUR_YEAR;
            }
            else if(currentVertMenuSelection < CUR_YEAR)
            {
              currentVertMenuSelection = CUR_YEAR;
            }
            manualYear = currentVertMenuSelection;
          }
          break;
        case 3: // entering time
          if(currentHoriMenuSelection == 0) // hour
          {
            if(currentVertMenuSelection > 23) currentVertMenuSelection = 0;
            manualHour = currentVertMenuSelection;
          }
          else if(currentHoriMenuSelection == 1) // minute
          {
            if(currentVertMenuSelection > 59)
            {
              currentVertMenuSelection = 0;
              manualHour++;
              if(manualHour > 23) manualHour = 0;
            }
            manualMinute = currentVertMenuSelection;
          }
          break;
        case 4: // selecting file from SD card
          if(currentVertMenuSelection > fileCount - 1) currentVertMenuSelection = fileCount - 1;
          if(currentVertMenuSelection % 12 == 0 && currentVertMenuSelection != 0 && currentVertMenuSelection != prevVertMenuSelection)
          {
            scroll++;
          }
          break;
      }
      #ifdef DEBUG_PRINT
      Serial.print("Current vert menu selection: ");
      Serial.println(currentVertMenuSelection);
      #endif
    }
  }
  else if (encRightPosition < encRightOldPosition - 2) // counterclockwise, go up
  {
    #ifdef DEBUG_PRINT
    Serial.println("Right knob turned ccw");
    #endif
    encRightOldPosition = encRightPosition;

    if(curMillis >= prevMenuMillis + MENU_UPDATE_TIME) // only update menu selection every 100ms
    {
      prevMenuMillis = curMillis;
      prevVertMenuSelection = currentVertMenuSelection;
      currentVertMenuSelection--;
      switch(page)
      {
        case 0: case 1:
          if (currentVertMenuSelection < 0) currentVertMenuSelection = 0; // stay at top of menu
          break;
        case 2: // entering date
          if(currentHoriMenuSelection == 0) // entering month
          {
            if(currentVertMenuSelection < 0) currentVertMenuSelection = 11; // wrap around
            manualMonth = currentVertMenuSelection + 1;
          }
          else if(currentHoriMenuSelection == 1) // entering day
          {
            if(currentVertMenuSelection < 0)
            {
              switch(manualMonth)
              {
                case 4: case 6: case 9: case 11: // April, June, September, November
                  currentVertMenuSelection = 29;
                  break;
                case 1: case 3: case 5: case 7: case 8: case 10: case 12: // Jan, Mar, May, Jul, Aug, Oct, Dec
                  currentVertMenuSelection = 30;
                  break;
                case 2:
                  if(manualYear % 4 == 0) // leap year
                  {
                    currentVertMenuSelection = 28;
                  }
                  else
                  {
                    currentVertMenuSelection = 27;
                  }
              }
            }
            manualDay = currentVertMenuSelection + 1;
          }
          else if(currentHoriMenuSelection == 2) // entering year
          {
            if(currentVertMenuSelection > 2099)
            {
              currentVertMenuSelection = CUR_YEAR;
            }
            else if(currentVertMenuSelection < CUR_YEAR)
            {
              currentVertMenuSelection = CUR_YEAR;
            }
            manualYear = currentVertMenuSelection;
          }
          break;
        case 3: // entering time
          if(currentHoriMenuSelection == 0) // hour
          {
            if(currentVertMenuSelection < 0) currentVertMenuSelection = 23;
            manualHour = currentVertMenuSelection;
          }
          else if(currentHoriMenuSelection == 1) // minute
          {
            if(currentVertMenuSelection < 0)
            {
              currentVertMenuSelection = 59;
              manualHour--;
              if(manualHour > 200) manualHour = 23;
            }
            manualMinute = currentVertMenuSelection;
          }
          break;
        case 4:
          if (currentVertMenuSelection < 0) currentVertMenuSelection = 0; // stay at top of menu
          if(currentVertMenuSelection % 12 == 11)
          {
            scroll--;
            if(scroll < 0) scroll = 0;
          }
      }
      #ifdef DEBUG_PRINT
      Serial.print("Current vert menu selection: ");
      Serial.println(currentVertMenuSelection);
      #endif
    }
  }

  if(encLeftPosition > encLeftOldPosition + 2)
  {
    #ifdef DEBUG_PRINT
    Serial.println("Left knob turned cw");
    Serial.println(encLeftPosition);
    #endif
    encLeftOldPosition = encLeftPosition;

    if(curMillis >= prevMenuMillis + MENU_UPDATE_TIME)
    {
      prevMenuMillis = curMillis;
      if(page == 2 || page == 3) // date or time entry
      {
        currentHoriMenuSelection++;
        if(page == 2) // date entry
        {
          if(currentHoriMenuSelection > 2) currentHoriMenuSelection = 2;

          if(currentHoriMenuSelection == 0) // month
          {
            currentVertMenuSelection = manualMonth - 1;
          }
          else if(currentHoriMenuSelection == 1) // day
          {
            currentVertMenuSelection = manualDay - 1;
          }
          else if(currentHoriMenuSelection == 2) // year
          {
            currentVertMenuSelection = manualYear;
          }
        }
        else if(page == 3)
        {
          if(currentHoriMenuSelection > 1) currentHoriMenuSelection = 1;

          if(currentHoriMenuSelection == 0) // hour
          {
            currentVertMenuSelection = manualHour;
          }
          else if(currentHoriMenuSelection == 1) // minute
          {
            currentVertMenuSelection = manualMinute;
          }
        }
      }
      #ifdef DEBUG_PRINT
      Serial.print("Current hori menu selection: ");
      Serial.println(currentHoriMenuSelection);
      #endif
    }
  }
  else if(encLeftPosition < encLeftOldPosition - 2)
  {
    #ifdef DEBUG_PRINT
    Serial.println("Left knob turned ccw");
    Serial.println(encLeftPosition);
    #endif
    encLeftOldPosition = encLeftPosition;

    if(curMillis >= prevMenuMillis + MENU_UPDATE_TIME)
    {
      currentHoriMenuSelection--;
      if(page == 2) // date entry
      {
        if(currentHoriMenuSelection < 0) currentHoriMenuSelection = 0;

        if(currentHoriMenuSelection == 0) // month
        {
          currentVertMenuSelection = manualMonth - 1;
        }
        else if(currentHoriMenuSelection == 1) // day
        {
          currentVertMenuSelection = manualDay - 1;
        }
        else if(currentHoriMenuSelection == 2) // year
        {
          currentVertMenuSelection = manualYear;
        }
      }
      else if(page == 3) // time entry
      {
        if(currentHoriMenuSelection < 0) currentHoriMenuSelection = 0;

        if(currentHoriMenuSelection == 0) // hour
        {
          currentVertMenuSelection = manualHour;
        }
        else if(currentHoriMenuSelection == 1) // minute
        {
          currentVertMenuSelection = manualMinute;
        }
      }
      #ifdef DEBUG_PRINT
      Serial.print("Current hori menu selection: ");
      Serial.println(currentHoriMenuSelection);
      #endif
    }
  }
}

// function for displaying various pages/menus
void displayPage(uint8_t page)
{
  // On all pages add "select" and "back" indicators on the bottom of the screen
  // On data collection page, only show "back"
  display.clearDisplay();
  display.drawLine(0, display.height()-10, display.width()-1, display.height()-10, SSD1327_WHITE);
  display.drawLine(display.width()/2 - 1, display.height()-10, display.width()/2 - 1, display.height()-1, SSD1327_WHITE);
  // display.drawLine((2*display.width()/3)-1, display.height()-10, (2*display.width()/3)-1, display.height()-1, SSD1327_WHITE);
  display.setTextColor(SSD1327_WHITE);
  display.setCursor(10, display.height()-8);
  display.print("Back ");
  if(page == 2 || page == 3) // only the date and time page uses the left knob for left-right
  {
    display.cp437(true);
    display.print("\x11\x10");
    display.cp437(false);
  }
  if (page != 5) // no select button on data collection screen
  {
    display.setCursor((display.width()/2) + 5, display.height()-8);
    display.cp437(true);
    display.print("\x1e\x1f");
    display.cp437(false);
    display.print(" Select");
  }

  switch(page)
  {
    case(0): // Initial menu
    {
      if (currentVertMenuSelection == 0)
      {
        updateDisplay("Start data collection\n", 0, true);
        updateDisplay("Upload data", 8, false);
      }
      else if (currentVertMenuSelection == 1)
      {
        updateDisplay("Start data collection\n", 0, false);
        updateDisplay("Upload data", 8, true);
      }
      break;
    }
    case(1): // Time entry method menu
    {
      display.drawLine(0, 10, display.width()-1, 10, SSD1327_WHITE);
      updateDisplay("Timestamp method?", 0, false);
      if (currentVertMenuSelection == 0) 
      {
        updateDisplay("Auto (GPS)\n", 12, true);
        updateDisplay("Manual", 20, false);
      }
      else if (currentVertMenuSelection == 1)
      {
        updateDisplay("Auto (GPS)\n", 12, false);
        updateDisplay("Manual", 20, true);
      }
      break;
    }
    case(2): // Date entry
    {
      display.drawLine(0, 10, display.width()-1, 10, SSD1327_WHITE);
      updateDisplay("Enter date", 0, false);  

      char displayMonth[3];
      char displayDay[3];
      char displayYear[5];

      itoa(manualMonth, displayMonth, 10);
      itoa(manualDay, displayDay, 10);
      itoa(manualYear, displayYear, 10);

      display.setTextSize(2);
      display.setCursor(0, 56);

      if(currentHoriMenuSelection == 0) display.setTextColor(SSD1327_BLACK, SSD1327_WHITE);
      if(manualMonth < 10) display.print('0');
      display.print(manualMonth);

      display.setTextColor(SSD1327_WHITE);
      display.print('/');

      if(currentHoriMenuSelection == 1) display.setTextColor(SSD1327_BLACK, SSD1327_WHITE);
      if(manualDay < 10) display.print('0');
      display.print(manualDay);

      display.setTextColor(SSD1327_WHITE);
      display.print('/');

      if(currentHoriMenuSelection == 2) display.setTextColor(SSD1327_BLACK, SSD1327_WHITE);
      display.print(manualYear);

      display.setTextColor(SSD1327_WHITE);
      display.setTextSize(1);
      break;
    }
    case(3): // Time entry
    {
      display.drawLine(0, 10, display.width()-1, 10, SSD1327_WHITE);
      updateDisplay("Enter time (UTC)", 0, false);  
  
      char displayHour[3];
      char displayMinute[3];

      itoa(manualHour, displayHour, 10);
      itoa(manualMinute, displayMinute, 10);

      display.setTextSize(2);
      display.setCursor(0, 56);

      if(currentHoriMenuSelection == 0) display.setTextColor(SSD1327_BLACK, SSD1327_WHITE);
      if(manualHour < 10) display.print('0');
      display.print(manualHour);

      display.setTextColor(SSD1327_WHITE);
      display.print(':');

      if(currentHoriMenuSelection == 1) display.setTextColor(SSD1327_BLACK, SSD1327_WHITE);
      if(manualMinute < 10) display.print('0');
      display.print(manualMinute);

      display.setTextColor(SSD1327_WHITE);
      display.setTextSize(1);

      break;
    }
    case(4): // Viewing list of files on SD card
    {
      char allFiles[fileCount][30];
      memcpy(allFiles, fileList, sizeof(allFiles));

      char screenFiles[12][30]; // files being displayed on screen

      // #ifdef DEBUG_PRINT
      // Serial.println("List of files on SD found in displayPage:");
      // for(int i = 0; i < fileCount; ++i)
      // {
      //   Serial.println(allFiles[i]);
      // }
      // Serial.println();
      // #endif

      display.drawLine(0, 10, display.width()-1, 10, SSD1327_WHITE);
      updateDisplay("Select a file", 0, false);  

      uint8_t numFilesToDisplay = 0;

      // copy next 12 names from allFiles into screenFiles, or fewer if there are fewer than 12 left
      if(fileCount - scroll*12 >= 12)
      {
        memcpy(screenFiles, allFiles[scroll*12], sizeof(screenFiles));
        numFilesToDisplay = 12;
      }
      else if(fileCount - scroll*12 < 12)
      {
        numFilesToDisplay = fileCount - scroll*12;
        memcpy(screenFiles, allFiles[scroll*12], numFilesToDisplay*30); // 30 bytes per file name
      }

      for(uint8_t i = 0; i < numFilesToDisplay; i++)
      {
        if(scroll == 0)
        {
          if(currentVertMenuSelection == i) updateDisplay(screenFiles[i], 12 + i*8, true);
          else updateDisplay(screenFiles[i], 12 + i*8, false);
        }
        else
        {
          if(currentVertMenuSelection == i + 12*scroll) updateDisplay(screenFiles[i], 12 + i*8, true);
          else updateDisplay(screenFiles[i], 12 + i*8, false);
        }          
        
      }
      break;
    }
    case(5): // Data collection
    {
      // uint16_t PM1p0_std = dustSensor.data.PM1p0_std;
      // uint16_t PM2p5_std = dustSensor.data.PM2p5_std;
      // uint16_t PM10p0_std = dustSensor.data.PM10p0_std;
      // uint16_t PM1p0_atm = dustSensor.data.PM1p0_atm;
      // uint16_t PM2p5_atm = dustSensor.data.PM2p5_atm;
      // uint16_t PM10p0_atm = dustSensor.data.PM10p0_atm;
      uint16_t count_0p3um = dustSensor.data.count_0p3um;
      // uint16_t count_0p5um = dustSensor.data.count_0p5um;
      // uint16_t count_1p0um = dustSensor.data.count_1p0um;
      // uint16_t count_2p5um = dustSensor.data.count_2p5um;
      // uint16_t count_5p0um = dustSensor.data.count_5p0um;
      // uint16_t count_10p0um = dustSensor.data.count_10p0um;

      char timeText[50];
      // char pm1p0Text[10];
      // char pm2p5Text[10];
      // char pm10p0Text[10];
      char hourText[10];
      char minuteText[10];
      char monthText[10];
      char dayText[10];
      char yearText[10];

      // itoa(PM1p0_atm, pm1p0Text, 10);
      // itoa(PM2p5_atm, pm2p5Text, 10);
      // itoa(PM10p0_atm, pm10p0Text, 10);

      itoa(utcHour, hourText, 10);
      itoa(utcMinute, minuteText, 10);
      itoa(utcMonth, monthText, 10);
      itoa(utcDay, dayText, 10);
      itoa(utcYear, yearText, 10);

      display.setTextSize(2);
      display.setCursor(0, 20);
      display.print(">0.3um:");
      display.setCursor(0, 48);
      display.print(count_0p3um);
      display.setCursor(0, 62);
      display.print("count/0.1L");
      display.setTextSize(1);

      // strcpy(displayText, "PM1.0:  ");
      // strcat(displayText, pm1p0Text);
      // strcat(displayText, " ug/m3");
      // updateDisplay(displayText, 24, false);

      // strcpy(displayText, "PM2.5:  ");
      // strcat(displayText, pm2p5Text);
      // strcat(displayText, " ug/m3");
      // updateDisplay(displayText, 32, false);

      // strcpy(displayText, "PM10.0: ");
      // strcat(displayText, pm10p0Text);
      // strcat(displayText, " ug/m3");
      // updateDisplay(displayText, 40, false);

      strcpy(timeText, monthText);
      strcat(timeText, "/");
      strcat(timeText, dayText);
      strcat(timeText, "/");
      strcat(timeText, yearText);
      strcat(timeText, " ");
      if(utcHour < 10)
      {
        strcat(timeText, "0");
      }
      strcat(timeText, hourText);
      strcat(timeText, ":");
      if(utcMinute < 10)
      {
        strcat(timeText, "0");
      }
      strcat(timeText, minuteText);
      if(gpsDisplayFail || manualTimeEntry) strcat(timeText, " (RTC)");
      else strcat(timeText, " (GPS)");
      updateDisplay(timeText, 108, false);
      break;
    }
  }
  display.display();
}

// function for displaying characters to OLED 
void updateDisplay(char* text, uint8_t height, bool bg)
{
  // if(clear) display.clearDisplay();

  display.setTextSize(1);

  if(bg) 
  {
    display.setTextColor(SSD1327_BLACK, SSD1327_WHITE);
  }
  else
  { 
    display.setTextColor(SSD1327_WHITE);
  }
  display.setCursor(0, height);

  display.print(text);

  // if(send) display.display();

}

int cmpstr(void const *a, void const *b)
{
  char const *aa = (char const *)a;
  char const *bb = (char const *)b;

  return -1*strcmp(aa, bb);
}

void getFileList()
{
  // First count files on SD card
          #ifdef DEBUG_PRINT
          Serial.println("\nCounting files on SD card");
          #endif
          File file;
          File root;
          fileCount = 0;
          if(!root.open("/"))
          {
            #ifdef DEBUG_PRINT
            Serial.println("Error opening root");
            #endif
            // TODO: figure out how to handle this error
          }
          while(file.openNext(&root, O_RDONLY))
          {
            if(!file.isHidden())
            {
              fileCount++;
              #ifdef DEBUG_PRINT
              file.printName(&Serial);
              Serial.print('\t');
              Serial.println(fileCount);
              #endif
            }
            file.close();
          }
          root.rewind();

          #ifdef DEBUG_PRINT
          Serial.print("\nFile count: ");
          Serial.println(fileCount);
          #endif

          // now create an array of file names
          char filesOnSd[fileCount][30]; // Each file name should be at most 22 characters long
          uint32_t curFile = 0;
          while(file.openNext(&root, O_RDONLY))
          {
            if(!file.isHidden())
            {
              char fileName[30] = {0};
              file.getName(fileName, sizeof(fileName));
              char shortenedFileName[30] = {0};
              memcpy(shortenedFileName, fileName, strlen(fileName) - 4); // everything but the file extension ".txt" (to fit on screen)
              strcpy(filesOnSd[curFile], shortenedFileName);
              curFile++;
            }
            file.close();
          }
          root.rewind();

          #ifdef DEBUG_PRINT
          Serial.println("\nList of files created (pre-sort):");
          for(int i = 0; i < fileCount; ++i)
          {
            Serial.println(filesOnSd[i]);
          }
          #endif

          // Sort filesOnSd alphabetically
          qsort(filesOnSd, fileCount, 30, cmpstr);

          #ifdef DEBUG_PRINT
          Serial.println("\nList of files created:");
          for(int i = 0; i < fileCount; ++i)
          {
            Serial.println(filesOnSd[i]);
          }
          #endif

          root.close();

          // copy contents fo fileList memory location
          // free()'d when the upload menu is left with the back button
          // free()'d when file list is sent over serial
          fileList = (char *)malloc(sizeof(filesOnSd));
          memcpy(fileList, filesOnSd, sizeof(filesOnSd));
}

/*
 * Functions and variables below this point are used for updating to ThingSpeak, not used in this version
 */

/*

WiFiClient client;
const char server[] = "api.thingspeak.com";

char tspeak_buf[5000]; // buffer for storing multiple rows of data from the CSV for ThingSpeak bulk updates
char status_buf[20]; // buffer for status text
uint8_t colPositions[17] = {0}; // array to store indices of commas in sd_buf, indicating column delineations
unsigned long prevFileSize = 0; // last recorded file size
unsigned long bytesLeft = 0; // bytes left in file to update to ThingSpeak

bool firstLineDone = false; // flag for first line (titles) having been read
uint32_t lastLinePosition = 0;
char sd_buf[200]; // buffer to store single SD card line

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

*/