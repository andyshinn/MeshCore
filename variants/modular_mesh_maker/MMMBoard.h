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

class MMMBoard : public MMMBoardBase {
protected:
  uint8_t btn_prev_state;
  float adc_mult = ADC_MULTIPLIER;
#if defined(NRF54_PLATFORM)
  NPM1300 pmic;
#endif

public:
  MMMBoard() : MMMBoardVBase("MMM_OTA") {}
  void begin();

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
