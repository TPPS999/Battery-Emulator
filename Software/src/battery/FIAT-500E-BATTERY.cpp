#include "FIAT-500E-BATTERY.h"
#include "../include.h"

// ============================================================================
// Fiat 500e (2020+) battery -- SCAFFOLD / WIP  (see FIAT-500E-BATTERY.h)
// Broadcast-primary (Pro One style) + optional UDS poll for per-cell data.
// NOT registered, NOT HW-tested. Scaling = Pro One hypothesis, verify on car.
// ============================================================================

static inline uint16_t be16(const uint8_t* d, uint8_t i) {
  return (uint16_t)(d[i] << 8) | d[i + 1];
}

void Fiat500eBattery::setup(void) {
  datalayer_battery->info.number_of_cells = NUMBER_OF_CELLS;
  datalayer_battery->info.max_design_voltage_dV = MAX_PACK_VOLTAGE_DV;
  datalayer_battery->info.min_design_voltage_dV = MIN_PACK_VOLTAGE_DV;
  datalayer_battery->info.max_cell_voltage_mV = MAX_CELL_VOLTAGE_MV;
  datalayer_battery->info.min_cell_voltage_mV = MIN_CELL_VOLTAGE_MV;
  datalayer_battery->info.max_cell_voltage_deviation_mV = MAX_CELL_DEVIATION_MV;
  datalayer_battery->info.chemistry = battery_chemistry_enum::NMC;
}

// ---- optional UDS cell-voltage reassembly (A100..A108) ----
static void decode_uds(DATALAYER_BATTERY_TYPE* dl, const uint8_t* buf, uint16_t len, uint16_t* cellv) {
  if (len < 3 || buf[0] != 0x62)
    return;
  uint16_t did = (buf[1] << 8) | buf[2];
  if (did >= 0xA100 && did <= 0xA108) {  // 12 cell voltages (mV) per DID, cells 1..108
    uint8_t base = (uint8_t)((did - 0xA100) * 12);
    for (uint8_t i = 0; i < 12 && (base + i) < 108; i++)
      cellv[base + i] = be16(buf, 3 + i * 2);
    if (did == 0xA108)
      memcpy(dl->status.cell_voltages_mV, cellv, dl->info.number_of_cells * sizeof(uint16_t));
  } else if (did == 0xA029) {  // SOH 16-bit /655.35 %
    dl->status.soh_pptt = (uint16_t)((uint32_t)be16(buf, 3) * 10000UL / 65535UL);
  }
}

