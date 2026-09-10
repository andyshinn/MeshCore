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
 *   - the NRF52_POWER_MANAGEMENT block (LPCOMP voltage wake, GPREGRET shutdown
 *     reasons, boot voltage protection). nRF54L has no LPCOMP; the equivalent
 *     would be built on COMP/ADC + the RESETREAS/retained registers.
 *   - isExternalPowered(). The nRF54LM20A does have a USB peripheral (VREGUSB
 *     with EVENTS_VBUSDETECTED), but the core currently sets
 *     NRFX_USBREG_ENABLED=0 and does not define NRF_VREGUSB, so there is no
 *     supported way to read VBUS yet.
 */
class NRF54Board : public mesh::MainBoard {
protected:
  uint8_t startup_reason;
  char *ota_name;

public:
  NRF54Board(char *otaname) : ota_name(otaname) {}
  virtual void begin();
  virtual uint8_t getStartupReason() const override { return startup_reason; }
  virtual float getMCUTemperature() override;
  virtual void reboot() override { NVIC_SystemReset(); }
  virtual void shutdownPeripherals();
  virtual void powerOff() override;
  virtual bool getBootloaderVersion(char* version, size_t max_len) override;
  virtual bool startOTAUpdate(const char *id, char reply[]) override;
  virtual void sleep(uint32_t secs) override;

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
