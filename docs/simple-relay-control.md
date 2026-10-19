# Simple USB / battery / NRST control

This design has only two power relays and one NRST transistor.

The preferred version is the editable [KiCad schematic](simple-relay-control.sch). Open it in KiCad with the accompanying [local symbol cache](simple-relay-control-cache.lib), then save it as the current `.kicad_sch` format. The SVG is only a fallback preview.

```text
USB +5 V  ───── K1 COM/NO ───── TARGET_USB_5V
Battery + ───── K2 COM/NO ───── TARGET_BAT+

USB GND ───────┐
Battery − ─────┼──────── COMMON_GND
Pi GND ────────┘

Pi GPIO26 ─ 1 kΩ ─ gate Q3 BSS138
Q3 source ──────── COMMON_GND
Q3 drain ───────── Target NRST
```

Keep all grounds common. Switch only the positive wires; switching target ground would remove the SWD signal reference. K1 and K2 must not be closed together if they feed the same target rail. Use break-before-make in software.

Each relay coil needs a low-side driver:

```text
Pi +5 V ─── relay coil ─── drain Q1/Q2
                 │          source ─ GND
                 └─ diode ──┘

GPIO5 ─ 1 kΩ ─ gate Q1   (K1 USB relay)
GPIO6 ─ 1 kΩ ─ gate Q2   (K2 battery relay)
Each gate: 100 kΩ to GND
```

Put the flyback diode across each coil with its cathode toward Pi +5 V. NRST needs a pull-up to the target I/O voltage; use the target board's existing pull-up if present. Q3 pulls NRST low when GPIO26 is high.

## Minimal BOM

| Ref | Part | Qty |
|---|---|---:|
| K1, K2 | 5 V SPDT relay, e.g. Songle SRD-05VDC-SL-C | 2 |
| Q1, Q2 | 2N7000 or AO3400A | 2 |
| Q3 | BSS138 or 2N7000 | 1 |
| D1, D2 | 1N4007 or 1N4148 flyback diode | 2 |
| R1, R2, R3 | 1 kΩ gate resistor | 3 |
| R4, R5 | 100 kΩ gate pulldown | 2 |
| R6 | 10 kΩ NRST pull-up, only if target lacks one | 1 |
| J1 | USB +5 V/GND connector | 1 |
| J2 | Battery +/− connector | 1 |
| J3 | Target/SWD connector | 1 |

If the battery is a 1-cell Li-ion battery, its raw voltage is 3.0–4.2 V, not 5 V. K2 can feed it directly only if the target accepts that voltage; otherwise K2 must feed a boost converter.
