#pragma once

#include <MeshCore.h>
#include <Arduino.h>

#if defined(NRF54_PLATFORM)
  #include <helpers/NRF54Board.h>
  #include <helpers/NPM1300.h>
  using MMMBoardBase  = NRF54BoardDCDC;
  using MMMBoardVBase = NRF54Board;    // the virtual base, for ctor init
#else
  #include <helpers/NRF52Board.h>
  using MMMBoardBase  = NRF52BoardDCDC;
  using MMMBoardVBase = NRF52Board;
#endif

#define  PIN_VBAT_READ 17
#define  ADC_MULTIPLIER   (1.815f) // dependent on voltage divider resistors. TODO: more accurate battery tracking

/* ===========================================================================
 * Battery charging -- XIAO nRF54LM20A / nPM1300 only.
 *
 * OFF UNLESS THE BUILD OPTS IN. The nPM1300 resets with its charger disabled
 * (32 mA, 3.60 V termination, PS 6.2.2/6.2.4/6.2.14.12) and nothing else on
 * this board changes that, so a cell on the XIAO's BAT pads simply never
 * charges. If BATT_CAPACITY_MAH is not defined, MMMBoard actively switches the
 * charger OFF at boot and says so -- it does not merely decline to touch it.
 * That distinction is the whole off-switch: the nPM1300 is the MCU's own power
 * source and does NOT reset when the nRF54 does, so a build that once opted in
 * leaves the charger enabled with the old current and termination voltage
 * straight through a reset, a DFU reflash and a downgrade. Deleting
 * BATT_CAPACITY_MAH and reflashing is the only way a user has to stop charging,
 * and it has to actually stop it.
 * There is no sane default charge current for a cell nobody has named.
 *
 * WHY A SECOND, UGLIER MACRO IS REQUIRED.
 * This board cannot measure cell temperature. The nPM1300's NTC pin is wired
 * to R6, a FIXED 10 kOhm resistor to ground (Seeed XIAO nRF54LM20A V1.0
 * schematic, sheet "03 Power"), and the BAT+/BAT- pads are two conductors with
 * nowhere for a pack thermistor to land. With the reset ADCNTCRSEL setting
 * that fixed resistor reads as a constant code of 512 (PS 6.2.5's equation,
 * KNTCTEMP = 1024 * RT / (RT + RB), with RT = RB = 10k), which sits inside the
 * nominal band (PS Table 13: NTCWARM 337 .. NTCCOOL 658), so the part believes
 * the cell is at exactly 25 C forever and charges at full current at any real
 * temperature. The JEITA interlock of PS Table 14 is therefore already defeated
 * in copper, before firmware gets a say: no cold cutoff, no cool-region current
 * reduction, no 60 C hot cutoff. Enabling charging here means charging a
 * lithium cell with NO cell-temperature protection, and no register setting can
 * change that.
 *
 * Because that risk is soldered in rather than chosen, it has to be
 * acknowledged rather than defaulted. Define BOTH macros to charge:
 *
 *   -D BATT_CAPACITY_MAH=<rated mAh of the fitted cell>
 *   -D BATT_CHARGE_WITHOUT_TEMPERATURE_SENSING=1
 *
 * Before defining the second one, satisfy PS 3.4, which requires a pack
 * connected to VBAT to carry overvoltage, undervoltage and overcurrent
 * protection AND "a thermal fuse to protect from overtemperature (if NTC
 * thermistor is not present)". On this board that pack protection is the only
 * cell-temperature protection in the system. A bare unprotected pouch cell is
 * not safe here.
 *
 * What the firmware does instead of pretending: it selects ADCNTCRSEL = Hi_Z
 * and sets DISABLENTC, which is what PS 6.2.5 requires of a board wired this
 * way, so NTCSTATUS reports "ignored" rather than a fictitious 25 C; and it
 * tightens the charger's die-temperature cutoff from the 110 C reset default
 * to 60 C stop / 50 C resume. That die sensor is NOT a cell sensor -- it
 * watches the PMIC junction -- but at these charge currents the linear charger
 * dissipates well under a quarter of a watt, so the die tracks board ambient
 * closely and a 60 C cutoff lands near the JEITA hot threshold it is standing
 * in for. It errs toward refusing to charge in a hot enclosure. That is the
 * intended behaviour; if the node stops charging on a hot day, that is the
 * interlock working, not a bug to tune away.
 * ===========================================================================
 */
/*
 * #error, not #warning: [arduino_base] in the top-level platformio.ini puts -w
 * in build_flags, which every env inherits, and -w silences #warning entirely
 * (verified with the arm-none-eabi-gcc this tree links with). A silent warning
 * would let someone hoist BATT_CAPACITY_MAH into a shared ini section, get no
 * feedback at all on the four nRF52 MMM envs, and reasonably conclude their
 * cells were charging. Failing the build is both louder and the fail-safe
 * direction: defining this macro on a board with no PMIC is always a mistake.
 */
#if defined(BATT_CAPACITY_MAH) && !defined(NRF54_PLATFORM)
  #error "BATT_CAPACITY_MAH is set on a Modular Mesh Maker build that has no nPM1300 (nRF52). Nothing here can charge a cell. Remove it, or move it into the nRF54 env that owns the PMIC."
