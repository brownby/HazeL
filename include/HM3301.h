#ifndef HM3301_H
#define HM3301_H

#include <Arduino.h>
#include <Wire.h>

#define HM3301_I2C_ADDR_DEFAULT 0x40
#define HM3301_SELECT_COMM 0x88

typedef struct {
  uint16_t PM1p0_std;
  uint16_t PM2p5_std;
  uint16_t PM10p0_std;
  uint16_t PM1p0_atm;
  uint16_t PM2p5_atm;
  uint16_t PM10p0_atm;
  uint16_t count_0p3um;
  uint16_t count_0p5um;
  uint16_t count_1p0um;
  uint16_t count_2p5um;
  uint16_t count_5p0um;
  uint16_t count_10p0um;
} HM3301_data_t;

class HM3301 {
public:
  HM3301(void);
  bool begin(uint8_t i2c_addr = HM3301_I2C_ADDR_DEFAULT);
  bool read();
  bool read(uint16_t *data);

  HM3301_data_t data;

private:
  bool readRaw(uint8_t *raw_data, uint32_t data_len = 29);
};


#endif