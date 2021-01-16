# 1 "C:\\Users\\beb539\\AppData\\Local\\Temp\\tmpxh7xg4k7"
#include <Arduino.h>
# 1 "C:/Users/beb539/Documents/Courses/S21/ES6/HazeL/src/HazeL.ino"
# 18 "C:/Users/beb539/Documents/Courses/S21/ES6/HazeL/src/HazeL.ino"
#include <SPI.h>

#include "SdFat.h"
#include <Wire.h>
#include "secrets.h"
#include "HM3301.h"


#include <U8g2lib.h>
#include <TinyGPS++.h>
#include <TimeLib.h>
#include <WiFi101.h>
#include "Seeed_BMP280.h"

#define SAMP_TIME 2500
#define BLINK_TIME 30
#define GPS_TIME 10000
#define BLINK_CNT 3
#define BUTTON_PIN A2
#define SWITCH_PIN A3
#define SD_CS_PIN 4
#define CUR_YEAR 2021


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

bool firstLineDone = false;
bool newDataFile = false;
uint32_t lastLinePosition = 0;
char sd_buf[200];
void setup();
void loop();
void buttonISR();
void updateSampleSD();
void uploadSerial();
void blinkLed();
void readGps();
void sleepGps();
void wakeGps();
void sendGpsCommand(const char* cmd);
char createChecksum(char* cmd);
void display(char* text, u8g2_uint_t height, bool clear, bool send);
void updateThingSpeak();
void connectWiFi();
bool httpRequest(char* buffer);
#line 80 "C:/Users/beb539/Documents/Courses/S21/ES6/HazeL/src/HazeL.ino"
void setup() {

  Serial.begin(115200);


  Serial1.begin(9600);


  Wire.begin();


  u8g2.begin();

  display("Initializing...", 20, true, true);
  delay(2500);
#ifdef DEBUG_PRINT
  Serial.println("Initializing...");
#endif


  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(SWITCH_PIN, INPUT_PULLUP);
  pinMode(SD_CS_PIN, OUTPUT);

#ifdef DEBUG_PRINT
  Serial.println("Initialize SD");
#endif
  display("Checking SD", 20, true, true);
  delay(2500);


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

    dataFile = SD.open(dataFileName, FILE_WRITE);
    if(dataFile)
    {
      dataFile.println('x');
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


  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, RISING);


  sleepGps();


  buttonISREn = true;
}

void loop() {

  curMillis = millis();


  if(ledFlag)
  {
    if(curMillis - prevLedMillis >= BLINK_TIME)
    {
      prevLedMillis = curMillis;
      blinkLed();
    }
  }


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


  if(buttonFlag)
  {
    buttonFlag = false;
    buttonISREn = true;
    uploadSerial();
  }

}


void buttonISR()
{
  if(buttonISREn == true)
  {
    buttonFlag = true;
    buttonISREn = false;
  }
}


