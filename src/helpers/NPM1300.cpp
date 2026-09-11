#include "NPM1300.h"

#include <MeshCore.h>   /* MESH_DEBUG_PRINTLN */

/*
 * Register map and scaling constants below are from the nPM1300 product
 * specification 4490_483 v1.1 -- the same document every PS section number in
 * this file refers to -- and cross-checked against
 * Nordic's own npmx driver (adk/npm1300_peripherals.h, drivers/src/npmx_adc.c).
 *
 * Each peripheral block has a base; registers within it are byte offsets.
 */

/*
 * VBUSIN -- used only to prove the part is alive, and for VBUS presence.
 * 0x02 is the VBUSIN bank; SYSTEM (which holds only SYSLABEL) is 0x01, so the
 * name this block used to carry would have sent the next SYSTEM register to
 * the wrong bank.
 */
#define NPM_BASE_VBUSIN           0x02
#define NPM_TASKUPDATEILIMSW      0x00   /* write 1: adopt VBUSINILIM0 now */
#define NPM_VBUSINILIM0           0x01   /* host-selected input current limit */
/*
 * VBUSINILIMSTARTUP (0x02) is deliberately NOT defined or written. It is the
 * value the part falls back to on a reset and on every USB replug (PS 6.1.3),
 * i.e. USB's pre-negotiation 100 mA compliance limit. Raising it would make the
 * board draw up to 500 mA from any host the instant VBUS appears, before source
 * detection and before firmware runs -- and it would survive into firmware that
 * never asked for it, because the PMIC keeps its state across an MCU reset. It
 * is the one setting that could not be undone by rebuilding without the
 * charging macros, so it is not made in the first place.
 */
#define NPM_VBUSINSTATUS          0x07   /* bit 0: VBUSINPRESENT */

/*
 * BCHARGER, PS 6.2.14. Note that BCHGISETDISCHARGEMSB/LSB sit immediately
 * after the charge-current pair at 0x0A/0x0B and reset to the 1 A discharge
 * limiter -- the TWI auto-increments, so every write below is a single byte
 * and none of them may be turned into a burst.
 */
#define NPM_BASE_BCHARGER         0x03
#define NPM_TASKRELEASEERR        0x00   /* W1: release the FSM from Error state */
#define NPM_BCHGENABLESET         0x04   /* W1S; reads back live state */
#define NPM_BCHGENABLECLR         0x05   /* W1C */
#define NPM_BCHGDISABLESET        0x06   /* W1S; reads back live state */
#define NPM_BCHGDISABLECLR        0x07   /* W1C */
#define NPM_BCHGISETMSB           0x08   /* floor(ICHG_mA / 4) */
#define NPM_BCHGISETLSB           0x09   /* (ICHG_mA / 2) & 1, the 2 mA step */
#define NPM_BCHGVTERM             0x0C   /* normal-region termination code */
#define NPM_BCHGVTERMR            0x0D   /* warm-region termination code */
#define NPM_DIETEMPSTOP           0x18   /* KDIETEMP[9:2] */
#define NPM_DIETEMPSTOPLSB        0x19   /* KDIETEMP[1:0] */
#define NPM_DIETEMPRESUME         0x1A
#define NPM_DIETEMPRESUMELSB      0x1B
#define NPM_BCHGCHARGESTATUS      0x34
#define NPM_BCHGERRREASON         0x36
#define NPM_BCHGERRSENSOR         0x37

/* BCHGENABLESET / BCHGENABLECLR bits (PS 6.2.14.4, 6.2.14.5) */
#define NPM_BCHGEN_CHARGING       0x01
#define NPM_BCHGEN_FULLCHGCOOL    0x02   /* 1 = full ICHG in the cool region */

/* BCHGDISABLESET / BCHGDISABLECLR bits (PS 6.2.14.6, 6.2.14.7) */
#define NPM_BCHGDIS_RECHARGE      0x01
#define NPM_BCHGDIS_NTC           0x02   /* 1 = JEITA thresholds ignored */

/*
 * BCHGERRREASON bits (PS 6.2.14.32):
 *   0 NTCSENSORERROR  1 VBATSENSORERROR  2 VBATLOW  3 VTRICKLE
 *   4 MEASTIMEOUT     5 CHARGETIMEOUT    6 TRICKLETIMEOUT
 *
 * REFUSE is the set that must never be retried automatically. The two timeouts
 * (7 h charge, 10 min trickle -- PS 6.2.7) are what a damaged or shorted cell
 * looks like, and the two sensor errors mean the part could not trust its own
 * measurement, which on a board with no thermistor is the last measurement
 * there is. RELEASE is the rest: VBATLOW, VTRICKLE and MEASTIMEOUT describe a
 * flat cell or a transient, not a dangerous one, and refusing on those would
 * permanently brick a node that merely ran itself down once.
 */
