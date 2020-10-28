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
// #include <WiFiNINA.h>
#include <WiFi101.h>
#include <SPI.h>
#include <SD.h>
#include <TinyGPS++.h>
#include <Wire.h>

#define SAMP_TIME 5000 // in milliseconds, sensor updates every 1 second, read it every 5
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
String dataFileName = "datadump.csv";

TinyGPSPlus gps;

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

uint32_t lastLineRead = 0; // latest line that SD card was updated from
char sd_buf[200]; // buffer to store single SD card line
char tspeak_buf[2000];

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

void setup() {
  // initialize Serial port
  Serial.begin(115200);

  // intialize comms with GPS object
  Serial1.begin(9600);

  Wire.begin();

  // Set sensor pin as input
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
  // If CSV already exists, data will just be appended
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

  if(wifiFlag)
  {
    updateThingSpeak();
    Serial.println("Updated to ThingSpeak");
    Serial.println();
    wifiFlag = false;
    buttonISREn = true;
  }

}

// updates to ThingSpeak in bulk updates of 2kB of data
void updateThingSpeak()
{
  connectWiFi();

  memset(tspeak_buf, 0, sizeof(tspeak_buf));
  strcpy(tspeak_buf, "write_api_key=");
  strcat(tspeak_buf, writeApiKey);
  strcat(tspeak_buf, "&");
  strcat(tspeak_buf, "time_format=absolute&updates=");

  // Open file on SD card
  dataFile = SD.open(dataFileName, FILE_READ);
  if(dataFile)
  {
    uint32_t lineCount = 0;
    // keep track of total number of characters read, use to cut off at 2000 bytes
    // starts at 60 because of initial body parameters
    uint32_t charCount = 60; 
    uint32_t i = 0;
    uint32_t linePosition = 0; // location in file of most recent line
    
    while(dataFile.available())
    {
      char c = dataFile.read();
      charCount++;

      // Every 2kB, send an update to thing speak
      if(charCount >= 2000 && c != '\n')
      {
        httpRequest(tspeak_buf);

        // reset byte counter and thingspeak buffer
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
      }
      else if(c == '\n')
      {
        // on a new line, process data on line
        i = 0;

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

        // SD data is already formatted for ThingSpeak updates, just concatenate each line with pipe character in between
        strcat(tspeak_buf, sd_buf);
        strcat(tspeak_buf, "|"); // add pipe character between updates

        memset(sd_buf, 0, sizeof(sd_buf));
      }
      else
      {
        // fill up buffer with characters read
        sd_buf[i++] = c;
      }
    }

    // store the latest line read for next update
    // minus 1 because lineCount has been incremented, so at the end of the loop it's one line ahead
    lastLineRead = lineCount - 1;

    // update with the latest data
    httpRequest(tspeak_buf);

    // shutoff WiFi
    WiFi.end();
  }

  // TODO: write function here to update ThingSpeak with all the new values in CSV
  // Need to note the timestamp when this submission happens, then store that time stamp and only update data that is after that time to ThingSpeak
  
}

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
    dataFile.print("good");
    dataFile.println(",");
    dataFile.close();

    ledFlag = true;
  }
  else
  {
    Serial.println("Couldn't open file");
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
  sendGpsCommand("051,1");
}

void wakeGps()
{
  sendGpsCommand("051,0");
}

void sendGpsCommand(const char* cmd)
{
  char cmdBase[] = "PGKC";
  char* finalCmd = strcat(cmdBase, cmd); // data between the $ and * - on which checksum is based
  char checksum = createChecksum(finalCmd);

  Serial1.write('$');
  Serial1.write(finalCmd);
  Serial1.write('*');
  Serial1.write(checksum);
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