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

  NPM1300() : _wire(NULL), _addr(DEFAULT_ADDR), _ichg_ma(0) {}

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

  /* BCHGCHARGESTATUS bits, PS 6.2.14.31. */
  static const uint8_t CHG_BATTERY_DETECTED = 0x01;
  static const uint8_t CHG_COMPLETED        = 0x02;  /* battery full */
  static const uint8_t CHG_TRICKLE          = 0x04;
  static const uint8_t CHG_CONSTANT_CURRENT = 0x08;
  static const uint8_t CHG_CONSTANT_VOLTAGE = 0x10;
  static const uint8_t CHG_RECHARGE         = 0x20;
  static const uint8_t CHG_DIETEMP_PAUSED   = 0x40;  /* stopped, die too hot */
  static const uint8_t CHG_SUPPLEMENT       = 0x80;

  /*
   * KDIETEMP codes for the charger's own thermal cutoff, copied verbatim from
   * PS Table 15 (section 6.2.6) rather than recomputed from the equation, so
   * there is no transcription of a formula to get wrong. Note the codes run
   * BACKWARDS: a larger code is a LOWER temperature, so the resume code must be
   * greater than or equal to the stop code.
   *
   * This watches the PMIC's own junction. It is NOT a cell temperature sensor
   * and must never be described as one -- see the NtcType comment below.
   */
  static const uint16_t DIETEMP_K_50C  = 435;
  static const uint16_t DIETEMP_K_60C  = 422;
  static const uint16_t DIETEMP_K_70C  = 410;
  static const uint16_t DIETEMP_K_80C  = 397;
  static const uint16_t DIETEMP_K_90C  = 384;
  static const uint16_t DIETEMP_K_100C = 372;
  /*
   * 359 is Table 15's tabulated 110 C code. It is NOT the reset value: PS
   * 6.2.14.24/.25 reset DIETEMPSTOP/DIETEMPSTOPLSB to 0x5A/0x00, i.e. code 360,
   * one step away. Do not "fix" a read-back of 360 to match this constant.
   */
  static const uint16_t DIETEMP_K_110C = 359;

  /*
   * What is wired to the PMIC's NTC pin.
   *
   * PS 6.2.5: "The host software must select the corresponding setting that
   * matches the battery thermistor before enabling charging in register
   * ADCNTCRSEL", and "If a thermistor is not used, the NTC pin must be tied
   * directly to ground or through a resistor. The functionality must be
   * disabled in register BCHGDISABLESET."
   *
   * NTC_NONE throws away the ENTIRE JEITA profile of PS Table 14: no cold
   * cutoff, no 50% current reduction in the cool region, no reduced
   * termination voltage in the warm region, and no cutoff above 60 C. The cell
   * is then charged at full current and full VTERM at any temperature. Do not
   * select it to "make charging work" -- select it only when you have
   * established that the board physically has no thermistor, and only with the
   * caller's own explicit opt-in.
   *
   * Only these two values exist because NTCCOLD/COOL/WARM/HOT are left at their
   * reset values, which PS Table 13 calibrates for a 10 kOhm/B3380 part. A 47k
   * or 100k thermistor needs all four threshold pairs recomputed and is
   * deliberately not supported here.
   */
  enum NtcType {
    NTC_NONE = 0,   /* ADCNTCRSEL Hi_Z + DISABLENTC set in BCHGDISABLESET */
    NTC_10K  = 1,   /* ADCNTCRSEL 10K + DISABLENTC cleared; JEITA defaults apply */
  };

  /*
   * Everything configureCharger() programs. Defaults are the least-energetic
   * settings the part supports, so a caller that forgets a field gets a slow,
   * low-voltage charge rather than a hot one.
   */
  struct ChargerConfig {
    uint16_t vterm_mv;       /* normal-region termination, 3500..4450 */
    uint16_t vterm_warm_mv;  /* warm-region termination, never above vterm_mv */
    uint16_t ichg_ma;        /* charge current, 32..800, quantised down to 2 mA */
    uint16_t vbus_ilim_ma;   /* VBUS input current limit, rounded down to a step */
    uint16_t die_stop_k;     /* DIETEMP_K_* at which charging stops */
    uint16_t die_resume_k;   /* DIETEMP_K_* at which it resumes; >= die_stop_k */
    NtcType  ntc;

    ChargerConfig()
      : vterm_mv(3500), vterm_warm_mv(3500), ichg_ma(32), vbus_ilim_ma(100),
        die_stop_k(DIETEMP_K_60C), die_resume_k(DIETEMP_K_50C), ntc(NTC_10K) {}
  };

  enum ChargerResult {
    CHARGER_OK = 0,
    CHARGER_NO_PMIC,        /* begin() never succeeded; nothing was written */
    CHARGER_BAD_CONFIG,     /* asked for something outside the part's range */
    CHARGER_BUS_ERROR,      /* an I2C transaction failed partway through */
    CHARGER_LATCHED_ERROR,  /* a charge/trickle timeout or sensor error is latched */
    CHARGER_VERIFY_FAILED,  /* a register did not read back as written */
    /*
     * The charger was refused AND the write that should have switched it off
     * failed, so its state is unknown and it may still be charging. Distinct
     * from every other failure above, all of which end with the charger off --
     * a caller must not print "the cell will not charge" for this one.
     */
    CHARGER_DISABLE_FAILED,
  };

  /*
   * Program the charger and enable it, or leave it DISABLED and say why.
   *
   * The part resets with the charger off, ICHG 32 mA and VTERM 3.60 V (PS
   * 6.2.2, 6.2.4, 6.2.14.12), so without this a cell on VBAT never charges.
   *
   * EVERY failure path leaves the charger off. That includes a bad config, a
   * bus error and a failed read-back -- there is exactly one return that leaves
   * it running (see the fast path below) and exactly one that turns it on.
   * The single exception is CHARGER_DISABLE_FAILED, which says the shutdown
   * write itself did not get through.
   *
   * Ordering is not incidental. PS 6.2.4: "CHARGER must be disabled before
   * changing the current setting in registers BCHGISETMSB and BCHGISETLSB. The
   * setting takes effect when charging is enabled." The PS does not say the
   * write is refused while charging -- Nordic's own npmx writes those registers
   * with no enable check -- so the likely behaviour is that the register file
   * updates but the analog setting does not re-latch mid-cycle. That matters
   * here: the read-back below proves the part ACCEPTED the register values, not
   * that the charger latched them, which is why the disable is mandatory rather
   * than something the verify could catch after the fact.
   *
   * It matters more than it looks, because the nPM1300 is the MCU's own power
   * source and does NOT reset when the MCU does -- an MCU reset, a DFU reboot,
   * the reset button and a reflash all leave the charger exactly as the
   * previous boot left it. Never assume a cold part.
   *
   * FAST PATH, and why it exists. PS 6.2.7 gives the charger a 7 h charge and a
   * 10 min trickle safety timer; they are the part's only automatic defence
   * against a cell that will not take charge, and disabling and re-enabling the
   * charger restarts them. A node that reboots more often than every 7 h would
   * therefore never let that timer expire. So the configuration is read back
   * FIRST: if every register already holds the requested value and the charger
   * is already running, nothing is written, the charge continues and the safety
   * timer keeps counting. The teardown happens only when something differs.
   *
   * A latched error in BCHGERRREASON is read before either path. The two
   * timeout bits and the two sensor-error bits leave the charger off and are
   * never cleared here -- PS 6.2.7 puts the "make sure it is safe to charge"
   * decision on the host, and clearing them at boot would turn a one-shot
   * protection into an unlimited retry against a failing cell or a blind
   * sensor. The remaining reasons (VBATLOW, VTRICKLE, MEASTIMEOUT) describe a
   * flat cell rather than a damaged one; those are logged and released with
   * TASKRELEASEERR so a deeply discharged node can recover on its own.
   *
   * Idempotent: every field is driven to an absolute value, including the
   * write-1-to-set/write-1-to-clear pairs, so repeated boots converge.
   */
  ChargerResult configureCharger(const ChargerConfig& cfg);

  /* Human-readable form of the above, for logging. Never NULL. */
  static const char* chargerResultStr(ChargerResult r);

  /* Stop charging. Also clears ENABLEFULLCHGCOOL. */
  bool disableCharging();

  /*
   * The charge current THIS boot programmed (or found already programmed), in
   * mA, quantised to what the part actually received -- log this, not the
   * request. Zero means this object did not configure the charger; it does NOT
   * mean the charger is off, because the PMIC keeps running across an MCU
   * reset. Read BCHGENABLESET if you need to know what the hardware is doing.
   */
  uint16_t getChargeMilliAmps() const { return _ichg_ma; }

  /* Raw BCHGCHARGESTATUS; decode with the CHG_* masks above. */
  bool getChargeStatus(uint8_t* status);

  /* Raw BCHGERRREASON (PS 6.2.14.32) and BCHGERRSENSOR (PS 6.2.14.33). */
  bool getChargeError(uint8_t* reason, uint8_t* sensor);