#define NPM_BCHGERR_REFUSE        0x63   /* bits 0,1,5,6 */
#define NPM_BCHGERR_RELEASE       0x1C   /* bits 2,3,4 */

/* BCHGCHARGESTATUS bits used internally; the public ones live in the header. */
#define NPM_CHARGESTATUS_TRICKLE  0x04
#define NPM_CHARGESTATUS_CC       0x08
#define NPM_CHARGESTATUS_CV       0x10

/* ADC */
#define NPM_BASE_ADC              0x05
#define NPM_ADCNTCRSEL            0x0A   /* PS 7.1.10.9; resets to 1 = NTC10K */
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
  if (!readReg(NPM_BASE_VBUSIN, NPM_VBUSINSTATUS, &dummy)) {
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
  if (!readReg(NPM_BASE_VBUSIN, NPM_VBUSINSTATUS, &status)) return false;
  return (status & 0x01) != 0;
}

bool NPM1300::isCharging() {
  uint8_t status;
  if (!readReg(NPM_BASE_BCHARGER, NPM_BCHGCHARGESTATUS, &status)) return false;
  return (status & (NPM_CHARGESTATUS_TRICKLE | NPM_CHARGESTATUS_CC | NPM_CHARGESTATUS_CV)) != 0;
}

/*
 * VTERM steps are not one uniform scale (PS 6.2.14.12): codes 0..3 are
 * 3.50/3.55/3.60/3.65 V and then the table jumps straight to 4.00 V at code 4.
 * Nothing exists in between, so a request landing in the 3.65..4.00 V gap
 * rounds DOWN to 3.65 V. Rounding is always downward -- for a cell, low is the
 * safe direction. Callers must range-check first: this cannot represent
 * anything below 3.50 V and would round a too-low request UP.
 */
static uint8_t npm_vterm_code(uint16_t mv) {
  if (mv >= 4450) return 13;
  if (mv >= 4000) return (uint8_t) (4 + (mv - 4000) / 50);
  if (mv >= 3650) return 3;
  return (uint8_t) ((mv - 3500) / 50);
}

/*
 * PS 6.2.14.12 again, in reverse, so a read-back can be reported in mV. Its
 * only caller is a MESH_DEBUG_PRINTLN, which compiles to nothing on a build
 * without MESH_DEBUG -- hence the attribute, so this file stays warning-clean
 * if anyone ever builds the helpers without platformio.ini's -w.
 */
__attribute__((unused))
static uint16_t npm_vterm_mv(uint8_t code) {
  if (code >= 14) return 3600;             /* 14 and 15 alias the 3.60 V default */
  if (code >= 4) return (uint16_t) (4000 + (code - 4) * 50);
  return (uint16_t) (3500 + code * 50);
}

/*
 * VBUS input current limit, rounded DOWN to a step the silicon is known to
 * implement.
 *
 * PS 6.1.8.2 tabulates codes 2, 3 and 4 as 200, 300 and 400 mA, but Nordic's
 * own generated register header (npmx adk/npm1300.h) names all three NOTUSED
 * and documents them as "100mA (reserved)". The PS and the vendor driver
 * disagree about the part, so those three codes are simply not issued: a
 * request between 100 and 499 mA lands on code 1 (100 mA), the value the two
 * sources agree such a code produces. PS 6.1.1 confirms 100 mA and 500 mA are
 * the only USB-compliant limits anyway; 600..1500 mA are the 100 mA steps.
 */
static uint8_t npm_vbus_ilim_code(uint16_t ma) {
  if (ma >= 1500) return 15;
  if (ma >= 600) return (uint8_t) (6 + (ma - 600) / 100);
  if (ma >= 500) return 5;
  return 1;   /* 100 mA: the lowest limit, and the only one below 500 mA */
}

const char* NPM1300::chargerResultStr(ChargerResult r) {
  switch (r) {
    case CHARGER_OK:            return "ok";
    case CHARGER_NO_PMIC:       return "PMIC not present";
    case CHARGER_BAD_CONFIG:    return "requested settings out of range";
    case CHARGER_BUS_ERROR:     return "I2C error";
    case CHARGER_LATCHED_ERROR: return "charger error latched from a previous cycle";
    case CHARGER_VERIFY_FAILED: return "registers did not read back as written";
    case CHARGER_DISABLE_FAILED: return "I2C error while switching the charger OFF -- state unknown";
  }
  return "unknown";
}

