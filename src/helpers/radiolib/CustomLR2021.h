#pragma once

#include <RadioLib.h>
#include "MeshCore.h"
#include "LR2021Pram.h"

// Base address of the LR2021's patch RAM. RadioLib has this in LR2021_registers.h,
// but that header is private to the library's own .cpp files and is not reachable
// via <RadioLib.h>, so it is repeated here.
#define LR2021_PRAM_BASE_ADDR         0x801000
// Words per SPI write, matching Semtech's lr20xx_patch_load_pram(). RadioLib caps a
// WRITE_REG_MEM_32 payload at 128 *bytes* (RADIOLIB_LRXXXX_SPI_MAX_READ_WRITE_LEN),
// so 32 words is exactly the ceiling, not a comfortable margin - raise it and
// writeRegMem32() returns RADIOLIB_ERR_SPI_CMD_INVALID (-706) for every block.
#define LR2021_PRAM_BLOCK_WORDS       32

class CustomLR2021 : public LR2021 {
  uint32_t _preambleMillis = 66;
  uint32_t _maxPayloadMillis = 3934;
  uint32_t _activityAt = 0;
  bool _headerSeen = false;
  bool _rx_boosted = false;

  public:
    CustomLR2021(Module *mod) : LR2021(mod) { irqDioNum = LR2021_IRQ_DIO; }

    bool std_init(SPIClass* spi = NULL)
    {
      
  #ifdef LR2021_TCXO_VOLTAGE
      float tcxo = LR2021_TCXO_VOLTAGE;
  #else
      float tcxo = 1.6f;
  #endif

  #ifdef LORA_CR
      uint8_t cr = LORA_CR;
  #else
      uint8_t cr = 5;
  #endif

  #if defined(P_LORA_SCLK)
    #if defined(NRF52_PLATFORM) || defined(NRF54_PLATFORM)
      if (spi) { spi->setPins(P_LORA_MISO, P_LORA_SCLK, P_LORA_MOSI); spi->begin(); }
    #elif defined(RP2040_PLATFORM)
      if (spi) {
        spi->setMISO(P_LORA_MISO);
        //spi->setCS(P_LORA_NSS); // Setting CS results in freeze
        spi->setSCK(P_LORA_SCLK);
        spi->setMOSI(P_LORA_MOSI);
        spi->begin();
      }
    #else
      if (spi) spi->begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);
    #endif
  #endif
      int status = begin(LORA_FREQ, LORA_BW, LORA_SF, cr, RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE, LORA_TX_POWER, 16, tcxo);
      // if radio init fails with -707/-706, try again with tcxo voltage set to 0.0f
      if (status == RADIOLIB_ERR_SPI_CMD_FAILED || status == RADIOLIB_ERR_SPI_CMD_INVALID) {
        tcxo = 0.0f;
        status = begin(LORA_FREQ, LORA_BW, LORA_SF, cr, RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE, LORA_TX_POWER, 16, tcxo);
      }
      if (status != RADIOLIB_ERR_NONE) {
        Serial.print("ERROR: radio init failed: ");
        Serial.println(status);
        return false;  // fail
      }

  #ifdef LR2021_LOAD_PRAM
      // Opt-in, per board. This header is shared by boards that nobody here can put on
      // a bench, and loading the patch changes radio bring-up on a chip whose firmware
      // we cannot audit - so it is off unless a variant asks for it. Enabling it on a
      // board means committing to a TX/RX soak test on that board.
      //
      // The patch has to go in *after* begin(), not before: PRAM is volatile and
      // begin() resets the chip inside findChip(), which would wipe anything we had
      // uploaded. Semtech's reference driver patches the chip before touching any
      // radio settings, so once the patch is live we re-apply what begin() applied.
      if (uploadPram()) {
        int16_t st = standby();
        if (st == RADIOLIB_ERR_NONE) st = config(RADIOLIB_LR2021_PACKET_TYPE_LORA);
        if (st == RADIOLIB_ERR_NONE) st = setFrequency(LORA_FREQ);
        if (st == RADIOLIB_ERR_NONE) st = setBandwidth(LORA_BW);
        if (st == RADIOLIB_ERR_NONE) st = setSpreadingFactor(LORA_SF);
        if (st == RADIOLIB_ERR_NONE) st = setCodingRate(cr);
        if (st == RADIOLIB_ERR_NONE) st = setSyncWord(RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE);
        if (st == RADIOLIB_ERR_NONE) st = setOutputPower(LORA_TX_POWER);
        if (st == RADIOLIB_ERR_NONE) st = setPreambleLength(16);
        if (st == RADIOLIB_ERR_NONE) st = invertIQ(false);
        if (st != RADIOLIB_ERR_NONE) {
          Serial.print("ERROR: radio re-config after PRAM load failed: ");
          Serial.println(st);
          return false;  // radio is now in an unknown state, don't pretend otherwise
        }
      }
  #endif

      setCRC(2);
      explicitHeader();

      
    #ifdef LR2021_RX_BOOSTED_GAIN
      setRxBoostedGainMode(LR2021_RX_BOOSTED_GAIN);
    #endif

