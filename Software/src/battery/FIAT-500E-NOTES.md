# Fiat 500e (2020+) battery -- integration notes (WIP scaffold)

Files: `FIAT-500E-BATTERY.{h,cpp}`. Poll-based (UDS 0x22) driver for the 2nd-gen 500e HV battery
pack control module (ECU **DA44**). Status: scaffold, **not registered, not HW-tested**.

Signal map/scaling: OBDb/FIAT-500e (verified on real cars) cross-checked with BDU firmware RE
(AURIX TC27x) -- see sibling project `fiat_500` (docs/mapowanie_can_obdb.md).

## Diagnostic addressing (FCA, 29-bit, 500 kbps) -- CONFIRM on car
- Request -> DA44: `0x18DA44F1`, Response: `0x18DAF144`, Flow control: `30 00 00`.

## DIDs polled (service 22) and scaling
| DID  | Signal                         | Bytes (payload) | Scaling -> unit            |
|------|--------------------------------|-----------------|----------------------------|
| A010 | HV SOC                         | byte3, u8       | raw*20/51 -> %             |
| A010 | min/max cell SOC, est1/est3    | byte0/1/2/4     | raw*20/51 -> %             |
| A029 | SOH                            | byte0..1, u16   | raw/655.35 -> %            |
| A029 | nominal / current capacity     | byte3..4 / 5..6 | raw/10 -> Ah               |
| A00A | HV current                     | byte0..1, u16   | raw/20 - 1638.4 -> A       |
| A011 | pack V (cell sum/module/link)  | byte0..1 ...    | raw/10 -> V                |
| A009 | max/min cell V                 | byte2..3/4..5   | raw -> mV                  |
| A009 | max/min cell number            | byte0/1         | scalar                     |
| A009 | max/min cell T                 | byte8../10..    | raw (verify offset) -> C   |
| A200 | 18 module temp sensors         | byte0..17, u8   | raw - 40 -> C              |
| A100-A107 | cell voltages 1..96       | 12x u16 / DID   | raw -> mV                  |
| A001 | lifetime Ah (x2), kWh (x2)     | u24 fields      | raw/10 -> Ah / kWh         |
| A021 | coolant inlet/outlet T         | u16             | (verify) -> C              |

## Pack variants -- the BDU firmware supports TWO (select FIAT500E_VARIANT_108S1P in the .h)
- **96s2p**  : 96S / 2P, 60Ah cells -> ~120 Ah, ~355 V nom, ~408 V max (96 * 4.25).
- **108s1p** : 108S / 1P                      -> ~459 V max (108 * 4.25).
The firmware's internal arrays/loops are 108-wide (108 == 0x6C is the dominant loop bound; `< 0x60`/96
appears mainly as index-range validation). Cell voltages span DIDs A100..A108 (9 frames x 12 = 108);
96s2p populates 96, 108s1p populates 108. Variant is config-driven (NVM/cal) -- confirm per car, or
detect from a cell-count DID. We poll A100..A108 and publish `info.number_of_cells` worth of cells.

## TODO before it works
1. Confirm 29-bit req/resp headers + whether a diagnostic session (10 03) / tester-present (3E) is
   needed before A0xx reads are allowed.
2. Verify A009 cell-temp and A021 scaling on a real car.
3. Fill `max_charge_power_W` / `max_discharge_power_W` from the firmware current-limit maps
   (DCCL/DDCL, see fiat_500 docs/kalibracja.md) or DA42 torque limit; fill `remaining_capacity_Wh`.
4. Add an HTML renderer (optional) like ECMP-HTML.h.

## Registration (shared files -- do when wiring in)
- `BATTERIES.h`: add `Fiat500e` to the `BatteryType` enum.
- `BATTERIES.cpp`: `#include "FIAT-500E-BATTERY.h"` and add `case BatteryType::Fiat500e:` to the
  name / `new` / battery2 / battery3 / setup switch blocks (mirror `StellantisEcmp`).
- Web UI battery selector enum (wherever BatteryType is surfaced).

## Architektura (v2): broadcast-primary (wzor Stellantis Pro One)
Pakiet 500e NADAJE stan na wew. szynie (11-bit) -- sterownik SLUCHA (jak Pro One), a UDS tylko
dopytuje to, czego nie ma w broadcast (napiecia ogniw A100-A108, SOH A029). Wspolne ramki (FW 500e
∩ Pro One), uzyte w dekoderze:
- 0x306 BPCM_SOC: SOC (bajty6-7 [11:0], pelna skala 4080) + stan stycznika bajt5 (8 off/9 precharge/10 on).
- 0x285 BPCM_ChargeLimits: 3x u16 BE prad ladowania [0.1A] (DCCL) -> max_charge_power_W.
- 0x359 Limits+Capacity: OBC charge limit [0.1A] + pojemnosc [0.1Ah].
- 0x307 Temperatures: layout TBD (nierozpisany tez w Pro One).
Procedura startu (contactor): emulowac ramki pojazdu + ramke "enable" (Pro One: 0x1D8) -> patrz
fiat_500/docs/procedura_startu.md. DDCL (discharge): 500e nie ma 0x281 -> zrodlo TBD.
Skalowanie = hipoteza Pro One, do potwierdzenia trace z auta.
