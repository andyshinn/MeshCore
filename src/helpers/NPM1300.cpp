#include "NPM1300.h"

/*
 * Register map and scaling constants below are from the nPM1300 product
 * specification v1.3 (section 7.1, "System monitor") and cross-checked against
 * Nordic's own npmx driver (adk/npm1300_peripherals.h, drivers/src/npmx_adc.c).
 *
 * Each peripheral block has a base; registers within it are byte offsets.
 */

/* SYSREG -- used only to prove the part is alive, and for VBUS presence. */
#define NPM_BASE_SYSREG           0x02
#define NPM_VBUSINSTATUS          0x07   /* bit 0: VBUS input present */

/* BCHARGER */
#define NPM_BASE_BCHARGER         0x03
#define NPM_BCHGCHARGESTATUS      0x34
#define NPM_CHARGESTATUS_TRICKLE  0x04
#define NPM_CHARGESTATUS_CC       0x08
#define NPM_CHARGESTATUS_CV       0x10

/* ADC */
#define NPM_BASE_ADC              0x05
#define NPM_TASKVBATMEASURE       0x00   /* write 1: start one-shot VBAT conversion */
#define NPM_TASKVSYSMEASURE       0x03   /* write 1: start one-shot VSYS conversion */
#define NPM_ADCVBATRESULTMSB      0x11   /* upper 8 bits of the 10-bit VBAT result */
#define NPM_ADCVSYSRESULTMSB      0x14   /* upper 8 bits of the 10-bit VSYS result */

/*
 * ADCGP0RESULTLSBS packs the low 2 bits of four channels into one byte:
 *   [1:0] VBAT   [3:2] NTC   [5:4] die temp   [7:6] VSYS
 */
#define NPM_ADCGP0RESULTLSBS      0x15
#define NPM_LSB_SHIFT_VBAT        0
#define NPM_LSB_SHIFT_VSYS        6

/*
 * Full-scale input voltages, PS section 7.1.9 "Electrical specification":
 *   VFSVBAT 5.0 V, VFSVSYS 6.375 V, VFSVBUS 7.5 V
 * The SAR is 10-bit and the PS divides by the full code range, not 1024.
 */
#define NPM_VFS_VBAT_MV           5000
#define NPM_VFS_VSYS_MV           6375
#define NPM_ADC_FULLSCALE         1023

/* PS 7.1.9: conversion time tCONV = 250us typ. Wait well past it. */
#define NPM_CONVERSION_WAIT_MICROS 1000

bool NPM1300::readReg(uint8_t base, uint8_t offset, uint8_t* value) {
  if (_wire == NULL) return false;

  _wire->beginTransmission(_addr);
  _wire->write(base);
  _wire->write(offset);
  /*
   * No stop between the address write and the read -- the nPM1300 latches the
   * register pointer for the following repeated-start read.
   */
  if (_wire->endTransmission(false) != 0) return false;

  if (_wire->requestFrom(_addr, (uint8_t) 1) != 1) return false;
  *value = _wire->read();
  return true;
}

bool NPM1300::writeReg(uint8_t base, uint8_t offset, uint8_t value) {
  if (_wire == NULL) return false;

  _wire->beginTransmission(_addr);
  _wire->write(base);
  _wire->write(offset);
  _wire->write(value);
  return _wire->endTransmission() == 0;
}

bool NPM1300::begin(TwoWire* wire, uint8_t addr) {
  _wire = wire;
  _addr = addr;
  _vbat = Cached();
  _vsys = Cached();

  /* Probe with a real register read; a bare address ACK is not proof of much. */
  uint8_t dummy;
  if (!readReg(NPM_BASE_SYSREG, NPM_VBUSINSTATUS, &dummy)) {
    _wire = NULL;
    return false;
  }
  return true;
}

uint16_t NPM1300::measure(Cached& cache, uint8_t task_offset, uint8_t msb_offset,
                          uint8_t lsb_shift, uint16_t full_scale_mv) {
  if (_wire == NULL) return 0;

  uint32_t now = millis();
  if (cache.mv != 0 && (now - cache.at) < CACHE_MILLIS) {
    return cache.mv;
  }

  if (!writeReg(NPM_BASE_ADC, task_offset, 1)) return 0;

  /*
   * The part does expose a completion event (EVENTSADCSET bit EVENTADCVBATRDY
   * and friends), but polling it costs a bus round trip -- longer than the
   * 250us conversion itself -- and would need the event cleared beforehand to
   * be meaningful. Waiting out the conversion is simpler and no slower.
   */
  delayMicroseconds(NPM_CONVERSION_WAIT_MICROS);

  /*
   * MSB and LSBs live in non-adjacent registers, so they need two reads. The
   * ADC is idle by now, so there is no risk of tearing across the pair.
   */
  uint8_t msb, lsbs;
  if (!readReg(NPM_BASE_ADC, msb_offset, &msb)) return 0;
  if (!readReg(NPM_BASE_ADC, NPM_ADCGP0RESULTLSBS, &lsbs)) return 0;

  uint16_t raw = ((uint16_t) msb << 2) | ((lsbs >> lsb_shift) & 0x03);

  cache.mv = (uint16_t) (((uint32_t) raw * full_scale_mv) / NPM_ADC_FULLSCALE);
  cache.at = now;
  return cache.mv;
}

uint16_t NPM1300::getBattMilliVolts() {
  return measure(_vbat, NPM_TASKVBATMEASURE, NPM_ADCVBATRESULTMSB,
                 NPM_LSB_SHIFT_VBAT, NPM_VFS_VBAT_MV);
}

uint16_t NPM1300::getSysMilliVolts() {
  return measure(_vsys, NPM_TASKVSYSMEASURE, NPM_ADCVSYSRESULTMSB,
                 NPM_LSB_SHIFT_VSYS, NPM_VFS_VSYS_MV);
}

bool NPM1300::isVbusPresent() {
  uint8_t status;
  if (!readReg(NPM_BASE_SYSREG, NPM_VBUSINSTATUS, &status)) return false;
  return (status & 0x01) != 0;
}

bool NPM1300::isCharging() {
  uint8_t status;
  if (!readReg(NPM_BASE_BCHARGER, NPM_BCHGCHARGESTATUS, &status)) return false;
  return (status & (NPM_CHARGESTATUS_TRICKLE | NPM_CHARGESTATUS_CC | NPM_CHARGESTATUS_CV)) != 0;
}
