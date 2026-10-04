# Fiat 500e (2020+) HV battery -- reverse engineering findings (preliminary)

Condensed notes behind the Fiat 500e battery scaffold. Derived from reverse engineering the BDU/BPCM
firmware (Infineon AURIX TriCore TC27x) cross-checked with OBDb/FIAT-500e (UDS) and the Stellantis
Pro One driver/DBC. **Preliminary -- scaling/byte order/IDs to be confirmed with a real CAN log.**

Companion file: `FIAT-500E.dbc` (221 frames, 283 signals; loads in cantools/SavvyCAN).

## ECU / pack
- BDU/BPCM = diagnostic ECU **DA44** (battery pack control module).
- MCU: Infineon AURIX TC27x. Firmware is 108-cell-wide; two pack variants:
  - **96s2p** (96S/2P, 60Ah cells) -> ~120Ah, ~355V nom, ~408V max.
  - **108s1p** (108S/1P) -> ~459V max.
  Select via `FIAT500E_VARIANT_108S1P` in FIAT-500E-BATTERY.h.
- CAN: 11-bit broadcast on the internal bus; 29-bit diagnostics (UDS) on DA44.

## CAN frame matrix (from firmware)
Firmware holds an AUTOSAR Com/PduR matrix: **221 frame IDs (0x001-0x4D1), 279 signal records.**
Signal record gives start bit, bit length and a type that encodes the physical class:
- type 0x34 = 8-bit measurement (SOC / temperature)
- type 0x07/0x04/0x06/0x03 = 10-14 bit analog (voltage / current / power)
- type 0x31 = 1-bit flag, 0x35 = 2-bit state, 0x33 = 3-bit enum (fault level)
- type 0xff = 32-bit raw block (cell/module data)

Blocks:
- A control/status: 0x001, 0x002, 0x008, 0x00A, 0x0C3-0x0C5
- B pack data: 0x28E-0x311 (~20 analog V/I frames, ~28 8-bit SOC/temp frames, rest flags/states)
- C cell-sense: 0x315-0x4D1 (113 frames, 32-bit raw, 2/group) = per-cell/module data mirror

## Named frames (shared with Stellantis Pro One -- these are decoded in the scaffold)
| ID | Frame | Content |
|----|-------|---------|
| 0x306 | BPCM_SOC | SOC (byte4 coarse; bytes6-7 low 12b fine, full scale 4080) + contactor status byte5 [3:0] (8 off / 9 precharge / 10 on) |
| 0x307 | BPCM_Temperatures | temperatures (layout TBD) |
| 0x285 | BPCM_ChargeLimits | 3x u16 BE charge current limit [0.1A] (DCCL) |
| 0x359 | BPCM_LimitsCapacity | bytes0-1 OBC charge limit [0.1A]; bytes2-3 pack capacity [0.1Ah] |
| 0x2A5 | VEH_ChargeRequest | vehicle -> pack charge request |
| 0x2C6, 0x3ED, 0x308 | BPCM/VEH | present, content TBD |

Note: Pro One's 0x281 (discharge limit) is NOT in the 500e matrix -> DDCL source still TBD.

## UDS (DA44, service 0x22 ReadDataByIdentifier) -- for data not broadcast
A010 SOC, A029 SOH + nominal/current capacity, A009 min/max cell V+T, A00A current, A011 pack V,
A100-A108 cell voltages (96-108), A200 module temps, A001 lifetime Ah/kWh, B004-B00C per-cell SOC.
(Full DID map verified against real cars by OBDb/FIAT-500e.)

## Startup / contactor procedure (same family as Stellantis)
Pack closes contactors only when it "sees the car": vehicle keep-alive frames + an enable/command frame
(Pro One uses 0x1D8) + safety gates (HVIL / isolation / crash) pass -> precharge -> main contactor close,
with readback. Contactor status reported in 0x306 (8/9/10). Physical drive in firmware = DIO with
read-back verification. Bench use may need to emulate 0x2A5 (charge request) and possibly a service /
isolation-bypass mode (UDS 0x27 SecurityAccess; firmware has crypto).

## Status / TODO (needs a real CAN log)
- Confirm broadcast IDs, byte order and scaling for all non-named frames in FIAT-500E.dbc.
- Identify the contactor "enable" frame and DDCL source.
- Confirm diagnostic header (DA44 vs 740/742 observed on some cars) and whether a session is required.
- Then: fill the DBC, finish the decoder, register the battery in BatteryType / BATTERIES.cpp.
