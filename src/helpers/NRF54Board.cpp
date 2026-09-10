#if defined(NRF54_PLATFORM)
#include "NRF54Board.h"
#include <target.h>

#include <bluefruit.h>
#include <nrf_soc.h>
#include <nrf.h>

static BLEDfu bledfu;

static void connect_callback(uint16_t conn_handle) {
  (void)conn_handle;
  MESH_DEBUG_PRINTLN("BLE client connected");
}

static void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void)conn_handle;
  (void)reason;
  MESH_DEBUG_PRINTLN("BLE client disconnected");
}

void NRF54Board::begin() {
  startup_reason = BD_STARTUP_NORMAL;

  // NOTE: nRF52 enables the CC310 TRNG here via USE_CC310_HW_CRYPTO. The
  // nRF54L equivalent is CRACEN (the core ships an nRF54Crypto library), but
  // MeshCore does not use it yet, so there is nothing to start.

  // Start SPI explicitly. target.cpp builds the radio as
  // `new Module(..., SPI)`, which reaches RadioLib's
  // ArduinoHal(SPIClass&, SPISettings) constructor -- and that constructor
  // never initialises `initInterface`, so whether ArduinoHal::init() calls
  // spiBegin() is indeterminate. On nRF52 it happens to come out true; on
  // nRF54L it comes out false, leaving SPIM PSEL unassigned and every radio
  // command failing with RADIOLIB_ERR_SPI_CMD_FAILED (-707).
#if defined(P_LORA_SCLK) && defined(P_LORA_MISO) && defined(P_LORA_MOSI)
  SPI.setPins(P_LORA_MISO, P_LORA_SCLK, P_LORA_MOSI);
#endif
  SPI.begin();
}

void NRF54BoardDCDC::begin() {
  NRF54Board::begin();

  // Enable the main DC/DC converter for better efficiency. Unlike nRF52 the
  // nRF54L can tell us whether the required inductor is actually populated, so
  // check first rather than enabling blind.
  if (NRF_REGULATORS->VREGMAIN.INDUCTORDET &
      REGULATORS_VREGMAIN_INDUCTORDET_DETECTED_Msk) {
    NRF_REGULATORS->VREGMAIN.DCDCEN = REGULATORS_VREGMAIN_DCDCEN_VAL_Enabled;
  } else {
    MESH_DEBUG_PRINTLN("VREGMAIN inductor not detected; staying on LDO");
  }
}

void NRF54Board::sleep(uint32_t secs) {
  // Event-driven sleep; the 'secs' parameter is ignored, same as on nRF52.
  // S145 has no sd_app_evt_wait(), so the core's waitForEvent() uses WFE
  // directly (and handles the SEV/WFE dance to clear the event register).
  waitForEvent();
}

float NRF54Board::getMCUTemperature() {
  // Core helper picks sd_temp_get() when the SoftDevice is enabled and falls
  // back to driving NRF_TEMP directly when it is not.
  return readCPUTemperature();
}

void NRF54Board::shutdownPeripherals() {
  // Power off the display if any
#ifdef DISPLAY_CLASS
  if (display.isOn()) {
    display.turnOff();
  }
#endif
  // Prep LoRa radio for power down
  #ifdef P_LORA_RESET
    digitalWrite(P_LORA_RESET, HIGH);  // preload OUT latch so pinMode can't glitch NRESET low
    pinMode(P_LORA_RESET, OUTPUT);
    digitalWrite(P_LORA_RESET, LOW);   // deliberate hardware reset (datasheet: >=100us)
    delayMicroseconds(200);
    digitalWrite(P_LORA_RESET, HIGH);
  #endif
  #if defined(P_LORA_SCLK) && defined(P_LORA_MISO) && defined(P_LORA_MOSI)
    SPI.setPins(P_LORA_MISO, P_LORA_SCLK, P_LORA_MOSI);
    SPI.begin(); // SPI may not be started on some shutdown paths, need it to shut down radio
  #endif
  #ifdef P_LORA_BUSY
    pinMode(P_LORA_BUSY, INPUT);
    uint32_t started_at = millis();
    while (digitalRead(P_LORA_BUSY) && millis() - started_at < 10) {} //wait for radio to be ready
  #endif
  #ifdef P_LORA_NSS
    pinMode(P_LORA_NSS, OUTPUT);
    digitalWrite(P_LORA_NSS, HIGH);
  #endif
  // Power off LoRa
  radio_driver.powerOff();

  // Keep LoRa inactive during deepsleep
  #ifdef P_LORA_NSS
    digitalWrite(P_LORA_NSS, HIGH);
  #endif

  // Power off GPS if any
  if (sensors.getLocationProvider() != NULL) {
    sensors.getLocationProvider()->stop();
  }

  // Flush serial buffers
  Serial.flush();
  delay(100);
}

void NRF54Board::powerOff() {
  shutdownPeripherals();

  // S145 has no sd_power_system_off(); on nRF54L System OFF is entered through
  // the REGULATORS block. The core's systemOff() helper wants a wake-up pin,
  // which this entry point does not have, so write the register directly.
  NRF_REGULATORS->SYSTEMOFF = REGULATORS_SYSTEMOFF_SYSTEMOFF_Enter;

  // Should not return; reset rather than falling through to the caller.
  NVIC_SystemReset();
}

bool NRF54Board::getBootloaderVersion(char* out, size_t max_len) {
  // nRF52 scrapes an "UF2 Bootloader " string out of a known flash window. On
  // nRF54L the Arduino core exports the version the bootloader handed over in
  // the MBR/boot handoff, so use that instead of guessing an RRAM address.
  if (bootloaderVersion == 0 || max_len < 12) return false;

  unsigned major = (bootloaderVersion >> 16) & 0xFF;
  unsigned minor = (bootloaderVersion >> 8) & 0xFF;
  unsigned patch = bootloaderVersion & 0xFF;
  snprintf(out, max_len, "%u.%u.%u", major, minor, patch);
  return true;
}

bool NRF54Board::startOTAUpdate(const char *id, char reply[]) {
  (void)id;

  // Config the peripheral connection with maximum bandwidth
  // more SRAM required by SoftDevice
  // Note: All config***() function must be called before begin()
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.configPrphConn(92, BLE_GAP_EVENT_LENGTH_MIN, 16, 16);

  if (!Bluefruit.begin(1, 0)) return false;

  Bluefruit.setTxPower(4);
  Bluefruit.setName(ota_name);

  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  // To be consistent OTA DFU should be added first if it exists
  bledfu.begin();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addName();

  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244); // in unit of 0.625 ms
  Bluefruit.Advertising.setFastTimeout(30);   // number of seconds in fast mode
  Bluefruit.Advertising.start(0);             // 0 = Don't stop advertising after n seconds

  uint8_t mac_addr[6];
  memset(mac_addr, 0, sizeof(mac_addr));
  Bluefruit.getAddr(mac_addr);
  sprintf(reply, "OK - mac: %02X:%02X:%02X:%02X:%02X:%02X", mac_addr[5], mac_addr[4], mac_addr[3],
          mac_addr[2], mac_addr[1], mac_addr[0]);

  return true;
}
#endif