bool NPM1300::disableCharging() {
  _ichg_ma = 0;
  if (_wire == NULL) return false;
  return writeReg(NPM_BASE_BCHARGER, NPM_BCHGENABLECLR,
                  NPM_BCHGEN_CHARGING | NPM_BCHGEN_FULLCHGCOOL);
}

bool NPM1300::getChargeStatus(uint8_t* status) {
  return readReg(NPM_BASE_BCHARGER, NPM_BCHGCHARGESTATUS, status);
}

bool NPM1300::getChargeError(uint8_t* reason, uint8_t* sensor) {
  if (!readReg(NPM_BASE_BCHARGER, NPM_BCHGERRREASON, reason)) return false;
  return readReg(NPM_BASE_BCHARGER, NPM_BCHGERRSENSOR, sensor);
}

NPM1300::ChargerResult NPM1300::refuse(ChargerResult r) {
  _ichg_ma = 0;
  if (!writeReg(NPM_BASE_BCHARGER, NPM_BCHGENABLECLR,
                NPM_BCHGEN_CHARGING | NPM_BCHGEN_FULLCHGCOOL)) {
    /*
     * The refusal itself did not get through. Do not report the reason the
     * caller asked about as if the charger were now off -- it may still be
     * running with whatever the previous boot programmed.
     */
    return CHARGER_DISABLE_FAILED;
  }
  return r;
}

bool NPM1300::applyVbusLimit(uint16_t ma) {
  /*
   * PS 6.1.1 defaults this rail to 100 mA and PS 6.1.3 reverts it to that
   * default on every reset and every USB unplug/replug, so it has to be
   * re-asserted on each boot -- including the boot where the charger is already
   * running and nothing else is written. Only VBUSINILIM0 plus
   * TASKUPDATEILIMSW: the startup/default register is left alone on purpose
   * (see the comment on VBUSINILIMSTARTUP above).
   *
   * This is a throughput setting, not a cell-safety one. PS 6.2.10 gives the
   * system load priority and backs the charge current off on its own, so a
   * wrong value here charges slowly rather than dangerously -- which is why it
   * is not in the verify-or-refuse set.
   */
  if (!writeReg(NPM_BASE_VBUSIN, NPM_VBUSINILIM0, npm_vbus_ilim_code(ma))) return false;
  return writeReg(NPM_BASE_VBUSIN, NPM_TASKUPDATEILIMSW, 0x01);
}

