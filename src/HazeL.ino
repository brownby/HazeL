/*
 * HazeL
 * Benjamin Y. Brown
 * 
 * Creates a file called "data.txt" on the SD card 
 * Writes PM and raw concentration data every 2.5s to the SD card
 *
 * When button is pressed, HazeL will upload the contents of the SD card through the serial port
 * To upload the entirety of data.txt, pull SWITCH_PIN high
 * To upload incrementally (only upload data collected since the last incremental upload), pull SWITCH_PIN low
 * 
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
#define BUTTON_PIN A2 // pin for button that triggers data uploads over USB
#define SWITCH_PIN A3 // pin for switch that sets upload mode (bulk or incremental)
#define SD_CS_PIN 4 // CS pin of SD card, 4 on SD MKR proto shield
#define CUR_YEAR 2021 // for GPS first fix error checking
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3D
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 128
#define ENC_RIGHT_BUTTON A1
#define ENC_LEFT_BUTTON 7
#define MENU_UPDATE_TIME 100 // milliseconds between menu updates
#define DEBUG_PRINT

HM3301 dustSensor;
BMP280 TPSensor;

SdFat SD;
File dataFile;
char dataFileName[] = "data.txt";

TinyGPSPlus gps;
bool firstGpsRead = false;
bool gpsFlag = false;
bool gpsAwake = true;
bool gpsDisplayFail = false;

int localYear;
int localMonth;
int localDay;
int localHour;
int localMinute;
int localSecond;
double latitude;
double longitude;
double altitude;

time_t prevTimeStamp = 0;
uint8_t manualMonth = 1;
uint8_t manualDay = 1;
uint16_t manualYear = CUR_YEAR;
uint8_t manualHour = 0;
uint8_t manualMinute = 0;
int8_t manualTimeZone = 0; // Hours behind or ahead of UTC
bool manualTimeEntry = false; // false means use GPS

Adafruit_SSD1327 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Encoder encRight(4, 5);
Encoder encLeft(0, 1);

long encRightOldPosition = 0;
long encLeftOldPosition = 0;

unsigned long prevSampMillis = 0;
unsigned long prevLedMillis = 0;
unsigned long prevGpsMillis = 0;
unsigned long prevMenuMillis = 0;
unsigned long curMillis;

volatile bool encRightButtonFlag = false;
volatile bool encRightButtonISREn = false;
volatile bool encLeftButtonFlag = false;
volatile bool encLeftButtonISREn = false;

bool ledFlag = false;
uint8_t ledCount = 0;

bool firstLineDone = false; // flag for first line (titles) having been read
bool newDataFile = false;
uint32_t lastLinePosition = 0;
char sd_buf[200]; // buffer to store single SD card line

// state = 0 initializing
// state = 1 navigating menu
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
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(SWITCH_PIN, INPUT_PULLUP);
  pinMode(SD_CS_PIN, OUTPUT);
  pinMode(ENC_RIGHT_BUTTON, INPUT_PULLDOWN);
  pinMode(ENC_LEFT_BUTTON, INPUT_PULLDOWN);

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

  char displayBuffer[25];
  // Create data.txt file if does not exist
  if(!SD.exists(dataFileName))
  {
    strcpy(displayBuffer, "Creating ");
    strcat(displayBuffer, dataFileName);
    display.clearDisplay();
    updateDisplay(displayBuffer, 40, false);
    display.display();
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
      display.clearDisplay();
      updateDisplay("Unable to open file", 32, false);
      updateDisplay("Check SD, reset device", 48, false);
      display.display();
    }

  }

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

  // Attach ISR for flipping buttonFlag when button is pressed
  attachInterrupt(digitalPinToInterrupt(ENC_RIGHT_BUTTON), encRightButtonISR, RISING);
  attachInterrupt(digitalPinToInterrupt(ENC_LEFT_BUTTON), encLeftButtonISR, RISING);

  // enable button ISR
  encRightButtonISREn = true;
  encLeftButtonISREn = true;

  state = 0;
}

void loop() {
  // check number of milliseconds since Arduino was turned on
  curMillis = millis();

  displayPage(page);

  if(state == 0)
  {
    updateMenuSelection();

    if(encRightButtonFlag)
    {
      if(page == 0)
      {
        if(currentVertMenuSelection == 0)
        {
          // state = 2; // collect data
          page = 1;
        }
        else if(currentVertMenuSelection == 1)
        {
          prevState = state;
          state = 3; // upload data
          page = 4;
        }
      }
      else if (page == 1)
      {
        if(currentVertMenuSelection == 0)
        {
          // Use GPS for time stamp
          prevState = state;
          state = 2; // collect data
          page = 5; 
        }
        else if(currentVertMenuSelection == 1)
        {
          // Use manual entry + RTC
          page = 2; // enter date
        }
      }
      else if (page == 2)
      {
        page = 3; // enter time
      }
      else if (page == 3)
      {
        // TODO: Save time stamp here and start RTC, for now just print it out
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
        
        prevState = state;
        state = 2; // collect data
        page = 5;
      }

      // reset menus for next page
      if(page == 2) currentVertMenuSelection = manualMonth - 1;
      else if(page == 3) currentVertMenuSelection = manualHour;
      else currentVertMenuSelection = 0;
      currentHoriMenuSelection = 0;
      encRightButtonFlag = false;
      encRightButtonISREn = true;
    }
  }

  if(state == 2) // Collecting data
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
        gpsFlag = true;
        prevGpsMillis = curMillis;
      }
      updateSampleSD();
    }
  }

  // Upload data.txt to serial monitor if buttonFlag has been set (inside buttonISR)
  if(state == 3) // uploading data
  {
    uploadSerial();
    state = prevState;
    prevState = 3;
  }

  if(encLeftButtonFlag) // back button
  {
    switch(page)
    {
      case 1: // time entry choice menu
        page = 0;
        prevState = state;
        state = 0;
        break;
      case 2: // date entry page
        page = 1;
        prevState = state;
        state = 0;
        break;
      case 3: // time entry page
        page = 2;
        prevState = state;
        state = 0;
        break;
      case 4: // SD card file list menu
        page = 0;
        prevState = state;
        state = 0;
        break;
      case 5: // data collection screen
        page = 1;
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
  bool firstFlag = false; // local version of the first GPS read flag used for timeout logic
  bool timeoutFlag = false;
  time_t localTime;
  time_t utcTime;

  BMP280_temp_t temp;
  BMP280_press_t press;

  if(gpsFlag)
  {
    displayPage(page);

    if(firstGpsRead)
    {
      firstFlag = true;
    }

    // // wake up GPS module
    if (!gpsAwake)
    {
      toggleGps();
    }
    unsigned long gpsReadCurMillis;
    unsigned long gpsReadStartMillis = millis();
    unsigned long gpsTimeoutMillis;
    if (firstFlag)
    {
      gpsTimeoutMillis = GPS_FIRST_TIMEOUT;
    }
    else
    {
      gpsTimeoutMillis = GPS_TIMEOUT;
    }
    #ifdef DEBUG_PRINT
    unsigned long preRead = millis();
    #endif

    // Read GPS data until it's valid
    // 10 minute timeout for first GPS read
    // 5 second timeout for further reads
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

        if (now() > prevTimeStamp)
        {
          prevTimeStamp = now();
          gpsDisplayFail = false;
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
      if (firstFlag || timeoutFlag)
      {
        delay(500); // add extra delay after first read or timeout to make sure sleep command is properly interpreted before next read
      }
    }

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

    if (firstGpsRead) {firstGpsRead = false;}
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

  unsigned long msTimer = millis();

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
      Serial.print(msTimer);
      Serial.print(',');

      if (timeoutFlag)
      {
        Serial.print("GPS read failed");
      }
      else
      {
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
      }
      Serial.print(',');
      Serial.print(temp.integral); Serial.print('.'); Serial.print(temp.fractional);
      Serial.print(',');
      Serial.print(press.integral); Serial.print('.'); Serial.println(press.fractional);
    }
    
    Serial.print(msTimer);
    Serial.print(',');
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

  if(gpsFlag)
  {
    dataFile.print("# ");
    dataFile.print(msTimer);
    dataFile.print(',');

    if (timeoutFlag)
    {
      dataFile.print("GPS read failed");
    }
    else
    {
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
    }

    dataFile.print(',');
    dataFile.print(temp.integral); dataFile.print('.'); dataFile.print(temp.fractional);
    dataFile.print(',');
    dataFile.print(press.integral); dataFile.print('.'); dataFile.println(press.fractional);
  }

    dataFile.print(msTimer);
    dataFile.print(',');
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

    ledFlag = true;
  }
  else
  {
    #ifdef DEBUG_PRINT
    Serial.println("Couldn't open file");
    #endif
    updateDisplay("Couldn't open file", 20, false);
  }

  if(gpsFlag)
  {
    gpsFlag = false;
  }

}

// upload SD card data over serial port
void uploadSerial()
{
  encRightButtonISREn = false; // disable button ISR
  uint8_t buffer[512] = {0}; // buffer to read/write data 512 bytes at a time
  uint16_t writeLen = sizeof(buffer);
  updateDisplay("Uploading data", 16, false);
  updateDisplay("via serial port", 24, false);
  #ifdef DEBUG_PRINT
  Serial.println("Serial upload initiated");
  #endif
  delay(2500);
  
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
      if (dataFile.available() > sizeof(buffer))
      {
        dataFile.read(buffer, sizeof(buffer));
        writeLen = sizeof(buffer);
      }
      else
      {
        writeLen = dataFile.available();  
        dataFile.read(buffer, dataFile.available());
      }
      
      // look for and remove x if there
      for (int i = 0; i < writeLen; i++)
      {
        if (buffer[i] == 'x')
        {
          // move all data back by 3 bytes
          writeLen -= 3;
          for (int j = i; j < writeLen; j++)
          {
            buffer[j] = buffer[j+3];
          }
          buffer[writeLen] = 0;
          buffer[writeLen+1] = 0;
          buffer[writeLen+2] = 0;
        }
      }
      Serial.write(buffer, writeLen);
      memset(buffer, 0, sizeof(buffer));
    }
    dataFile.close();
  }
  else // if switch is low (to the right), only upload from the location in the file where last update ended
  {
    #ifdef DEBUG_PRINT
    Serial.println("Mode 3, incremental upload");
    #endif
    bool xFound = false; // find the x, indicating last line read
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
    }
    else
    {
      #ifdef DEBUG_PRINT
      Serial.println("Couldn't open file");
      #endif
    }
    // Now remove x, add it to the end of the file  
    dataFile.seek(0);
    // Open temporary file
    File tmpFile = SD.open("tmp.txt", FILE_WRITE);
    #ifdef DEBUG_PRINT
    unsigned long preSD = millis();
    #endif
    if(dataFile && tmpFile)
    {
      #ifdef DEBUG_PRINT
      Serial.println("Moving data into tmp.txt");
      #endif
      // Move data, minus the x and following CR and NL, into tmp.txt
      while(dataFile.available())
      {
        if (dataFile.available() > sizeof(buffer))
        {
          dataFile.read(buffer, sizeof(buffer));
          writeLen = sizeof(buffer);
        }
        else
        {
          writeLen = dataFile.available();
          dataFile.read(buffer, dataFile.available());
        }
        
        // look for and remove x if there
        for (int i = 0; i < writeLen; i++)
        {
          if (buffer[i] == 'x')
          {
            // move all data back by 3 bytes
            writeLen -= 3;
            for (int j = i; j < writeLen; j++)
            {
              buffer[j] = buffer[j+3];
            }
            buffer[writeLen] = 0;
            buffer[writeLen+1] = 0;
            buffer[writeLen+2] = 0;
          }
        }

        tmpFile.write(buffer, writeLen);
        memset(buffer, 0, sizeof(buffer));
      }
      #ifdef DEBUG_PRINT
      unsigned long postSD = millis();
      Serial.print("Moving contents of data.txt took: "); Serial.print(postSD - preSD); Serial.println(" ms");
      Serial.println("Renaming and deleting old data file");
      #endif
      // Rename tmp.txt to data.txt, delete old data.txt (after renaming to datatmp.txt)
      if (dataFile.rename("datatmp.txt"))
      {
        #ifdef DEBUG_PRINT
        Serial.println("Renamed data.txt to datatmp.txt");
        #endif
      }
      if (tmpFile.rename(dataFileName))
      {
        #ifdef DEBUG_PRINT
        Serial.println("Renamed tmp.txt to data.txt");
        #endif
      }
      dataFile.close();
      tmpFile.close();
      SD.remove("datatmp.txt");
      
      tmpFile = SD.open(dataFileName, FILE_WRITE);

      #ifdef DEBUG_PRINT
      Serial.println("Moving x to end of new data file");
      #endif
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
  encRightButtonISREn = true;
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
  if(!manualTimeEntry)
  {
    // get time stamp from GPS, create data files with names accordingly
  }
  else
  {
    // RTC clock should already be set, use that
  }
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
          else if(currentHoriMenuSelection == 2) // time zone
          {
            if(currentVertMenuSelection > 12) currentVertMenuSelection = 12;
            manualTimeZone = currentVertMenuSelection;
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
          else if(currentHoriMenuSelection == 2) // time zone
          {
            if(currentVertMenuSelection < -12) currentVertMenuSelection = -12;
            manualTimeZone = currentVertMenuSelection;
          }
          break;
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
          if(currentHoriMenuSelection > 2) currentHoriMenuSelection = 2;

          if(currentHoriMenuSelection == 0) // hour
          {
            currentVertMenuSelection = manualHour;
          }
          else if(currentHoriMenuSelection == 1) // minute
          {
            currentVertMenuSelection = manualMinute;
          }
          else if(currentHoriMenuSelection == 2) // time zone
          {
            currentVertMenuSelection = manualTimeZone;
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
        else if(currentHoriMenuSelection == 2) // time zone
        {
          currentVertMenuSelection = manualTimeZone;
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
  if (page != 4)
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
    case(1): // Time entry method menu
      display.drawLine(0, 10, display.width()-1, 10, SSD1327_WHITE);
      updateDisplay("Time entry method?", 0, false);
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
    case(2): // Date entry
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
    case(3): // Time entry
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

      display.print(' ');
      if(currentHoriMenuSelection == 2) display.setTextColor(SSD1327_BLACK, SSD1327_WHITE);
      if(manualTimeZone >= 0) display.print('+');
      display.print(manualTimeZone);

      display.setTextColor(SSD1327_WHITE);
      display.setTextSize(1);

      break;
    case(5): // Data collection
      if(gpsFlag) 
      {
        if(firstGpsRead)
        {
          updateDisplay("Reading GPS...", 32, false);
          updateDisplay("(GPS warming up)", 48, false);
        }
        else
        {
          updateDisplay("Reading GPS...", 40, false);
        }
      }
      else
      {
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
        strcat(displayText, " ug/m3");
        updateDisplay(displayText, 16, false);

        strcpy(displayText, "PM2.5:  ");
        strcat(displayText, pm2p5Text);
        strcat(displayText, " ug/m3");
        updateDisplay(displayText, 24, false);

        strcpy(displayText, "PM10.0: ");
        strcat(displayText, pm10p0Text);
        strcat(displayText, " ug/m3");
        updateDisplay(displayText, 32, false);

        if(gpsDisplayFail)
        {
          updateDisplay("GPS read failed", 64, false);
        }
        else
        {
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
          updateDisplay(timeText, 64, false);
        }
      }
      break;
  }
  display.display();
}

// function for displaying characters to OLED 
void updateDisplay(char* text, uint8_t height, bool bg)
{
  // if(clear) display.clearDisplay();

  if(page == 2 || page == 3) display.setTextSize(2); // bigger for date/time entry
  else display.setTextSize(1);

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