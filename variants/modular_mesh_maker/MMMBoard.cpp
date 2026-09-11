#include <Arduino.h>
#include <Wire.h>

#include "MMMBoard.h"

void MMMBoard::begin() {
    MMMBoardVBase::begin();
    btn_prev_state = HIGH;

  #if defined(NRF54_PLATFORM)
    // No GPIO battery divider on the nRF54LM20A XIAO; battery voltage comes
    // from the nPM1300 PMIC on Wire1, its own private bus (P1.18/P1.17).
    Wire1.begin();
    if (!pmic.begin(&Wire1)) {
      MESH_DEBUG_PRINTLN("nPM1300 not responding; battery voltage unavailable");
    } else {
      // Only reached when the PMIC actually answered a register read, so the
      // charger path never talks to a part that is not there.
      configureBatteryCharger();
    }
  #else
    pinMode(PIN_VBAT_READ, INPUT);
  #endif

    #ifdef BUTTON_PIN
      pinMode(BUTTON_PIN, INPUT_PULLUP);
    #endif

    #if defined(PIN_BOARD_SDA) && defined(PIN_BOARD_SCL)
      Wire.setPins(PIN_BOARD_SDA, PIN_BOARD_SCL);
    #endif

    Wire.begin();

    pinMode(P_LORA_POWER_EN, OUTPUT);
    digitalWrite(P_LORA_POWER_EN, HIGH);
    delay(10);
}

#if defined(NRF54_PLATFORM)
void MMMBoard::configureBatteryCharger() {
#ifdef BATT_CAPACITY_MAH
  chg_attempted = true;

  NPM1300::ChargerConfig cfg;
  cfg.vterm_mv      = BATT_CHARGE_VTERM_MILLIVOLTS;
  cfg.vterm_warm_mv = BATT_CHARGE_VTERM_WARM_MILLIVOLTS;
  cfg.ichg_ma       = BATT_CHARGE_MILLIAMPS;
  cfg.vbus_ilim_ma  = BATT_CHARGE_VBUS_LIMIT_MILLIAMPS;

  // The only temperature interlock this board has. It measures the PMIC die,
  // not the cell -- see the long comment in MMMBoard.h before changing it.
  cfg.die_stop_k    = NPM1300::DIETEMP_K_60C;
  cfg.die_resume_k  = NPM1300::DIETEMP_K_50C;

  // There is no thermistor on this board: the nPM1300 NTC pin is a fixed 10k
  // to ground. Saying so (Hi_Z + DISABLENTC, per PS 6.2.5) is not what makes
  // charging possible here -- that fixed resistor already reads as a permanent
  // 25 C, so the part would charge either way. It is the honest setting: it
  // stops NTCSTATUS and any reported battery temperature from being fiction.
  cfg.ntc           = NPM1300::NTC_NONE;

#if (BATT_CHARGE_IDEAL_MILLIAMPS) > (BATT_CHARGE_CEILING_MILLIAMPS)
  MESH_DEBUG_PRINTLN("nPM1300 charger: %u mAh cell wants %u mA at 0.5C, clamped DOWN to the board ceiling %u mA",
                     (unsigned) (BATT_CAPACITY_MAH), (unsigned) (BATT_CHARGE_IDEAL_MILLIAMPS),
                     (unsigned) (BATT_CHARGE_CEILING_MILLIAMPS));
#endif

  chg_result = pmic.configureCharger(cfg);

  // Repeated from onBootComplete(), which is where a terminal can actually see
  // it. Printing it here too costs nothing and keeps the verdict in the log for
  // anyone who is already attached.
  reportBatteryCharger(false);

#else
  // The fail-safe path -- and it has to WRITE, not just decline to write.
  //
  // The nPM1300 is the MCU's own power source and does not reset when the nRF54
  // does (the same fact that forces disable-before-configure inside
  // configureCharger). A build that previously opted in leaves the charger
  // enabled with the old current and termination voltage straight through a DFU
  // reflash, so removing BATT_CAPACITY_MAH -- the only off-switch a user has --
  // would otherwise change nothing while this line claimed it had.
  //
  // Writing BCHGENABLECLR cannot start a charge, so this strictly reduces risk.
  chg_attempted = true;
  chg_result = pmic.disableCharging() ? NPM1300::CHARGER_OK
                                      : NPM1300::CHARGER_DISABLE_FAILED;
  reportBatteryCharger(false);
#endif
}