NPM1300::ChargerResult NPM1300::configureCharger(const ChargerConfig& cfg) {
  _ichg_ma = 0;
  if (_wire == NULL) return CHARGER_NO_PMIC;

  /*
   * Range-check first, and refuse rather than clamp in the directions where
   * clamping would give the cell MORE than was asked for: a termination
   * voltage below 3.50 V and a charge current below 32 mA are simply not
   * representable, so honouring them would mean charging harder than the
   * caller intended. Refusing is the only honest answer.
   *
   * refuse() switches the charger off on the way out. That matters even here,
   * where nothing has been written yet: the PMIC does not reset with the MCU,
   * so "we have not touched it" is not the same as "it is off".
   */
  if (cfg.vterm_mv < 3500 || cfg.vterm_mv > 4450) return refuse(CHARGER_BAD_CONFIG);
  if (cfg.vterm_warm_mv < 3500 || cfg.vterm_warm_mv > cfg.vterm_mv) return refuse(CHARGER_BAD_CONFIG);
  if (cfg.ichg_ma < 32) return refuse(CHARGER_BAD_CONFIG);
  if (cfg.die_resume_k < cfg.die_stop_k) return refuse(CHARGER_BAD_CONFIG);  /* codes run backwards */
  if (cfg.die_stop_k > 1023 || cfg.die_resume_k > 1023) return refuse(CHARGER_BAD_CONFIG);

  /*
   * The one clamp that is allowed, because it moves the current DOWN: 800 mA
   * is the part's ceiling (PS 6.2.4). The caller reads the value back with
   * getChargeMilliAmps() and is expected to report it, so this is never
   * silent.
   */
  uint16_t ichg_ma = cfg.ichg_ma > 800 ? 800 : cfg.ichg_ma;
  uint8_t iset_msb = (uint8_t) (ichg_ma / 4);          /* PS 6.2.4: floor(ICHG/4) */
  uint8_t iset_lsb = (uint8_t) ((ichg_ma / 2) & 0x01); /* PS 6.2.4: 1 if ICHG/2 odd */
  uint16_t ichg_actual = (uint16_t) (((iset_msb * 2) + iset_lsb) * 2);

  uint8_t vterm_code  = npm_vterm_code(cfg.vterm_mv);
  uint8_t vtermr_code = npm_vterm_code(cfg.vterm_warm_mv);
  if (vtermr_code > vterm_code) vtermr_code = vterm_code;

  uint8_t ntc_sel   = (cfg.ntc == NTC_NONE) ? 0x00 : 0x01;
  uint8_t want_disable = (cfg.ntc == NTC_NONE) ? NPM_BCHGDIS_NTC : 0x00;

  /*
   * Every register that decides HOW the cell is charged. This table is used
   * twice: once to ask "is the part already set up exactly like this?" and
   * again, after writing, to prove it accepted the values before the charger
   * is allowed to start.
   *
   * `mask` is the width of the field the datasheet actually gives us (PS
   * 6.2.14.x: VTERM/VTERMR 4 bits, ISETLSB 1 bit, the DIETEMP LSBs 2 bits,
   * ADCNTCRSEL 2 bits, and two bits each in the enable/disable pairs). A whole-
   * byte comparison would turn any reserved or status bit that happens to read
   * back set into a spurious verify failure, which on the bench looks exactly
   * like a genuine fault.
   */
  struct { uint8_t base, offset, expect, mask; } check[] = {
    { NPM_BASE_ADC,      NPM_ADCNTCRSEL,       ntc_sel,      0x03 },
    { NPM_BASE_BCHARGER, NPM_BCHGDISABLESET,   want_disable,
      (uint8_t) (NPM_BCHGDIS_NTC | NPM_BCHGDIS_RECHARGE) },
    { NPM_BASE_BCHARGER, NPM_BCHGVTERM,        vterm_code,   0x0F },
    { NPM_BASE_BCHARGER, NPM_BCHGVTERMR,       vtermr_code,  0x0F },
    { NPM_BASE_BCHARGER, NPM_BCHGISETMSB,      iset_msb,     0xFF },
    { NPM_BASE_BCHARGER, NPM_BCHGISETLSB,      iset_lsb,     0x01 },
    { NPM_BASE_BCHARGER, NPM_DIETEMPSTOP,      (uint8_t) (cfg.die_stop_k >> 2),      0xFF },
    { NPM_BASE_BCHARGER, NPM_DIETEMPSTOPLSB,   (uint8_t) (cfg.die_stop_k & 0x03),    0x03 },
    { NPM_BASE_BCHARGER, NPM_DIETEMPRESUME,    (uint8_t) (cfg.die_resume_k >> 2),    0xFF },
    { NPM_BASE_BCHARGER, NPM_DIETEMPRESUMELSB, (uint8_t) (cfg.die_resume_k & 0x03),  0x03 },
  };
  const unsigned n_check = sizeof(check) / sizeof(check[0]);

  /*
   * Read the latched error state before deciding anything. Disabling the
   * charger does not touch these latches, so this is the previous cycle's
   * verdict and not something this call provoked.
   */
  uint8_t err_reason = 0, err_sensor = 0;
  if (!getChargeError(&err_reason, &err_sensor)) return refuse(CHARGER_BUS_ERROR);

  if (err_reason & NPM_BCHGERR_REFUSE) {
    /*
     * A charge or trickle timeout, or a sensor the part could not trust. Leave
     * the charger off and hand the decision to the operator. Deliberately NOT
     * cleared: PS 6.2.7 says the host "must make sure it is safe to charge"
     * first, and clearing at boot would convert a one-shot protection into an
     * unlimited retry loop against a failing cell.
     */
    MESH_DEBUG_PRINTLN("nPM1300 charger refusing: BCHGERRREASON=0x%02x BCHGERRSENSOR=0x%02x",
                       err_reason, err_sensor);
    return refuse(CHARGER_LATCHED_ERROR);
  }

  /*
   * FAST PATH. Look before writing.
   *
   * PS 6.2.7's 7 h charge and 10 min trickle safety timers are restarted every
   * time the charger is disabled and re-enabled, and they are the only
   * automatic stop this board has left once the JEITA interlock is gone. A node
   * that reboots more often than every 7 h -- not hypothetical on this hardware,
   * whose supply sags on transmit -- would otherwise keep resetting them and
   * could charge a failing cell forever. So if the part is already programmed
   * exactly as requested and already charging, leave it completely alone.
   */
  bool matches = true;
  for (unsigned i = 0; i < n_check; i++) {
    uint8_t got;
    if (!readReg(check[i].base, check[i].offset, &got)) return refuse(CHARGER_BUS_ERROR);
    if ((got & check[i].mask) != check[i].expect) { matches = false; break; }
  }
  if (matches) {
    uint8_t enabled;
    if (!readReg(NPM_BASE_BCHARGER, NPM_BCHGENABLESET, &enabled)) return refuse(CHARGER_BUS_ERROR);
    if ((enabled & (NPM_BCHGEN_CHARGING | NPM_BCHGEN_FULLCHGCOOL)) == NPM_BCHGEN_CHARGING) {
      /* The input limit did revert on this reset (PS 6.1.3), so re-assert it. */
      if (!applyVbusLimit(cfg.vbus_ilim_ma)) return refuse(CHARGER_BUS_ERROR);
      _ichg_ma = ichg_actual;
      MESH_DEBUG_PRINTLN("nPM1300 charger already configured and running: %u mA, term %u mV -- left untouched so the 7h safety timer keeps counting",
                         (unsigned) ichg_actual, (unsigned) npm_vterm_mv(vterm_code));
      return CHARGER_OK;
    }
  }

  /*
   * Something differs, so reconfigure from scratch -- and disable first,
   * unconditionally, before any of it.
   *
   * PS 6.2.4 requires the charger to be disabled before the current registers
   * are written, and the nPM1300 keeps its state across an MCU reset: it is the
   * MCU's own power source, so a reset, a DFU reboot or a reflash leaves the
   * charger exactly as the previous boot left it. Doing it here also means that
   * every `return` below leaves the charger OFF, whatever went wrong; nothing
   * re-enables it except the very last step, after the whole configuration has
   * been read back and checked. ENABLEFULLCHGCOOL is cleared in the same write:
   * it would remove the 50% current reduction in the cool JEITA region, and
   * nothing here ever wants that.
   */
  if (!writeReg(NPM_BASE_BCHARGER, NPM_BCHGENABLECLR,
                NPM_BCHGEN_CHARGING | NPM_BCHGEN_FULLCHGCOOL)) return CHARGER_DISABLE_FAILED;

  if (err_reason & NPM_BCHGERR_RELEASE) {
    /*
     * A flat cell (VBATLOW / VTRICKLE) or a measurement that timed out. The FSM
     * may be parked in its error state, and PS 6.2.7 distinguishes clearing the
     * reasons from releasing the charger, so release it -- otherwise a node that
     * once ran itself down would never charge again without the PMIC losing all
     * power, which on this board means removing USB and the cell together.
     * The reason bits are left latched on purpose so the history stays
     * readable; the timeout bits above are never released this way.
     */
    MESH_DEBUG_PRINTLN("nPM1300 charger: releasing recoverable error, BCHGERRREASON=0x%02x BCHGERRSENSOR=0x%02x",
                       err_reason, err_sensor);
    if (!writeReg(NPM_BASE_BCHARGER, NPM_TASKRELEASEERR, 0x01)) return refuse(CHARGER_BUS_ERROR);
  }

  /*
   * NTC before anything else, because PS 6.2.5 requires ADCNTCRSEL to match
   * the fitted thermistor "before enabling charging". Both directions are
   * driven: DISABLENTC lives in a write-1-to-set register whose clear is a
   * DIFFERENT register, so a build that switches from NTC_NONE back to NTC_10K
   * would otherwise keep ignoring the thermistor until the PMIC lost power.
   */
  if (!writeReg(NPM_BASE_ADC, NPM_ADCNTCRSEL, ntc_sel)) return refuse(CHARGER_BUS_ERROR);
  if (cfg.ntc == NTC_NONE) {
    if (!writeReg(NPM_BASE_BCHARGER, NPM_BCHGDISABLESET, NPM_BCHGDIS_NTC)) return refuse(CHARGER_BUS_ERROR);
    /* Recharge stays enabled; clear it in case a previous run set it. */
    if (!writeReg(NPM_BASE_BCHARGER, NPM_BCHGDISABLECLR, NPM_BCHGDIS_RECHARGE)) return refuse(CHARGER_BUS_ERROR);
  } else {
    if (!writeReg(NPM_BASE_BCHARGER, NPM_BCHGDISABLECLR,
                  NPM_BCHGDIS_NTC | NPM_BCHGDIS_RECHARGE)) return refuse(CHARGER_BUS_ERROR);
  }

  if (!writeReg(NPM_BASE_BCHARGER, NPM_BCHGVTERM, vterm_code)) return refuse(CHARGER_BUS_ERROR);
  if (!writeReg(NPM_BASE_BCHARGER, NPM_BCHGVTERMR, vtermr_code)) return refuse(CHARGER_BUS_ERROR);

  if (!writeReg(NPM_BASE_BCHARGER, NPM_BCHGISETMSB, iset_msb)) return refuse(CHARGER_BUS_ERROR);
  if (!writeReg(NPM_BASE_BCHARGER, NPM_BCHGISETLSB, iset_lsb)) return refuse(CHARGER_BUS_ERROR);

  /*
   * Die-temperature cutoff. On a board with no cell thermistor this is the
   * only temperature interlock left, so it is worth tightening from the 110 C
   * reset default -- but it measures the PMIC junction, not the cell, and a
   * cell heading for thermal runaway need not take the PMIC with it. Treat it
   * as protection for the charger and the PCB that happens to correlate with
   * ambient, never as cell-temperature protection.
   */
  if (!writeReg(NPM_BASE_BCHARGER, NPM_DIETEMPSTOP, (uint8_t) (cfg.die_stop_k >> 2))) return refuse(CHARGER_BUS_ERROR);
  if (!writeReg(NPM_BASE_BCHARGER, NPM_DIETEMPSTOPLSB, (uint8_t) (cfg.die_stop_k & 0x03))) return refuse(CHARGER_BUS_ERROR);
  if (!writeReg(NPM_BASE_BCHARGER, NPM_DIETEMPRESUME, (uint8_t) (cfg.die_resume_k >> 2))) return refuse(CHARGER_BUS_ERROR);
  if (!writeReg(NPM_BASE_BCHARGER, NPM_DIETEMPRESUMELSB, (uint8_t) (cfg.die_resume_k & 0x03))) return refuse(CHARGER_BUS_ERROR);

  /*
   * Verify before enabling. These registers all read back live state, and
   * reporting success on settings the part did not accept is the failure mode
   * this whole function exists to prevent -- so any disagreement leaves the
   * charger off.
   */
  for (unsigned i = 0; i < n_check; i++) {
    uint8_t got;
    if (!readReg(check[i].base, check[i].offset, &got)) return refuse(CHARGER_BUS_ERROR);
    if ((got & check[i].mask) != check[i].expect) {
      MESH_DEBUG_PRINTLN("nPM1300 charger verify failed: bank 0x%02x offset 0x%02x = 0x%02x, wanted 0x%02x (mask 0x%02x)",
                         check[i].base, check[i].offset, got, check[i].expect, check[i].mask);
      return refuse(CHARGER_VERIFY_FAILED);
    }
  }

  if (!writeReg(NPM_BASE_BCHARGER, NPM_BCHGENABLESET, NPM_BCHGEN_CHARGING)) {
    return refuse(CHARGER_BUS_ERROR);
  }

  /* BCHGENABLESET reads back live state, so this proves the charger is on and
   * that ENABLEFULLCHGCOOL stayed off. */
  uint8_t enabled;
  if (!readReg(NPM_BASE_BCHARGER, NPM_BCHGENABLESET, &enabled)) return refuse(CHARGER_BUS_ERROR);
  if ((enabled & (NPM_BCHGEN_CHARGING | NPM_BCHGEN_FULLCHGCOOL)) != NPM_BCHGEN_CHARGING) {
    return refuse(CHARGER_VERIFY_FAILED);
  }

  /* Last, so that a configuration which ends in a refusal never leaves the
   * input limit raised behind it. */
  if (!applyVbusLimit(cfg.vbus_ilim_ma)) return refuse(CHARGER_BUS_ERROR);

  _ichg_ma = ichg_actual;
  MESH_DEBUG_PRINTLN("nPM1300 charger on: %u mA, term %u mV (warm %u mV), VBUS limit %u mA, NTC %s",
                     (unsigned) ichg_actual, (unsigned) npm_vterm_mv(vterm_code),
                     (unsigned) npm_vterm_mv(vtermr_code), (unsigned) cfg.vbus_ilim_ma,
                     cfg.ntc == NTC_NONE ? "DISABLED (no cell temperature sensing)" : "10k");
  return CHARGER_OK;
}
