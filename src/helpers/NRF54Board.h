#pragma once

#include <Arduino.h>
#include <MeshCore.h>
#include <helpers/KeyValueStore.h>

#if defined(NRF54_PLATFORM)

/*
 * Nordic nRF54L board support -- sibling of NRF52Board for the Cortex-M33
 * nRF54L series (RRAM, S145 SoftDevice).
 *
 * This is deliberately NOT a subclass of, or a shared header with, NRF52Board:
 * the register interfaces differ enough that sharing would be all #ifdef. Of
 * the SoftDevice SoC calls NRF52Board relies on, S145 keeps only sd_temp_get(),
 * sd_softdevice_is_enabled() and sd_power_gpregret_*(). It has no
 * sd_app_evt_wait(), sd_power_system_off(), sd_power_dcdc_mode_set() or
 * sd_power_usbregstatus_get(). Where an equivalent exists, we call the helper
 * the nRF54L Arduino core already provides in wiring.h (waitForEvent(),
 * readCPUTemperature(), readResetReason(), bootloaderVersion) rather than
 * touching registers here.
 *
 * NOT yet implemented, relative to NRF52Board:
 *   - the NRF52_POWER_MANAGEMENT block (voltage wake, GPREGRET shutdown
 *     reasons, boot voltage protection). The silicon is not the obstacle:
 *     the nRF54LM20A does have LPCOMP -- it shares base 0x50106000 with COMP
 *     (nrf54lm20a_global.h) and keeps the same ENABLE / PSEL / REFSEL /
 *     EXTREFSEL / ANADETECT / HYST registers as nRF52 -- and RESETREAS bit 9
 *     is a dedicated "woke from System OFF on LPCOMP ANADETECT" flag. Two
 *     register differences matter if NRF52Board::configureVoltageWake() is
 *     ever ported: PSEL is {PORT,PIN} rather than an AIN index, and HYST is a
 *     plain enable bit with no 50mV enum. NRF_POWER->GPREGRET[1] is free for
 *     a shutdown reason -- the bootloader matches its DFU magic only in
 *     GPREGRET[0], and the core writes only GPREGRET[0]. What this board
 *     lacks is something for LPCOMP to watch: VBAT sits behind the nPM1300 on
 *     I2C and the variant brings out no PMIC interrupt line, so the PMIC
 *     cannot wake the MCU either.
 *   - isExternalPowered() from the MCU. The nRF54LM20A does have a USB
 *     regulator (VREGUSB, and NRF_VREGUSB is defined by the MDK headers), but
 *     it reports VBUS only through EVENTS_VBUSDETECTED after TASKS_START, and
 *     the core sets NRFX_USBREG_ENABLED=0 so nothing ever starts it. A board
 *     carrying a PMIC can answer the question anyway and should override it
 *     itself -- NPM1300::isVbusPresent() reads the VBUSIN bank directly --
 *     leaving MainBoard's default of false for boards that cannot.
 */
class NRF54Board : public mesh::MainBoard {
protected:
  uint8_t startup_reason;
  char *ota_name;
  uint32_t reset_reason;

public:
  NRF54Board(char *otaname) : ota_name(otaname), reset_reason(0) {}
  virtual void begin();
  virtual uint8_t getStartupReason() const override { return startup_reason; }
  virtual float getMCUTemperature() override;
  virtual void reboot() override { NVIC_SystemReset(); }
  virtual void shutdownPeripherals();
  virtual void powerOff() override;
  virtual bool getBootloaderVersion(char* version, size_t max_len) override;
  virtual bool startOTAUpdate(const char *id, char reply[]) override;
  virtual void sleep(uint32_t secs) override;

  // Not behind NRF52_POWER_MANAGEMENT: `get pwrmgt.bootreason` is the one
  // command in that family CommonCLI answers unconditionally, so these two
  // are all it takes to replace its "Not available" with a real reason.
  virtual uint32_t getResetReason() const override { return reset_reason; }
  const char* getResetReasonString(uint32_t reason) override;

  void attachDynamicPrefs(KeyValueStore* prefs) { }  // no-op
};

/*
 * nRF54L main regulator has a DC/DC mode that is more efficient than the LDO,
 * but it requires the inductor to be populated on the module/board. Boards that
 * meet the hardware requirement use this subclass.
 */
class NRF54BoardDCDC : virtual public NRF54Board {
public:
  NRF54BoardDCDC() {}
  virtual void begin() override;
};
#endif