#endif

#if defined(NRF54_PLATFORM) && defined(BATT_CAPACITY_MAH)

  /*
   * Exactly 1, not merely "defined". `#ifndef` would accept
   * -D BATT_CHARGE_WITHOUT_TEMPERATURE_SENSING=0 -- someone writing an explicit
   * "no" -- as consent, which is the one thing an acknowledgement macro must
   * never do. The error text has to say so, because editing the ini line from
   * =1 to =0 is exactly how a person tries to turn this off.
   */
  #if !defined(BATT_CHARGE_WITHOUT_TEMPERATURE_SENSING) || ((BATT_CHARGE_WITHOUT_TEMPERATURE_SENSING) != 1)
    #error "BATT_CAPACITY_MAH is set, but BATT_CHARGE_WITHOUT_TEMPERATURE_SENSING is not defined as exactly 1. This board has no battery thermistor (the nPM1300 NTC pin is a fixed 10k resistor), so charging would run with no cell-temperature protection. Setting this macro to 0 does NOT disable charging -- remove BATT_CAPACITY_MAH to leave the charger off. To opt in, confirm the pack has its own protection circuit including a thermal fuse (nPM1300 PS 3.4) and set -D BATT_CHARGE_WITHOUT_TEMPERATURE_SENSING=1."
  #endif

  /*
   * 0.5C is the conventional lithium-polymer charge rate and the one a cell's
   * own datasheet is specified at, so it is what a stated capacity actually
   * licenses. It is not raised to "charge faster": with no cell-temperature
   * protection the only lever left is to put less energy per unit time into the
   * cell, and a higher rate also means more heat in a linear charger sitting
   * over a volt above the cell.
   *
   * Below 64 mAh, 0.5C falls under the part's 32 mA floor (PS 6.2.4), so the
   * nPM1300 physically cannot charge such a cell at or below 0.5C. Refuse
   * rather than clamp upward -- clamping would silently charge a 40 mAh cell at
   * 0.8C, the one direction this must never round.
   */
  #if (BATT_CAPACITY_MAH) < 64
    #error "BATT_CAPACITY_MAH is below 64 mAh: 0.5C would be under the nPM1300's 32 mA minimum charge current, so this part cannot charge that cell at a safe rate. Do not raise the macro to work around this."
  #endif

  #define BATT_CHARGE_C_RATE_DIVISOR          2      /* 0.5C */
  #define BATT_CHARGE_IDEAL_MILLIAMPS         ((BATT_CAPACITY_MAH) / BATT_CHARGE_C_RATE_DIVISOR)

  /*
   * Two independent ceilings, and the lower one wins.
   *
   * 150 mA is the board ceiling: it is Seeed's own configured charge current
   * for this exact module in their upstream Zephyr board file, and it is a
   * defensible hard cap for a build whose cell size is not known to this code
   * beyond one macro someone typed.
   *
   * The VBUS ceiling is the input budget. The nPM1300 takes charge current from
   * the same VBUS rail that runs the node, and PS 6.1.1/6.1.3 default that rail
   * to 100 mA and revert to it on every USB replug, so the limit is programmed
   * to 500 mA below. Reserving 300 mA for the MCU plus the LR2021 leaves 200 mA
   * of input headroom for charging. PS 6.2.10 says system load takes priority
   * and charge current backs off on its own, so this is belt-and-braces rather
   * than the thing keeping the rail up -- but budgeting it explicitly beats
   * discovering it.
   */
  #define BATT_CHARGE_BOARD_MAX_MILLIAMPS     150
  #define BATT_CHARGE_VBUS_LIMIT_MILLIAMPS    500
  #define BATT_CHARGE_SYSTEM_RESERVE_MILLIAMPS 300
  #define BATT_CHARGE_VBUS_HEADROOM_MILLIAMPS \
    ((BATT_CHARGE_VBUS_LIMIT_MILLIAMPS) - (BATT_CHARGE_SYSTEM_RESERVE_MILLIAMPS))

  #define BATT_CHARGE_CEILING_MILLIAMPS \
    ((BATT_CHARGE_BOARD_MAX_MILLIAMPS) < (BATT_CHARGE_VBUS_HEADROOM_MILLIAMPS) \
      ? (BATT_CHARGE_BOARD_MAX_MILLIAMPS) : (BATT_CHARGE_VBUS_HEADROOM_MILLIAMPS))

  /* Clamped down, never up. The clamp is reported at boot, never silent. */
  #define BATT_CHARGE_MILLIAMPS \
    ((BATT_CHARGE_IDEAL_MILLIAMPS) > (BATT_CHARGE_CEILING_MILLIAMPS) \
      ? (BATT_CHARGE_CEILING_MILLIAMPS) : (BATT_CHARGE_IDEAL_MILLIAMPS))

  /*
   * Chemistry is fixed as LiPo for these boards, so termination is fixed here
   * too and is deliberately NOT a build macro -- a settable termination voltage
   * is the single most dangerous knob to expose and has no legitimate use once
   * the chemistry is decided. 4150 mV is an exact step of the part's own
   * non-uniform VTERM table (PS 6.2.14.12, code 7), so nothing rounds.
   *
   * WHY 4150 AND NOT THE USUAL 4200. PS Table 14 derates termination to VTERMR
   * in the warm JEITA region, which is where a hot, full cell is supposed to be
   * protected. This board can never enter that region -- DISABLENTC is set
   * because there is no thermistor -- so the cell terminates at the SAME
   * voltage at 0 C and at 55 C, and recharge is left enabled, so a
   * mains-powered node holds it near termination indefinitely. High state of
   * charge plus heat is the textbook recipe for a swollen pouch. Giving up
   * about 4% of capacity buys back some of the margin the missing JEITA
   * derating used to provide. Seeed'"'"'s own Zephyr board file for this exact
   * module -- the same source this file cites for the 150 mA ceiling --
   * specifies term-microvolt = 4150000, so this also stops us from taking
   * Seeed'"'"'s current while quietly departing from their voltage.
   *
   * VTERMR is inert on THIS board: the warm region is unreachable, so the value
   * below is never applied to anything. It is written anyway so the register
   * holds something defensible rather than its 3.60 V reset value, and so the
   * verify set has a known value to check. A variant that wires a real
   * thermistor must set its own, from its own build.
   */
  #define BATT_CHARGE_VTERM_MILLIVOLTS        4150
  #define BATT_CHARGE_VTERM_WARM_MILLIVOLTS   4000

