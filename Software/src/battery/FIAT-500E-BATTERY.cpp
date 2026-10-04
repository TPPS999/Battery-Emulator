#include "FIAT-500E-BATTERY.h"
#include "../include.h"

// ============================================================================
// Fiat 500e (2020+) battery -- SCAFFOLD / WIP  (see FIAT-500E-BATTERY.h)
// Poll DA44 via UDS 0x22; scaling from OBDb/FIAT-500e + BDU firmware RE.
// NOT registered, NOT hardware-tested. Verify headers/scaling on a real car.
// ============================================================================

// ---- DIDs polled on DA44 (service 0x22) ----
static const uint16_t POLL_DIDS[] = {
    0xA010,  // SOC (min/max cell, pack, estimates)
    0xA029,  // SOH + nominal/current capacity
    0xA009,  // min/max cell voltage + number + min/max cell temperature
    0xA00A,  // HV battery current
    0xA011,  // pack voltage (cell sum / module sum / link)
    0xA200,  // module temperatures (18 sensors)
    0xA100, 0xA101, 0xA102, 0xA103, 0xA104, 0xA105, 0xA106, 0xA107,  // cell voltages 1..96
    0xA001,  // lifetime Ah / kWh
};
static const uint8_t NUM_POLL_DIDS = sizeof(POLL_DIDS) / sizeof(POLL_DIDS[0]);
static uint8_t poll_index = 0;

// big-endian unsigned read from the ISO-TP payload (payload starts at buf[3] = after 62 A0xx)
static uint32_t be(const uint8_t* buf, uint16_t byte_off, uint8_t len_bytes) {
  uint32_t v = 0;
  for (uint8_t i = 0; i < len_bytes; i++)
    v = (v << 8) | buf[3 + byte_off + i];
  return v;
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

// decode one fully-reassembled UDS positive response (buf = 62 A0 xx <data...>)
static void decode_did(Fiat500eBattery* self, DATALAYER_BATTERY_TYPE* dl, uint16_t did, const uint8_t* buf,
                       uint16_t len, uint16_t* cellv) {
  switch (did) {
    case 0xA010:  // SOC: 8-bit * 20/51 = %
      dl->status.real_soc = (uint16_t)((uint32_t)be(buf, 3, 1) * 20 * 100 / 51);  // 0.01%
      break;
    case 0xA029:  // SOH 16-bit / 655.35 %, capacities 16-bit / 10 Ah
      dl->status.soh_pptt = (uint16_t)((uint32_t)be(buf, 0, 2) * 10000UL / 65535UL);  // 0.01%
      // nominal cap be(buf,3,2)/10 Ah ; current cap be(buf,5,2)/10 Ah -> remaining_capacity_Wh TODO
      break;
    case 0xA00A:  // current: 16-bit /20 - 1638.4 A  -> dA (deciamps)
      dl->status.current_dA = (int16_t)((int32_t)be(buf, 0, 2) / 2 - 16384);  // (raw/20-1638.4)*10
      break;
    case 0xA011:  // pack voltage: cell sum 16-bit /10 V  -> dV == raw
      dl->status.voltage_dV = (uint16_t)be(buf, 0, 2);
      break;
    case 0xA009:  // max/min cell mV (16-bit), min/max cell temp (16-bit, verify scaling)
      dl->status.cell_max_voltage_mV = (uint16_t)be(buf, 2, 2);
      dl->status.cell_min_voltage_mV = (uint16_t)be(buf, 4, 2);
      break;
    case 0xA200: {  // 18 module temps, 8-bit, raw - 40 = C  -> dC
      int16_t tmin = 32767, tmax = -32768;
      for (uint8_t i = 0; i < 18; i++) {
        int16_t t = (int16_t)be(buf, i, 1) - 40;
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
      }
      dl->status.temperature_min_dC = tmin * 10;
      dl->status.temperature_max_dC = tmax * 10;
      break;
    }
    case 0xA100: case 0xA101: case 0xA102: case 0xA103:
    case 0xA104: case 0xA105: case 0xA106: case 0xA107: {  // 12 cell voltages (mV) each
      uint8_t base = (did - 0xA100) * 12;
      for (uint8_t i = 0; i < 12 && (base + i) < 96; i++)
        cellv[base + i] = (uint16_t)be(buf, i * 2, 2);
      if (did == 0xA107)
        memcpy(dl->status.cell_voltages_mV, cellv, 96 * sizeof(uint16_t));
      break;
    }
    case 0xA001:  // lifetime Ah/kWh 24-bit /10 -- diagnostics only, TODO expose
    default:
      break;
  }
}

void Fiat500eBattery::handle_incoming_can_frame(CAN_frame rx_frame) {
  if (rx_frame.ID != RSP_ID_DA44)
    return;
  datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;

  uint8_t pci = rx_frame.data.u8[0] & 0xF0;
  if (pci == 0x00) {  // single frame
    iso_tp_len = rx_frame.data.u8[0] & 0x0F;
    memcpy(iso_tp_buf, &rx_frame.data.u8[1], iso_tp_len);
    iso_tp_idx = iso_tp_len;
  } else if (pci == 0x10) {  // first frame -> send flow control, start reassembly
    iso_tp_len = ((rx_frame.data.u8[0] & 0x0F) << 8) | rx_frame.data.u8[1];
    if (iso_tp_len > sizeof(iso_tp_buf))
      iso_tp_len = sizeof(iso_tp_buf);
    memcpy(iso_tp_buf, &rx_frame.data.u8[2], 6);
    iso_tp_idx = 6;
    CAN_frame fc = {.FD = false, .ext_ID = true, .DLC = 8, .ID = REQ_ID_DA44,
                    .data = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0}};
    transmit_can_frame(&fc);
  } else if (pci == 0x20) {  // consecutive frame
    uint8_t n = 7;
    if (iso_tp_idx + n > iso_tp_len)
      n = iso_tp_len - iso_tp_idx;
    memcpy(&iso_tp_buf[iso_tp_idx], &rx_frame.data.u8[1], n);
    iso_tp_idx += n;
  }

  if (iso_tp_idx >= iso_tp_len && iso_tp_len >= 3 && iso_tp_buf[0] == 0x62) {
    uint16_t did = (iso_tp_buf[1] << 8) | iso_tp_buf[2];
    decode_did(this, datalayer_battery, did, iso_tp_buf, iso_tp_len, cellvoltages_mV);
    iso_tp_len = 0;
    iso_tp_idx = 0;
  }
}

