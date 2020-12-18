#include "HM3301.h"

// empty constructor
HM3301::HM3301() {};

// send initial command to dust sensor
bool HM3301::begin(uint8_t i2c_addr)
{
  bool initSuccess;
  Wire.beginTransmission(HM3301_I2C_ADDR_DEFAULT);
  Wire.write(HM3301_SELECT_COMM); // select command
  initSuccess = Wire.endTransmission();
  // endTransmission() returns 0 on a success

  return !initSuccess;
}

// returns false if there's a timeout of 10ms
// or if the checksum is invalid
bool HM3301::readRaw(uint8_t *raw_data, uint32_t data_len)
{
  uint32_t timeOutCnt = 0;
  Wire.requestFrom(HM3301_I2C_ADDR_DEFAULT, data_len);
  while(data_len != Wire.available())
  {
    timeOutCnt++;
    if(timeOutCnt > 10) return false;
    delay(1);
  }
  for(int i = 0; i < data_len; i++)
  {
    raw_data[i] = Wire.read();
  }

  // verify checksum
  byte sum = 0;

  for(int i = 0; i< 28; i++)
  {
    sum += raw_data[i];
  }
  if(sum != raw_data[28])
  {
    return false;
  }
}

bool HM3301::read()
{
  uint8_t raw_data[29];
  if(!readRaw(raw_data)) return false;

  // parse data into HM3301_data_t struct
  data.PM1p0_std = (raw_data[4] << 8 | raw_data[5]);
  data.PM2p5_std = (raw_data[6] << 8 | raw_data[7]);
  data.PM10p0_std = (raw_data[8] << 8 | raw_data[9]);
  data.PM1p0_atm = (raw_data[10] << 8 | raw_data[11]);
  data.PM2p5_atm = (raw_data[12] << 8 | raw_data[13]);
  data.PM10p0_atm = (raw_data[14] << 8 | raw_data[15]);
  data.count_0p3um = (raw_data[16] << 8 | raw_data[17]);
  data.count_0p5um = (raw_data[18] << 8 | raw_data[19]);
  data.count_1p0um = (raw_data[20] << 8 | raw_data[21]);
  data.count_2p5um = (raw_data[22] << 8 | raw_data[23]);
  data.count_5p0um = (raw_data[24] << 8 | raw_data[25]);
  data.count_10p0um = (raw_data[26] << 8 | raw_data[27]);

  return true;
}

bool HM3301::read(uint16_t *data)
{
  int j = 0;
  uint8_t raw_data[29];
  if(!readRaw(raw_data)) return false;

  for(int i = 4; i <=26 ; i += 2)
  {
    data[j] = (raw_data[i] << 8) | (raw_data[i+1]);
    j++;
  }

  return true;
}