void Fiat500eBattery::handle_incoming_can_frame(CAN_frame rx_frame) {
  // ---- broadcast frames (11-bit), shared with Stellantis Pro One ----
  switch (rx_frame.ID) {
    case ID_BPCM_SOC: {  // 0x306: SOC + contactor status
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      uint16_t soc_fine = (uint16_t)((rx_frame.data.u8[6] & 0x0F) << 8) | rx_frame.data.u8[7];
      if (soc_fine != 0)
        datalayer_battery->status.real_soc = (uint16_t)((uint32_t)soc_fine * 10000u / SOC_FINE_FULL_SCALE);
      contactor_status = rx_frame.data.u8[5] & 0x0F;  // 8 off / 9 precharge / 10 on
      return;
    }
    case ID_BPCM_CHARGELIM:  // 0x285: 3x u16 BE charge current limit [0.1A] (DCCL)
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      charge_limit_dA = (be16(rx_frame.data.u8, 0) == LIMIT_INVALID) ? 0 : be16(rx_frame.data.u8, 0);
      return;
    case ID_BPCM_LIM_CAP:  // 0x359: OBC charge limit [0.1A] + pack capacity [0.1Ah]
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      pack_capacity_ah_tenths = be16(rx_frame.data.u8, 2);
      return;
    case ID_BPCM_TEMPS:  // 0x307: temperatures -- TODO layout (not yet decoded even in Pro One)
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      return;
    default:
      break;
  }

  // ---- optional UDS responses (DA44) for per-cell voltages / SOH ----
  if (rx_frame.ID == RSP_ID_DA44) {
    uint8_t pci = rx_frame.data.u8[0] & 0xF0;
    if (pci == 0x00) {
      iso_tp_len = rx_frame.data.u8[0] & 0x0F;
      memcpy(iso_tp_buf, &rx_frame.data.u8[1], iso_tp_len);
      iso_tp_idx = iso_tp_len;
    } else if (pci == 0x10) {
      iso_tp_len = ((rx_frame.data.u8[0] & 0x0F) << 8) | rx_frame.data.u8[1];
      if (iso_tp_len > sizeof(iso_tp_buf))
        iso_tp_len = sizeof(iso_tp_buf);
      memcpy(iso_tp_buf, &rx_frame.data.u8[2], 6);
      iso_tp_idx = 6;
      CAN_frame fc = {.FD = false, .ext_ID = true, .DLC = 8, .ID = REQ_ID_DA44,
                      .data = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0}};
      transmit_can_frame(&fc);
    } else if (pci == 0x20) {
      uint8_t n = (iso_tp_idx + 7 > iso_tp_len) ? (uint8_t)(iso_tp_len - iso_tp_idx) : 7;
      memcpy(&iso_tp_buf[iso_tp_idx], &rx_frame.data.u8[1], n);
      iso_tp_idx += n;
    }
    if (iso_tp_idx >= iso_tp_len && iso_tp_len >= 3) {
      decode_uds(datalayer_battery, iso_tp_buf, iso_tp_len, cellvoltages_mV);
      iso_tp_len = iso_tp_idx = 0;
    }
  }
}

void Fiat500eBattery::transmit_can(unsigned long currentMillis) {
  // TODO (needs trace): emulate the vehicle keep-alive frames + the "enable" frame that gates
  // contactor closing (Pro One uses 0x1D8). Without them a bench pack will not close contactors.
  // Optional: poll per-cell voltages over UDS (A100..A108) + SOH (A029) on DA44.
  if (currentMillis - previousMillisPoll < 500)
    return;
  previousMillisPoll = currentMillis;
  static const uint16_t uds_dids[] = {0xA100, 0xA101, 0xA102, 0xA103, 0xA104,
                                      0xA105, 0xA106, 0xA107, 0xA108, 0xA029};
  static uint8_t pi = 0;
  uint16_t did = uds_dids[pi];
  pi = (uint8_t)((pi + 1) % (sizeof(uds_dids) / sizeof(uds_dids[0])));
  CAN_frame req = {.FD = false, .ext_ID = true, .DLC = 8, .ID = REQ_ID_DA44,
                   .data = {0x03, 0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF), 0, 0, 0, 0}};
  transmit_can_frame(&req);
}

void Fiat500eBattery::update_values() {
  // min/max cell + deviation from the cell array (filled via UDS)
  uint16_t vmin = 0xFFFF, vmax = 0;
  for (int i = 0; i < NUMBER_OF_CELLS; i++) {
    uint16_t v = datalayer_battery->status.cell_voltages_mV[i];
    if (!v)
      continue;
    if (v < vmin)
      vmin = v;
    if (v > vmax)
      vmax = v;
  }
  if (vmax) {
    datalayer_battery->status.cell_max_voltage_mV = vmax;
    datalayer_battery->status.cell_min_voltage_mV = vmin;
  }

  // contactor gating from the broadcast status (0x306)
  datalayer_battery->status.battery_allows_contactor_closing = (contactor_status == CONTACTORS_ON);

  // charge power limit from DCCL (charge_limit_dA, 0.1A) * pack voltage
  if (datalayer_battery->status.voltage_dV && charge_limit_dA)
    datalayer_battery->status.max_charge_power_W =
        (uint32_t)charge_limit_dA * datalayer_battery->status.voltage_dV / 100;
  // TODO: max_discharge_power_W (500e has no 0x281; DDCL source TBD),
  //       voltage_dV / current_dA / temperatures (0x307 layout), remaining_capacity_Wh.
}
