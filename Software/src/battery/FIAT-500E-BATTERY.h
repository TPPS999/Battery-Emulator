#ifndef FIAT_500E_BATTERY_H
#define FIAT_500E_BATTERY_H

// ============================================================================
// Fiat 500e (2020+) battery -- SCAFFOLD / WIP
// ----------------------------------------------------------------------------
// Poll-based (UDS service 0x22 ReadDataByIdentifier) driver for the 2nd-gen
// Fiat 500e HV battery pack control module (BPCM / "BDU", diagnostic ECU DA44).
//
// Signal map + scaling derived from:
//   - OBDb/FIAT-500e (community-verified signalset, service 22 DIDs), and
//   - reverse engineering of the BDU firmware (AURIX TC27x) -- see project fiat_500.
//
// STATUS: scaffold only. NOT registered in BATTERIES.cpp / BatteryType enum
// (do that separately). NOT tested on hardware. Diagnostic request/response
// headers and the exact poll/ISO-TP flow must be confirmed against a real car.
//
// Diagnostic addressing (FCA 29-bit): request to DA44 = 0x18DA44F1,
// response from DA44 = 0x18DAF144, flow-control = 0x30 0x00 0x00.
// ============================================================================

#include "CanBattery.h"

class Fiat500eBattery : public CanBattery {
 public:
  Fiat500eBattery(DATALAYER_BATTERY_TYPE* datalayer_ptr, CAN_Interface targetCan) : CanBattery(targetCan) {
    datalayer_battery = datalayer_ptr;
  }
  Fiat500eBattery() { datalayer_battery = &datalayer.battery; }

  virtual void setup(void);
  virtual void handle_incoming_can_frame(CAN_frame rx_frame);
  virtual void update_values();
  virtual void transmit_can(unsigned long currentMillis);
  static constexpr const char* Name = "Fiat 500e (2020+) battery";

 private:
  DATALAYER_BATTERY_TYPE* datalayer_battery;

  // --- pack topology (from RE + OBDb). The BDU firmware supports TWO variants; its internal
  //     arrays/loops are 108-wide (108 == 0x6C, dominant loop bound in the firmware):
  //       * 96s2p  : 96 cells in series, 2p, 60Ah cells -> ~120Ah, ~408V max (96 * 4.25)
  //       * 108s1p : 108 cells in series, 1p           -> ~459V max (108 * 4.25)
  //     Variant is config-driven (NVM/cal). Select here (or detect from cell-count DID reply). ---
#define FIAT500E_VARIANT_108S1P 0  // 0 = 96s2p, 1 = 108s1p
#if FIAT500E_VARIANT_108S1P
  static const int NUMBER_OF_CELLS = 108;
  static const int MAX_PACK_VOLTAGE_DV = 4590;  // 108 * 4.25 V
#else
  static const int NUMBER_OF_CELLS = 96;
  static const int MAX_PACK_VOLTAGE_DV = 4080;  // 96 * 4.25 V  (OCV table max)
#endif
  static const int MAX_CELLS_SUPPORTED = 108;   // firmware array width; cell DIDs A100..A108
  static const int MIN_PACK_VOLTAGE_DV = 3000;  // verify
  static const int MAX_CELL_VOLTAGE_MV = 4250;
  static const int MIN_CELL_VOLTAGE_MV = 3000;
  static const int MAX_CELL_DEVIATION_MV = 120;

  // --- UDS diagnostic headers (29-bit, FCA) -- CONFIRM on car ---
  static const uint32_t REQ_ID_DA44 = 0x18DA44F1;
  static const uint32_t RSP_ID_DA44 = 0x18DAF144;

  // --- DIDs we poll on DA44 (service 0x22). See FIAT-500E-BATTERY.cpp decode. ---
  // A010 SOC, A029 SOH+capacity, A009 min/max cell V+T, A00A current, A011 pack V,
  // A200 module temps (18), A100..A107 cell voltages (96), A001 lifetime Ah/kWh.

  unsigned long previousMillisPoll = 0;

  // ISO-TP reassembly buffer for the current multiframe response
  uint8_t iso_tp_buf[256] = {0};
  uint16_t iso_tp_len = 0;
  uint16_t iso_tp_idx = 0;
  uint16_t current_did = 0;  // DID we are currently expecting a reply for

  // cell voltage staging (filled across A100..A108; up to 108 for 108s1p)
  uint16_t cellvoltages_mV[MAX_CELLS_SUPPORTED] = {0};
};

#endif  // FIAT_500E_BATTERY_H
