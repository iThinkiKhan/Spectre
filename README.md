# SPECTRE

<div align="center">

![SPECTRE — Pocket field lab. Living RF map. Cyberpunk fieldware. $20 hardware.](assets/banner-boot-final.png)

**Pocket field lab. Living RF map. Cyberpunk fieldware. $20 hardware.**

[![Platform](https://img.shields.io/badge/platform-ESP32--S3-FCE700?style=flat-square&labelColor=000)](#hardware)
[![Display](https://img.shields.io/badge/display-320×170%20TFT-00F0FF?style=flat-square&labelColor=000)](#display)
[![UI](https://img.shields.io/badge/ui-LVGL-00FF9C?style=flat-square&labelColor=000)](#ui)
[![Status](https://img.shields.io/badge/status-alpha-FF003C?style=flat-square&labelColor=000)](#status)
[![License](https://img.shields.io/badge/license-MIT-FCE700?style=flat-square&labelColor=000)](#license)

</div>

---

## `> spectre.boot`

SPECTRE is a cyberpunk-aesthetic **ESP32 field intelligence platform**. A 320×170px TFT, an LVGL framework, a ghost mascot with opinions. Designed to scan, log, and look good doing it.

```
> DISPLAY                 [ OK ]
> RADIO MODULE            [ OK ]
> STORAGE ENGINE          [ OK ]
> CRYPTO LAYER            [ OK ]
> GPS COMPANION           [ OK ]
> NEURAL LINK........[SEARCHING]

BOOT COMPLETE.
```

---

## `> mascot.states`

The mascot **is** the UI. Props communicate mode. No icon font, no SVG library — every glyph is procedurally drawn in C against an LVGL sprite buffer.

<div align="center">

| | | |
|:---:|:---:|:---:|
| ![Standby](assets/tile-standby.png) | ![Homelab Sync](assets/tile-homelab.png) | ![Bad USB](assets/tile-bad-usb.png) |
| ![PWNY // Viking](assets/tile-pwnagotchi.png) | ![Alert](assets/tile-alert.png) | |

</div>

| State | Props | Mode |
|---|---|---|
| **STANDBY** | Headphones + sat dish | Listening · passive scan |
| **HOMELAB SYNC** | Glasses + monitor | Offload · cold storage |
| **BAD USB** | Plug + sparks | Payload armed |
| **PWNY // VIKING** | Horned helm + axe + shield | Handshakes > 0 · raid mode |
| **ALERT** | Arms up + packet | Contact · track active |

---

## `> rf.map`

![SPECTRE Live RF Map](assets/banner-rf-final.png)

Six radios, one pocket. WiFi (2.4GHz), Sub-GHz (868/433MHz), BLE, LoRa, NFC (125kHz), IR. Scan results stream into a circular buffer. Top-N renders to the device sidebar; everything else flushes to SD on the next homelab sync.

---

## `> hardware`

| | |
|---|---|
| **MCU** | ESP32-S3 (dual-core, 240MHz, 8MB PSRAM) |
| **Display** | SSD1351-driven 320×170 TFT, 16-bit color |
| **Radio** | SX1276 (LoRa/sub-GHz), NRF24L01 (2.4GHz), built-in BLE/WiFi |
| **Storage** | µSD card, 32GB recommended |
| **Power** | 18650 Li-ion · ~4h12m active · USB-C charge |
| **BOM** | ~$20 USD |

---

## `> ui.system`

```
┌──────────────────── 320px ────────────────────┐
│ STATUS BAR                       24px         │
├──────┬────────────────────────────────────────┤
│      │                                        │
│  M   │  CONTENT AREA                          │
│  A   │  244 × 128                             │
│  S   │                                        │
│  C   │                                        │
│  O   │                                        │
│  T   │                                        │
├──────┴────────────────────────────────────────┤
│ ACTION BAR                       18px         │
└───────────────────────────────────────────────┘
```

**Palette** — pure black backgrounds, neon as text/lines/borders only. No gradients, no rounded corners, no drop shadows.

| Token | Hex | Use |
|---|---|---|
| `CLR_YELLOW` | `#FCE700` | Primary accent |
| `CLR_CYAN` | `#00F0FF` | Secondary accent |
| `CLR_GREEN` | `#00FF9C` | OK |
| `CLR_RED` | `#FF003C` | FAIL · alert · raid |
| `CLR_HOTPINK` | `#FF00FF` | Pwnagotchi evil mode |
| `CLR_WHITE` | `#FFFFFF` | Mascot body · default text |
| `CLR_DIM` | `#787878` | Borders, hairlines |

**Type** — Orbitron Bold (titles) · Space Mono (body) · Share Tech Mono (system labels). All compiled to LVGL bitmap format.

**Motion** — chromatic aberration glitch on title reveals · 22–28ms typewriter · frame-counter linear (no easing) · 120ms mascot sprite anim · concentric ring pulses for radio scan.

---

## `> file.index`

```
spectre/
├── ui/
│   ├── Theme.h              // color + font tokens
│   ├── Mascot.cpp           // procedural mascot states
│   ├── BootSequence.cpp     // glitch-in title, typewriter sys check
│   ├── LVGLDriver.cpp       // 320×170 framebuffer plumbing
│   └── MascotState.h        // enum of every mood
├── assets/                  // banners + mascot tiles (this README)
├── colors_and_type.css      // web-equivalent tokens
└── README.md
```

---

## `> license`

MIT. Build it, mod it, scan with it. Don't be a creep.

---

<div align="center">

`ready_`

</div>
