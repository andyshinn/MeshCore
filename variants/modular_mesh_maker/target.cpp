#include <Arduino.h>
#include "target.h"
#include <helpers/ArduinoHelpers.h>

MMMBoard board;

RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);

WRAPPER_CLASS radio_driver(radio, board);

VolatileRTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
#if ENV_INCLUDE_GPS
  #include <helpers/sensors/MicroNMEALocationProvider.h>
  MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
  EnvironmentSensorManager sensors = EnvironmentSensorManager(nmea);
#else
  EnvironmentSensorManager sensors;
#endif

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true, true);
#endif

#ifdef RF_SWITCH_TABLE

  // Pin functions come from NiceRF's own LoRa2021F33-2G4 demo firmware
  // (LoRa/Core/Src/lr2021.c in the vendor package), not from the LR2021 datasheet -
  // they are a property of the module, not of the chip. There: DIO9 = IRQ,
  // DIO6 = sub-GHz TXEN, DIO8 = 2.4GHz TXEN, DIO5 = 2.4GHz LNA enable, and
  // DIO7 = 2.4GHz PA supply enable held statically high. Sub-GHz RX is passive,
  // so nothing is asserted for it.
  const uint32_t rfswitch_dios[] = {
    RADIOLIB_LR2021_DIO5,
    RADIOLIB_LR2021_DIO6,
    RADIOLIB_LR2021_DIO7,
    RADIOLIB_LR2021_DIO8,
    RADIOLIB_NC
  };

  // DIO7 is listed here but never asserted, and that is a tradeoff rather than an
  // oversight. On the F33 it is the 2.4GHz PA's supply enable - NiceRF drives it as a
  // static GPIO high - so a per-mode switch table cannot express it. Holding it low in
  // every row leaves the 2.4GHz front end definitively off, which is what this sub-GHz
  // board wants; the vendor-faithful alternative is high in every row, which would make
  // the board 2.4GHz-ready at the cost of keeping that supply enabled during sub-GHz
  // operation (current draw unmeasured). Either way, bringing up 2.4GHz here means
  // driving DIO7 separately with setDioFunction(7,
  // RADIOLIB_LR2021_DIO_FUNCTION_GPIO_OUTPUT_HIGH, ...), not adding a HIGH to TX_HF.
  static const Module::RfSwitchMode_t rfswitch_table[] = {
    // mode                DIO5  DIO6  DIO7  DIO8
    { LR2021::MODE_STBY,  {LOW,  LOW,  LOW,  LOW }},
    { LR2021::MODE_RX,    {LOW,  LOW,  LOW,  LOW }},   // sub-GHz RX is passive on the F33
    { LR2021::MODE_TX,    {LOW,  HIGH, LOW,  LOW }},   // DIO6 = sub-GHz TXEN (PA key)
    { LR2021::MODE_RX_HF, {HIGH, LOW,  LOW,  LOW }},   // DIO5 = 2.4GHz LNA (never selected at 915MHz)
    { LR2021::MODE_TX_HF, {LOW,  LOW,  LOW,  HIGH}},   // DIO8 = 2.4GHz TXEN (never selected at 915MHz)
    END_OF_MODE_TABLE,
  };

#endif

bool radio_init() {
  rtc_clock.begin(Wire);

  int err = radio.std_init(&SPI);
  if (err != 1) return err;

#ifdef RF_SWITCH_TABLE
  // Must stay after std_init(). LR2021 declares its own setRfSwitchTable - unrelated to
  // Module's same-named method, since LR2021 derives from LRxxxx/PhysicalLayer and only
  // holds a Module - and it configures the chip's DIOs over SPI (SetDioFunction /
  // SetDioRfSwitchConfig) rather than any MCU GPIO. It therefore needs the SPI command
  // set that modSetup() installs, and begin() hard-resets the chip inside findChip(),
  // which would discard anything written before it.
  radio.setRfSwitchTable(rfswitch_dios, rfswitch_table);
#endif

  return true;
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}
