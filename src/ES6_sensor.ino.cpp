# 1 "C:\\Users\\beb539\\AppData\\Local\\Temp\\tmpw6m77cg3"
#include <Arduino.h>
# 1 "C:/Users/beb539/Documents/Courses/S21/ES6/ES6_sensor/src/ES6_sensor.ino"
# 16 "C:/Users/beb539/Documents/Courses/S21/ES6/ES6_sensor/src/ES6_sensor.ino"
#include "ThingSpeak.h"
#include <U8g2lib.h>

#include <WiFi101.h>
#include <SPI.h>
#include <SD.h>
#include <TinyGPS++.h>
#include <TimeLib.h>
#include <Wire.h>

#define SAMP_TIME 5000
#define BLINK_TIME 300
#define BLINK_CNT 3
#define SENSE_PIN 0
#define BUTTON_PIN 7
#define SD_CS_PIN 4
#define SENSOR_ADDR 0x40

time_t prevUpdate;

char ssid[] = "Landfall";
char password[] = "slosilo!";
WiFiClient client;

File dataFile;
String dataFileName = "datadump.csv";

TinyGPSPlus gps;

U8G2_SSD1306_128X64_ALT0_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const uint32_t channelNumber = 1186416;
const char writeApiKey[] = "IRCA839MQSQUAH59";
const char server[] = "api.thingspeak.com";

unsigned long prevSampMillis;
unsigned long prevLedMillis;
unsigned long curMillis;

volatile bool wifiFlag = false;
volatile bool buttonISREn = false;

bool ledFlag = false;
uint8_t ledCount = 0;
# 73 "C:/Users/beb539/Documents/Courses/S21/ES6/ES6_sensor/src/ES6_sensor.ino"
uint16_t particleData[12];

uint8_t i2c_buf[30];

uint32_t lastLineRead = 0;
char sd_buf[200];
char tspeak_buf[2000];
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
#line 81 "C:/Users/beb539/Documents/Courses/S21/ES6/ES6_sensor/src/ES6_sensor.ino"
void setup() {

  Serial.begin(115200);


  Serial1.begin(9600);

  Wire.begin();

  u8g2.begin();


  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT);
  pinMode(SD_CS_PIN, OUTPUT);

  Serial.println("Initialize SD");


  if(!SD.begin(SD_CS_PIN))
  {
    Serial.println("Card failed");
    while(true);
  }
  else
  {
    Serial.println("Card initialized successfully");
  }



  if(!SD.exists(dataFileName))
  {
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
    }

  }

  if(!initDustSensor())
  {
    Serial.println("Failed to initialize dust sensor");
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
  connectWiFi();

  memset(tspeak_buf, 0, sizeof(tspeak_buf));
  strcpy(tspeak_buf, "write_api_key=");
  strcat(tspeak_buf, writeApiKey);
  strcat(tspeak_buf, "&");
  strcat(tspeak_buf, "time_format=absolute&updates=");


  dataFile = SD.open(dataFileName, FILE_READ);
  if(dataFile)
  {
    uint32_t lineCount = 0;


    uint32_t charCount = 60;
    uint32_t i = 0;
    uint32_t linePosition = 0;

    while(dataFile.available())
    {
      char c = dataFile.read();
      charCount++;


      if(charCount >= 2000 && c != '\n')
      {
        httpRequest(tspeak_buf);


        charCount = 60;
        memset(tspeak_buf, 0, sizeof(tspeak_buf));
        strcpy(tspeak_buf, "write_api_key=");
        strcat(tspeak_buf, writeApiKey);
        strcat(tspeak_buf, "&");
        strcat(tspeak_buf, "time_format=absolute&updates=");


        memset(sd_buf, 0, sizeof(sd_buf));
        dataFile.seek(linePosition);

        delay(20000);
      }
      else if(c == '\n')
      {

        i = 0;


        linePosition = dataFile.position();


        if(lineCount++ <= lastLineRead)
        {

          memset(sd_buf, 0, sizeof(sd_buf));


          charCount = 60;
          continue;
        }


        strcat(tspeak_buf, sd_buf);
        strcat(tspeak_buf, "|");

        memset(sd_buf, 0, sizeof(sd_buf));
      }
      else
      {

        sd_buf[i++] = c;
      }
    }


    dataFile.close();



    lastLineRead = lineCount - 1;


    httpRequest(tspeak_buf);


    WiFi.end();
  }
  else
  {
    Serial.println("unable to open file");
  }
}


void updateSampleSD()
{
  buttonISREn = false;

  wakeGps();

  prevSampMillis = curMillis;

  readGps();


  while(!(gps.date.isValid() && gps.time.isValid() && gps.location.isValid()))
  {
    readGps();
  }

  while(!readDustSensor(i2c_buf, 29))
  {
    Serial.println("Sensor reading didn't work, trying again");
  }

  if(!parseSensorData(particleData, i2c_buf))
  {
    Serial.println("checksum incorrect, data will be stale");
  }




  if(gps.time.age() <= 1500 && gps.date.age() <= 1500)
  {
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
      Serial.println(gps.time.age());
      Serial.println(gps.date.age());
      Serial.println(gps.location.age());

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
      dataFile.print("good");
      dataFile.println(",");
      dataFile.close();

      char displayText[50];
      char pm1p0Text[10];
      char pm2p5Text[10];
      char pm10p0Text[10];
      itoa(particleData[3], pm1p0Text, 10);
      itoa(particleData[4], pm2p5Text, 10);
      itoa(particleData[5], pm10p0Text, 10);
      strcpy(displayText, "PM1.0: ");
      strcat(displayText, pm1p0Text);
      strcat(displayText, " ug/m\xb3");
      display(displayText, 10, true, false);

      strcpy(displayText, "PM2.5: ");
      strcat(displayText, pm2p5Text);
      strcat(displayText, " ug/m\xb3");
      display(displayText, 20, false, false);

      strcpy(displayText, "PM10.0: ");
      strcat(displayText, pm10p0Text);
      strcat(displayText, " ug/m\xb3");
      display(displayText, 30, false, true);

      ledFlag = true;
    }
    else
    {
      Serial.println("Couldn't open file");
    }
  }

  sleepGps();

  buttonISREn = true;
}


bool httpRequest(char* buffer)
{
  client.stop();
  char* data_length;
  char* post;
  char* channelID;
  itoa(channelNumber, channelID, 10);
  itoa(strlen(buffer), data_length, 10);

  strcpy(post, "POST /channels/");
  strcat(post, channelID);
  strcat(post, "/bulk_update.csv HTTP/1.1");

  if(client.connect(server, 80))
  {
    client.println(post);
    client.println("Host: api.thingspeak.com");
    client.println("Content-Type: application/x-www-form-urlencoded");
    client.println("time_format: absolute");
    client.println();
    client.println(buffer);
  }
  else
  {
    Serial.println("Failed to connect to ThingSpeak");
    return false;
  }

  delay(250);
  client.parseFloat();
  int resp = client.parseInt();
  if(resp == 200)
  {
    Serial.println("Successful update");
    return true;
  }
  else
  {
    Serial.print("Failed update, code: ");
    Serial.println(resp);
    return false;
  }

  client.stop();

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

  u8g2.setFont(u8g2_font_helvB08_tf);
  u8g2.drawStr(0, height, text);
  u8g2.sendBuffer();

  if(send)
  {
    u8g2.sendBuffer();
  }
}