void Fiat500eBattery::transmit_can(unsigned long currentMillis) {
  if (currentMillis - previousMillisPoll < 100)  // poll next DID every 100 ms
    return;
  previousMillisPoll = currentMillis;
  uint16_t did = POLL_DIDS[poll_index];
  poll_index = (poll_index + 1) % NUM_POLL_DIDS;
  CAN_frame req = {.FD = false, .ext_ID = true, .DLC = 8, .ID = REQ_ID_DA44,
                   .data = {0x03, 0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF), 0, 0, 0, 0}};
  transmit_can_frame(&req);
}

void Fiat500eBattery::update_values() {
  // min/max cell deviation + fallbacks from the cell array
  uint16_t vmin = 0xFFFF, vmax = 0;
  for (int i = 0; i < NUMBER_OF_CELLS; i++) {
    uint16_t v = datalayer_battery->status.cell_voltages_mV[i];
    if (v == 0)
      continue;
    if (v < vmin)
      vmin = v;
    if (v > vmax)
      vmax = v;
  }
  if (vmax) {
    if (!datalayer_battery->status.cell_max_voltage_mV)
      datalayer_battery->status.cell_max_voltage_mV = vmax;
    if (!datalayer_battery->status.cell_min_voltage_mV)
      datalayer_battery->status.cell_min_voltage_mV = vmin;
  }

  // TODO (needs RE firmware current-limit maps DCCL/DDCL, or DA42 torque limit):
  //   datalayer_battery->status.max_charge_power_W / max_discharge_power_W
  //   datalayer_battery->status.remaining_capacity_Wh (from A029 capacity * nominal V)
  datalayer_battery->status.battery_allows_contactor_closing = true;  // placeholder
}
