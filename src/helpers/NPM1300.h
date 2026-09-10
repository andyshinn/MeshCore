#pragma once

#include <Arduino.h>
#include <Wire.h>

/*
 * Nordic nPM1300 PMIC -- system monitoring only.
 *
 * This is not a full nPM1300 driver: it covers the ADC channels and status
 * bits a MeshCore board actually needs and leaves the regulators, LEDs, GPIOs
 * and ship-mode alone. Boards that need those should talk to the part directly.
 *
 * Registers are addressed as a 16-bit big-endian {base, offset} pair, which is
 * why this uses TwoWire directly rather than Adafruit_I2CDevice -- the write
 * is always two address bytes followed by the data.
 *
 * On the XIAO nRF54LM20A the PMIC sits on its own private I2C bus (P1.18/P1.17,
 * exposed by the Arduino core as Wire1), not on the header bus.
 */
class NPM1300 {
public:
  static const uint8_t DEFAULT_ADDR = 0x6B;

  NPM1300() : _wire(NULL), _addr(DEFAULT_ADDR) {}

  /* Returns false if the part does not ACK. `wire` must already be begin()'d. */
  bool begin(TwoWire* wire, uint8_t addr = DEFAULT_ADDR);

  bool isPresent() const { return _wire != NULL; }

  /*
   * Battery voltage in millivolts, or 0 if unavailable. This is VBAT at the
   * cell, ahead of the power path.
   *
   * Triggers a one-shot conversion and waits for it, so the call blocks for a
   * few hundred microseconds. Results are cached for CACHE_MILLIS to keep
   * repeated telemetry/UI reads from re-triggering the ADC every time.
   */
  uint16_t getBattMilliVolts();

  /*
   * System rail (VSYS) in millivolts, or 0 if unavailable. VSYS is the PMIC's
   * internal power-path output that feeds the bucks -- roughly VBAT on battery
   * and roughly the USB rail when VBUS is present, so it is what tells you
   * which source is actually carrying the board. Same caching as above.
   */
  uint16_t getSysMilliVolts();

  /* True when USB (VBUS) is supplying the board. */
  bool isVbusPresent();

  /* True while the charger is in trickle, constant-current or constant-voltage. */
  bool isCharging();

private:
  static const uint32_t CACHE_MILLIS = 1000;

  struct Cached {
    uint16_t mv;
    uint32_t at;
    Cached() : mv(0), at(0) {}
  };

  TwoWire* _wire;
  uint8_t _addr;
  Cached _vbat;
  Cached _vsys;

  /*
   * Trigger one ADC channel and convert its 10-bit result to millivolts.
   * `lsb_shift` is where that channel's low 2 bits sit in ADCGP0RESULTLSBS.
   */
  uint16_t measure(Cached& cache, uint8_t task_offset, uint8_t msb_offset,
                   uint8_t lsb_shift, uint16_t full_scale_mv);

  bool readReg(uint8_t base, uint8_t offset, uint8_t* value);
  bool writeReg(uint8_t base, uint8_t offset, uint8_t value);
};
