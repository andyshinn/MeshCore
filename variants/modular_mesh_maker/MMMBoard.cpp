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