#endif  /* NRF54_PLATFORM && BATT_CAPACITY_MAH */

class MMMBoard : public MMMBoardBase {
protected:
  uint8_t btn_prev_state;
  float adc_mult = ADC_MULTIPLIER;
#if defined(NRF54_PLATFORM)
  NPM1300 pmic;

  /*
   * What configureBatteryCharger() concluded, kept so onBootComplete() can say
   * it again once a terminal is actually attached.
   */
  NPM1300::ChargerResult chg_result = NPM1300::CHARGER_NO_PMIC;
  bool chg_attempted = false;

  /*
   * Program and enable the nPM1300 charger, or switch it off and explain why.
   * Called from begin() only after pmic.begin() has actually succeeded. When
   * the build did not opt in with BATT_CAPACITY_MAH this still writes
   * BCHGENABLECLR -- see the header comment above; "untouched" is not "off" on
   * a PMIC that outlives the MCU'"'"'s reset.
   */
  void configureBatteryCharger();

  /*
   * Print what the charger is doing (or refusing to do). `sample_status` reads
   * BCHGCHARGESTATUS as well, which is only worth doing once the part has had
   * a few seconds to run its own battery detection.
   */
  void reportBatteryCharger(bool sample_status);
#endif

public:
  MMMBoard() : MMMBoardVBase("MMM_OTA") {}
  void begin();

#if defined(NRF54_PLATFORM)
  /*
   * Re-state the charger'"'"'s verdict at the END of setup().
   *
   * begin() runs about a second after boot, and on USB CDC the host has
   * usually not opened the port yet -- simple_repeater'"'"'s own
   * "give some extra time for serial to settle" delay(5000) comes AFTER
   * board.begin(), so anything printed from there can be lost. A safety
   * refusal nobody can read is the same as no refusal at all, so the verdict
   * is repeated here, inside the window the firmware itself created for
   * exactly this. Sampling the charge status here also gives the part several
   * seconds to finish its battery-detection cycle, which it has certainly not
   * done in the same millisecond the charger was enabled.
   */
  void onBootComplete() override;
#endif

  #define BATTERY_SAMPLES 8

  uint16_t getBattMilliVolts() override {
  #if defined(NRF54_PLATFORM)
    /*
     * The XIAO nRF54LM20A has no GPIO resistor divider; battery voltage comes
     * from the on-board nPM1300 PMIC on its private I2C bus (Wire1). The PMIC
     * measures VBAT directly, so adc_mult does not apply here.
     *
     * Returns 0 if the PMIC did not probe, which reports "unknown" rather
     * than a fabricated reading.
     */
    return pmic.getBattMilliVolts();
  #else
    analogReadResolution(12);

    uint32_t raw = 0;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
      raw += analogRead(PIN_VBAT_READ);
    }
    raw = raw / BATTERY_SAMPLES;
    return (adc_mult * raw);
  #endif
  }

  bool setAdcMultiplier(float multiplier) override {
    if (multiplier == 0.0f) {
      adc_mult = ADC_MULTIPLIER;}
    else {
      adc_mult = multiplier;
    }
    return true;
  }
  float getAdcMultiplier() const override {
    if (adc_mult == 0.0f) {
      return ADC_MULTIPLIER;
    } else {
      return adc_mult;
    }
  }

  const char* getManufacturerName() const override {
    return MANUFACTURER_STRING;
  }

  int buttonStateChanged() {
    #ifdef BUTTON_PIN
      uint8_t v = digitalRead(BUTTON_PIN);
      if (v != btn_prev_state) {
        btn_prev_state = v;
        return (v == LOW) ? 1 : -1;
      }
    #endif
      return 0;
  }
};
