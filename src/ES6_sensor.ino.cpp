# 1 "C:\\Users\\beb539\\AppData\\Local\\Temp\\tmpfgwfutd5"
#include <Arduino.h>
# 1 "C:/Users/beb539/Documents/Courses/S21/ES6/ES6_sensor/src/ES6_sensor.ino"
# 18 "C:/Users/beb539/Documents/Courses/S21/ES6/ES6_sensor/src/ES6_sensor.ino"
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include "secrets.h"


#include <U8g2lib.h>
#include <WiFi101.h>
#include <TinyGPS++.h>
#include <TimeLib.h>

#define SAMP_TIME 5000
#define BLINK_TIME 300
#define BLINK_CNT 3
#define BUTTON_PIN 0
#define SD_CS_PIN 4
#define SENSOR_ADDR 0x40

WiFiClient client;

File dataFile;
char dataFileName[] = "data.csv";

TinyGPSPlus gps;
time_t prevTimeStamp;

U8G2_SSD1306_128X64_ALT0_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const char server[] = "api.thingspeak.com";

unsigned long prevSampMillis;
unsigned long prevLedMillis;
unsigned long prevWiFiMillis;
unsigned long curMillis;

volatile bool wifiFlag = false;
volatile bool buttonISREn = false;

bool ledFlag = false;
uint8_t ledCount = 0;
# 74 "C:/Users/beb539/Documents/Courses/S21/ES6/ES6_sensor/src/ES6_sensor.ino"
uint16_t particleData[12];

uint8_t i2c_buf[30];

bool firstLineDone = false;
uint32_t lastLineRead = 0;
uint32_t lastLinePosition = 0;
char sd_buf[200];
char tspeak_buf[5000];
char status_buf[20];
uint8_t colPositions[17] = {0};
void setup();
void loop();
void buttonISR();
bool initDustSensor();
bool readDustSensor(uint8_t *data, uint32_t data_len);
bool parseSensorData(uint16_t *data_out, uint8_t *data_raw);
void updateThingSpeak();
void updateSampleSD();
bool httpRequest(char* buffer);
void blinkLed();
void connectWiFi();
void readGps();
void sleepGps();
void wakeGps();
void sendGpsCommand(const char* cmd);
char createChecksum(char* cmd);
void display(char* text, u8g2_uint_t height, bool clear, bool send);
#line 86 "C:/Users/beb539/Documents/Courses/S21/ES6/ES6_sensor/src/ES6_sensor.ino"
void setup() {

  Serial.begin(115200);


  Serial1.begin(9600);


  Wire.begin();


  u8g2.begin();

  display("Initializing...", 20, true, true);
  delay(2500);


  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT);
  pinMode(SD_CS_PIN, OUTPUT);

  Serial.println("Initialize SD");
  display("Checking SD", 20, true, true);
  delay(2500);


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
      blinkLed();
    }
  }


  if(curMillis - prevSampMillis >= SAMP_TIME)
  {
    updateSampleSD();
    Serial.println("Updated sample in SD card");
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


void buttonISR()
{
  if(buttonISREn == true)
  {
    wifiFlag = true;
    buttonISREn = false;
  }
}


bool initDustSensor()
{
  bool initSuccess;
  Wire.beginTransmission(SENSOR_ADDR);
  Wire.write(0x88);
  initSuccess = Wire.endTransmission();


  return !initSuccess;
}


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



bool parseSensorData(uint16_t *data_out, uint8_t *data_raw)
{
  int j = 0;
  byte sum = 0;

  for(int i = 0; i < 28; i++)
  {
    sum += data_raw[i];
  }


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


void updateThingSpeak()
{

  memset(tspeak_buf, 0, sizeof(tspeak_buf));
  strcpy(tspeak_buf, "write_api_key=");
  strcat(tspeak_buf, writeApiKey);
  strcat(tspeak_buf, "&time_format=absolute&updates=");


  display("Updating to", 16, true, false);
  display("ThingSpeak...", 24, false, true);
  dataFile = SD.open(dataFileName, FILE_READ);
  if(dataFile)
  {
    uint32_t lineCount = 0;
    uint32_t charCount = strlen(tspeak_buf);
    uint32_t colCount = 0;
    uint32_t i = 0;
    uint32_t linePosition = 0;

    dataFile.seek(lastLinePosition);

    while(dataFile.available())
    {
      char c = dataFile.read();


      if((charCount >= sizeof(tspeak_buf) - 200) && (c != '\n'))
      {
        Serial.println("Buffer full!");
        Serial.println(charCount);
        tspeak_buf[strlen(tspeak_buf) - 1] = 0;

        while(!httpRequest(tspeak_buf));


        memset(tspeak_buf, 0, sizeof(tspeak_buf));
        strcpy(tspeak_buf, "write_api_key=");
        strcat(tspeak_buf, writeApiKey);
        strcat(tspeak_buf, "&time_format=absolute&updates=");

        i = 0;
        colCount = 0;
        charCount = strlen(tspeak_buf);


        memset(sd_buf, 0, sizeof(sd_buf));
        dataFile.seek(linePosition);

        delay(20000);
        display("Updating to", 16, true, false);
        display("ThingSpeak...", 24, false, true);

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
          uint8_t lastIndex;


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
    while(!httpRequest(tspeak_buf));
  }
  else
  {
    Serial.println("unable to open file");
    display("Couldn't open file", 20, true, false);
    delay(5000);
  }
}


void updateSampleSD()
{
  bool staleFlag = false;


  buttonISREn = false;


  wakeGps();

  prevSampMillis = curMillis;

  display("Reading GPS...", 20, true, true);


  readGps();


  while(!(gps.date.isValid() && gps.time.isValid() && gps.location.isValid() && gps.altitude.isValid()))
  {
    readGps();
  }


  setTime(gps.time.hour(), gps.time.minute(), gps.time.second(), gps.date.day(), gps.date.month(), gps.date.year());


  if(now() > prevTimeStamp)
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



  dataFile = SD.open(dataFileName, FILE_WRITE);
  if(dataFile)
  {

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
    dataFile.print(particleData[0]);
    dataFile.print(",");
    dataFile.print(particleData[1]);
    dataFile.print(",");
    dataFile.print(particleData[2]);
    dataFile.print(",");
    dataFile.print(particleData[3]);
    dataFile.print(",");
    dataFile.print(particleData[4]);
    dataFile.print(",");
    dataFile.print(particleData[5]);
    dataFile.print(",");
    dataFile.print(particleData[6]);
    dataFile.print(",");
    dataFile.print(particleData[7]);
    dataFile.print(",");
    dataFile.print(particleData[8]);
    dataFile.print(",");
    dataFile.print(particleData[9]);
    dataFile.print(",");
    dataFile.print(particleData[10]);
    dataFile.print(",");
    dataFile.print(particleData[11]);
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
    if(staleFlag)
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

  Serial.print("Command sent to GPS module: ");
  Serial.write('$');
  Serial.write(finalCmd);
  Serial.write('*');
  Serial.print(checksum, HEX);
  Serial.write("\r\n");

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