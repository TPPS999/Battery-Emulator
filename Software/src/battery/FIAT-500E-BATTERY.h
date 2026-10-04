#ifndef FIAT_500E_BATTERY_H
#define FIAT_500E_BATTERY_H

// ============================================================================
// Fiat 500e (2020+) battery -- SCAFFOLD / WIP
// ----------------------------------------------------------------------------
// HV battery pack control module ("BDU"/BPCM, diagnostic ECU DA44) of the 2nd-gen
// Fiat 500e. Modelled on the Stellantis Pro One driver: the pack BROADCASTS its
// state on the internal CAN bus (11-bit IDs), so this driver primarily LISTENS,
// and optionally polls UDS (service 0x22) for data that is not broadcast (per-cell
// voltages A100..A108, SOH A029).
//
// Frame IDs + layout cross-validated between:
//   - BDU firmware RE (AURIX TC27x) -- CAN matrix @0x800faf2c, see project fiat_500,
//   - Stellantis Pro One DBC/driver (shared BPCM frames), and
//   - OBDb/FIAT-500e (UDS DIDs).
// Shared BPCM frames found in both the 500e firmware and Pro One:
//   0x306 BPCM_SOC(+contactor), 0x307 Temperatures, 0x285 ChargeLimits,
//   0x359 Limits+Capacity, 0x2A5 VEH_ChargeRequest.
//
// STATUS: scaffold. NOT registered (BatteryType/BATTERIES.cpp), NOT HW-tested.
// Scaling for 500e is the Pro One layout as a hypothesis -- verify on a real car.
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

  // --- pack variants (BDU firmware is 108-cell-wide; variant is config-driven) ---
  //   96s2p : 96S/2P, 60Ah cells -> ~120Ah, ~408V max      108s1p : 108S/1P -> ~459V max
#define FIAT500E_VARIANT_108S1P 0  // 0 = 96s2p, 1 = 108s1p
#if FIAT500E_VARIANT_108S1P
  static const int NUMBER_OF_CELLS = 108;
  static const int MAX_PACK_VOLTAGE_DV = 4590;
#else
  static const int NUMBER_OF_CELLS = 96;
  static const int MAX_PACK_VOLTAGE_DV = 4080;
#endif
  static const int MAX_CELLS_SUPPORTED = 108;
  static const int MIN_PACK_VOLTAGE_DV = 3000;
  static const int MAX_CELL_VOLTAGE_MV = 4250;
  static const int MIN_CELL_VOLTAGE_MV = 3000;
  static const int MAX_CELL_DEVIATION_MV = 120;

  // --- broadcast frame IDs (shared with Stellantis Pro One; confirmed in 500e FW matrix) ---
  static const uint16_t ID_BPCM_SOC = 0x306;        // SOC + contactor status
  static const uint16_t ID_BPCM_TEMPS = 0x307;      // temperatures (layout TBD)
  static const uint16_t ID_BPCM_CHARGELIM = 0x285;  // charge current limits (DCCL)
  static const uint16_t ID_BPCM_LIM_CAP = 0x359;    // OBC charge limit + pack capacity
  static const uint16_t ID_VEH_CHARGEREQ = 0x2A5;   // vehicle -> pack charge request

  static const uint16_t SOC_FINE_FULL_SCALE = 4080;  // 255 * 16 (Pro One)
  static const uint16_t LIMIT_INVALID = 0xFFFF;

  // contactor status from 0x306 byte5 [3:0]: 8 off, 9 precharge, 10 on
  enum { CONTACTORS_OFF = 8, CONTACTORS_PRECHARGE = 9, CONTACTORS_ON = 10 };
  uint8_t contactor_status = CONTACTORS_OFF;
  uint16_t charge_limit_dA = 0;          // from 0x285 / 0x359
  uint16_t pack_capacity_ah_tenths = 0;  // from 0x359

  // --- optional UDS poll (per-cell voltages A100..A108, SOH A029) on DA44 ---
  static const uint32_t REQ_ID_DA44 = 0x18DA44F1;  // verify on car
  static const uint32_t RSP_ID_DA44 = 0x18DAF144;
  unsigned long previousMillisPoll = 0;
  uint8_t iso_tp_buf[256] = {0};
  uint16_t iso_tp_len = 0, iso_tp_idx = 0;
  uint16_t cellvoltages_mV[MAX_CELLS_SUPPORTED] = {0};
};

#endif  // FIAT_500E_BATTERY_H