void updateSampleSD()
{
  bool staleFlag = false;
  time_t localTime;
  time_t utcTime;

  BMP280_temp_t temp;
  BMP280_press_t press;

  if(gpsFlag)
  {

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


    do
    {
      readGps();
    } while (!(gps.date.isValid() && gps.time.isValid() && gps.location.isValid() && gps.altitude.isValid() && gps.date.year() == CUR_YEAR));


    setTime(gps.time.hour(), gps.time.minute(), gps.time.second(), gps.date.day(), gps.date.month(), gps.date.year());


    utcTime = now();


    localTime = utcTime;
    localYear = year(localTime);
    localMonth = month(localTime);
    localDay = day(localTime);
    localHour = hour(localTime);
    localMinute = minute(localTime);
    localSecond = second(localTime);


    temp = TPSensor.getTemperature();
    press = TPSensor.getPressure();
  }


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



  dataFile = SD.open(dataFileName, FILE_WRITE);
  if(dataFile)
  {



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
      Serial.print(gps.location.lat(), 5);
      Serial.print(',');
      Serial.print(gps.location.lng(), 5);
      Serial.print(',');
      Serial.print(gps.altitude.meters(), 2);

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
    dataFile.print(gps.location.lat(), 5);
    dataFile.print(',');
    dataFile.print(gps.location.lng(), 5);
    dataFile.print(',');
    dataFile.print(gps.altitude.meters(), 2);

    dataFile.print(',');
    dataFile.print(temp.integral); dataFile.print('.'); dataFile.print(temp.fractional);
    dataFile.print(',');
    dataFile.print(press.integral); dataFile.print('.'); dataFile.println(press.fractional);
  }

    dataFile.print(PM1p0_std);
    dataFile.print(",");
    dataFile.print(PM2p5_std);
    dataFile.print(",");
    dataFile.print(PM10p0_std);
    dataFile.print(",");
    dataFile.print(PM1p0_atm);
    dataFile.print(",");
    dataFile.print(PM2p5_atm);
    dataFile.print(",");
    dataFile.print(PM10p0_atm);
    dataFile.print(",");
    dataFile.print(count_0p3um);
    dataFile.print(",");
    dataFile.print(count_0p5um);
    dataFile.print(",");
    dataFile.print(count_1p0um);
    dataFile.print(",");
    dataFile.print(count_2p5um);
    dataFile.print(",");
    dataFile.print(count_5p0um);
    dataFile.print(",");
    dataFile.println(count_10p0um);
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


  if(gpsFlag)
  {
    sleepGps();
    gpsFlag = false;
  }

}