      return true;  // success
    }
    
    // Uploads the Semtech firmware patch and activates it. RadioLib knows how to check
    // for a patch (checkPramLoaded) and how to activate one (activatePram) but has no way
    // to put one there, so on a cold boot there is nothing to activate and activatePram()
    // just fails - hence doing the upload ourselves.
    //
    // This is a stopgap. It belongs in RadioLib, which owns writeRegMem32, the PRAM
    // addresses and modSetup(), and could therefore patch the chip before configuring the
    // radio the way Semtech's driver does - no reaching through RADIOLIB_GODMODE, and no
    // hand-copied replay of begin()'s tail that will drift the next time RadioLib changes
    // that sequence. Drop this in favour of a RadioLib API as soon as one exists.
    //
    // Returns true only if the patch was uploaded and activated by this call, i.e. the
    // caller still has to restore the radio configuration. Any failure is reported and
    // returns false: the radio works unpatched, so this is not worth aborting startup for.
    bool uploadPram() {
      bool loaded = false;
      int16_t st = checkPramLoaded(&loaded);
      if (st != RADIOLIB_ERR_NONE) {
        Serial.print("WARNING: LR2021 PRAM check failed: ");
        Serial.println(st);
        return false;
      }
      if (loaded) return false;  // survived from a previous boot, nothing to do

      const uint32_t* img = lr2021_pram_image();
      for (int i = 0; i < LR2021_PRAM_IMAGE_LEN; i += LR2021_PRAM_BLOCK_WORDS) {
        int n = LR2021_PRAM_IMAGE_LEN - i;
        if (n > LR2021_PRAM_BLOCK_WORDS) n = LR2021_PRAM_BLOCK_WORDS;
        st = writeRegMem32(LR2021_PRAM_BASE_ADDR + i * sizeof(uint32_t), &img[i], n);
        if (st != RADIOLIB_ERR_NONE) {
          Serial.print("WARNING: LR2021 PRAM write failed at word ");
          Serial.print(i);
          Serial.print(": ");
          Serial.println(st);
          return false;  // partial image: leave it inactive rather than activating garbage
        }
      }

      st = activatePram();
      if (st != RADIOLIB_ERR_NONE) {
        Serial.print("WARNING: LR2021 PRAM activate failed: ");
        Serial.println(st);
        return false;
      }

      // the magic word only appears once the patch is actually running
      if (checkPramLoaded(&loaded) != RADIOLIB_ERR_NONE || !loaded) {
        Serial.println("WARNING: LR2021 PRAM did not come up after activate");
        return false;
      }

      uint16_t ver = 0;
      getPramVersion(&ver);
      Serial.print("LR2021 PRAM loaded, version 0x");
      Serial.println(ver, HEX);
      return true;
    }

    float getFreqMHz() const { return freqMHz; }

    bool getRxBoostedGainMode() const { return _rx_boosted; }

    int16_t startReceive() override {
      // include the PREAMBLE_DETECTED irq bit in reported flags
      return LR2021::startReceive(RADIOLIB_LR2021_RX_TIMEOUT_INF, RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED), RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
    }

    bool isReceiving() {
      uint32_t irq = getIrqStatus();
      bool preamble = irq & RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED;  // bit 5
      bool header   = irq & RADIOLIB_LR2021_IRQ_LORA_HEADER_VALID;  // bit 6
      bool hdrErr   = irq & RADIOLIB_LR2021_IRQ_LORA_HDR_CRC_ERROR; // bit 9
      uint32_t now  = millis();
      if (hdrErr) {
        clearIrqFlags(RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED | RADIOLIB_LR2021_IRQ_LORA_HEADER_VALID | RADIOLIB_LR2021_IRQ_LORA_HDR_CRC_ERROR);
        _activityAt = 0;
        _headerSeen = false;
        return false;
      }
      if (!header && _headerSeen) {
        // something cleared the header flag, reset our state.
        _activityAt = 0; _headerSeen = false;
        return false;
      }

      if (header) {
        if (!_headerSeen) { _headerSeen = true; _activityAt = now; };
        if (now - _activityAt > _maxPayloadMillis) {
          MESH_DEBUG_PRINTLN("Clearing header IRQ after %ums", _maxPayloadMillis);
          clearIrqFlags(RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED | RADIOLIB_LR2021_IRQ_LORA_HEADER_VALID | RADIOLIB_LR2021_IRQ_LORA_HDR_CRC_ERROR);
          _activityAt = 0; _headerSeen = false;
          return false;
        }
        return true;
      }
      if (preamble) {
        if (_activityAt == 0) _activityAt = now;
        if (now - _activityAt > _preambleMillis) {
          clearIrqFlags(RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED);
          _activityAt = 0;
          MESH_DEBUG_PRINTLN("Clearing preamble IRQ after %ums", _preambleMillis);
          return false;
        }
        return true;
      }
      _activityAt = 0; _headerSeen = false;
      return false;
    }
    
    void setPreambleMillis(uint32_t preambleMillis) {
      _preambleMillis = preambleMillis;
      MESH_DEBUG_PRINTLN("Set _preambleMillis=%u", _preambleMillis);
    }
    void setMaxPayloadMillis(uint32_t payloadMillis) {
      _maxPayloadMillis = payloadMillis;
      MESH_DEBUG_PRINTLN("Set _maxPayloadMillis=%u", _maxPayloadMillis);
    }


    uint8_t getSpreadingFactor() const { return spreadingFactor; }
};