void MMMBoard::onBootComplete() {
  reportBatteryCharger(true);
}

void MMMBoard::reportBatteryCharger(bool sample_status) {
#ifdef BATT_CAPACITY_MAH
  if (!chg_attempted) {
    // pmic.begin() failed, so the charger was never touched either way.
    MESH_DEBUG_PRINTLN("nPM1300 charger: PMIC did not respond, charger state UNKNOWN");
    return;
  }

  if (chg_result == NPM1300::CHARGER_DISABLE_FAILED) {
    // The one failure that does NOT mean the charger is off.
    MESH_DEBUG_PRINTLN("nPM1300 charger state UNKNOWN -- the shutdown write failed, so it may still be charging. Unplug USB and disconnect the cell.");
    return;
  }

  if (chg_result != NPM1300::CHARGER_OK) {
    MESH_DEBUG_PRINTLN("nPM1300 charger NOT enabled (%s); the cell will not charge",
                       NPM1300::chargerResultStr(chg_result));
    if (chg_result == NPM1300::CHARGER_LATCHED_ERROR) {
      uint8_t reason = 0, sensor = 0;
      pmic.getChargeError(&reason, &sensor);
      // PS 6.2.14.32: bits 5/6 are the 7 h charge and 10 min trickle timeouts,
      // bits 0/1 a sensor the part could not trust. Those are not cleared here
      // on purpose. Recovery needs the PMIC to lose power -- unplug USB AND
      // disconnect the cell -- after establishing that the cell is safe.
      MESH_DEBUG_PRINTLN("nPM1300 BCHGERRREASON=0x%02x BCHGERRSENSOR=0x%02x (latched from an earlier charge cycle; not cleared automatically)",
                         reason, sensor);
    }
    return;
  }

  MESH_DEBUG_PRINTLN("nPM1300 charger enabled: %u mA into a %u mAh cell, terminating at %u mV, NO cell temperature sensing on this board",
                     (unsigned) pmic.getChargeMilliAmps(), (unsigned) (BATT_CAPACITY_MAH),
                     (unsigned) BATT_CHARGE_VTERM_MILLIVOLTS);

  if (sample_status) {
    uint8_t status = 0;
    if (pmic.getChargeStatus(&status)) {
      MESH_DEBUG_PRINTLN("nPM1300 BCHGCHARGESTATUS=0x%02x", status);
      if ((status & NPM1300::CHG_BATTERY_DETECTED) == 0) {
        // Only meaningful once the part has had time to run its own battery
        // detection, which is why this is not sampled in begin().
        //
        // If a cell IS fitted and this still says no, suspect the pack's
        // protection circuit: after a deep discharge some PCMs will not release
        // their FET until they see close to the termination voltage (VRECHARGE
        // is 95% of VTERM, PS 6.2.12 -- 3942 mV here), and the nPM1300 then
        // never sees a battery to start on. Do NOT "fix" that by raising
        // BATT_CHARGE_VTERM_MILLIVOLTS; charge the pack elsewhere.
        MESH_DEBUG_PRINTLN("nPM1300 charger: no battery detected on BAT+/BAT-");
      }
    }
  }

  // Once this is on, getBattMilliVolts() reads a cell that is being actively
  // charged whenever USB is present, so stats-core's battery_mv and the LPP
  // telemetry voltage sit near the termination voltage regardless of the true
  // state of charge. Do not write low-battery logic against that number.

#else   /* no BATT_CAPACITY_MAH: charging is off, and was actively turned off */
  (void) sample_status;

  if (!chg_attempted) {
    MESH_DEBUG_PRINTLN("nPM1300 charger: PMIC did not respond, charger state UNKNOWN");
  } else if (chg_result == NPM1300::CHARGER_DISABLE_FAILED) {
    MESH_DEBUG_PRINTLN("nPM1300 charger: BCHGENABLECLR write FAILED -- charger state UNKNOWN, it may still be enabled by an earlier firmware. Unplug USB and disconnect the cell.");
  } else {
    MESH_DEBUG_PRINTLN("nPM1300 charger DISABLED: BATT_CAPACITY_MAH is not defined for this build");
  }
#endif
}
#endif