void uploadSerial()
{
  buttonISREn = false;

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
        dataFile.read();
        dataFile.read();
        continue;
      }
      else
      {
        Serial.write(c);
      }
    }
  }
  else
  {
#ifdef DEBUG_PRINT
    Serial.println("Mode 3, incremental upload");
#endif
    bool xFound = false;
    uint32_t xPosition;
    int i = 0;
    dataFile = SD.open(dataFileName, FILE_READ);
    if(dataFile)
    {
      while(dataFile.available())
      {
        char c = dataFile.read();


        if(!xFound)
        {
          if(c == 'x')
          {
            xFound = true;
            xPosition = dataFile.position();
            dataFile.read();
            dataFile.read();
          }
          continue;
        }
        else
        {
          if(c == '\n')
          {
            i = 0;
            Serial.println(sd_buf);
            memset(sd_buf, 0, sizeof(sd_buf));
          }
          else
          {
            sd_buf[i++] = c;
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


    File tmpFile = SD.open("tmp.txt", FILE_WRITE);
    dataFile = SD.open(dataFileName, FILE_READ);
    if(dataFile && tmpFile)
    {

      while(dataFile.available())
      {
        char c = dataFile.read();
        if (c == 'x')
        {

          dataFile.read();
          dataFile.read();
          continue;
        }
        tmpFile.write(c);
      }


      dataFile.rename("datatmp.txt");
      tmpFile.rename(dataFileName);
      dataFile.close();
      tmpFile.close();
      SD.remove("datatmp.txt");

      tmpFile = SD.open(dataFileName, O_RDWR);

      tmpFile.seek(tmpFile.size()-1);
      tmpFile.println('x');
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


void blinkLed()
{

  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));

  if(++ledCount >= BLINK_CNT*2)
  {
    ledFlag = false;
    ledCount = 0;
  }
}


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


void sleepGps()
{
  sendGpsCommand("105,8");
}


void wakeGps()
{
  sendGpsCommand("105,0");
}


void sendGpsCommand(const char* cmd)
{
  char cmdBase[] = "PGKC";
  char* finalCmd = strcat(cmdBase, cmd);
  char checksum = createChecksum(finalCmd);

  Serial1.write('$');
  Serial1.write(finalCmd);
  Serial1.write('*');
  Serial1.print(checksum, HEX);
  Serial1.write("\r\n");
}


char createChecksum(char* cmd)
{
  char checksum = 0;

  for(int i = 0; i < strlen(cmd); i++)
  {
    checksum = checksum ^ cmd[i];
  }

  return checksum;
}


void display(char* text, u8g2_uint_t height, bool clear, bool send)
{
  if(clear)
  {
    u8g2.clearBuffer();
  }


  u8g2.setFont(u8g2_font_synchronizer_nbp_tf);
  u8g2.drawStr(0, height, text);
  u8g2.sendBuffer();

  if(send)
  {
    u8g2.sendBuffer();
  }
}





WiFiClient client;
const char server[] = "api.thingspeak.com";

char tspeak_buf[5000];
char status_buf[20];
uint8_t colPositions[17] = {0};
unsigned long prevFileSize = 0;
unsigned long bytesLeft = 0;


void updateThingSpeak()
{

  memset(tspeak_buf, 0, sizeof(tspeak_buf));
  strcpy(tspeak_buf, "write_api_key=");
  strcat(tspeak_buf, writeApiKey);
  strcat(tspeak_buf, "&time_format=absolute&updates=");


  display("Updating to", 16, true, false);
  display("ThingSpeak...00%", 24, false, true);
  dataFile = SD.open(dataFileName, FILE_READ);
  if(dataFile)
  {
    uint32_t charCount = strlen(tspeak_buf);
    uint32 rawByteCount = 0;
    uint32_t colCount = 0;
    uint32_t i = 0;
    uint32_t linePosition = 0;
    int percentComplete = 0;

    dataFile.seek(lastLinePosition);
    bytesLeft = dataFile.size() - prevFileSize;
    prevFileSize = dataFile.size();

    while(dataFile.available())
    {
      char c = dataFile.read();
      rawByteCount++;


      if((charCount >= sizeof(tspeak_buf) - 200) && (c != '\n'))
      {
        Serial.println("Buffer full!");
        Serial.println(charCount);
        tspeak_buf[strlen(tspeak_buf) - 1] = 0;

        while(!httpRequest(tspeak_buf));
        percentComplete = (rawByteCount*100)/bytesLeft;


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


        memset(sd_buf, 0, sizeof(sd_buf));
        dataFile.seek(linePosition);

        display("Updating to", 16, true, false);
        display(tspeakDisplay, 24, false, true);
        delay(15000);

        Serial.println("Back to updating");
      }
      else if(c == '\n')
      {

        i = 0;
        colCount = 0;


        linePosition = dataFile.position();


        if(!firstLineDone)
        {
          firstLineDone = true;
          continue;
        }


        memset(status_buf, 0, sizeof(status_buf));
        int j = 0;
        for(int k = colPositions[16]; k < strlen(sd_buf); k++)
        {
          status_buf[j++] = sd_buf[k];
        }


        if(status_buf[0] == 'g')
        {
          Serial.println("Updating thingspeak buffer");


          uint8_t pmStIndex = colPositions[1];
          uint8_t pmAtIndex = colPositions[4];
          uint8_t zerop5umIndex = colPositions[8];
          uint8_t oneumIndex = colPositions[9];
          uint8_t pmColsDel = pmAtIndex - pmStIndex;
          uint8_t cntsColDel = oneumIndex - zerop5umIndex;


          zerop5umIndex -= pmColsDel;
          oneumIndex -= pmColsDel;

          for(int k = pmStIndex; k < sizeof(sd_buf) - pmColsDel; k++)
          {
            sd_buf[k] = sd_buf[k + pmColsDel];
          }
          for(int k = zerop5umIndex; k < sizeof(sd_buf) - cntsColDel; k++)
          {
            sd_buf[k] = sd_buf[k + cntsColDel];
          }


          sd_buf[strlen(sd_buf) - 1] = 0;
          Serial.println(sd_buf);


          strcat(tspeak_buf, sd_buf);
          strcat(tspeak_buf, "|");
          charCount += strlen(sd_buf)+1;
        }

        memset(sd_buf, 0, sizeof(sd_buf));
      }
      else if(firstLineDone)
      {

        if(c == ',')
        {
          colPositions[++colCount] = i+1;
        }
        sd_buf[i++] = c;
      }
    }


    dataFile.close();


    lastLinePosition = linePosition;


    tspeak_buf[strlen(tspeak_buf) - 1] = 0;


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


void connectWiFi()
{

  unsigned long startTime = millis();
  display("Connecting to WiFi", 20, true, true);

  WiFi.begin(ssid, password);

  while(WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    if(millis() - startTime > 20000)
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


bool httpRequest(char* buffer)
{
  bool success = false;


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