private:
  static const uint32_t CACHE_MILLIS = 1000;

  struct Cached {
    uint16_t mv;
    uint32_t at;
    Cached() : mv(0), at(0) {}
  };

  TwoWire* _wire;
  uint8_t _addr;
  uint16_t _ichg_ma;   /* what the charger was actually programmed with, 0 = off */
  Cached _vbat;
  Cached _vsys;

  /*
   * Trigger one ADC channel and convert its 10-bit result to millivolts.
   * `lsb_shift` is where that channel's low 2 bits sit in ADCGP0RESULTLSBS.
   */
  uint16_t measure(Cached& cache, uint8_t task_offset, uint8_t msb_offset,
                   uint8_t lsb_shift, uint16_t full_scale_mv);

  /*
   * Switch the charger off and return `r`, or CHARGER_DISABLE_FAILED if even
   * that write did not get through. Every refusal in configureCharger() goes
   * through here, so "refused" always means "and it is off".
   */
  ChargerResult refuse(ChargerResult r);

  /* VBUS input current limit; a throughput setting, not a cell-safety one. */
  bool applyVbusLimit(uint16_t ma);

  bool readReg(uint8_t base, uint8_t offset, uint8_t* value);
  bool writeReg(uint8_t base, uint8_t offset, uint8_t value);
};
