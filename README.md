<p align="center">
  <img src="docs/assets/spectre_banner.png" alt="Spectre banner" width="100%">
</p>

# Spectre

<p align="center">
  <b>Pocket field lab. Living RF map. Cyberpunk UI. $20 hardware.</b>
</p>

<p align="center">
  <b>Spectre is a cyberpunk field node that extends a stationary RF sensor network with GPS-tagged collection, autonomous sync, companion-assisted enrichment, and a modular radio toolkit for real-world wireless reconnaissance.</b>
</p>

<p align="center">
  <img alt="Platform" src="https://img.shields.io/badge/platform-ESP32--S3-black?style=for-the-badge">
  <img alt="UI" src="https://img.shields.io/badge/UI-LVGL%20Cyberpunk-fce700?style=for-the-badge">
  <img alt="BLE" src="https://img.shields.io/badge/BLE-P256%20%2B%20AES--GCM-00f0ff?style=for-the-badge">
  <img alt="Storage" src="https://img.shields.io/badge/storage-binary%20spool-ff003c?style=for-the-badge">
</p>

---

## What is Spectre?

Spectre is a pocket-sized field intelligence platform for extending a stationary RF sensor network into the real world.

It collects wireless activity in the field, tags it with location and mission context, stores it locally, and syncs it back home to enrich a larger RF database. With the companion phone app, Spectre can use GPS, relay storage, and MQTT bridging during extended missions. Back at base, collected data can feed map overlays, trilateration workflows, drone tracking, and long-term RF environment analysis.

Think of it as the cheapest homemade cyberpunk field lab you can actually build.

---

## Why it matters

| Capability | What it means |
|---|---|
| **Living RF Map** | GPS-tagged collection + home sync turns field observations into database-backed map overlays. |
| **Network Extension** | A stationary sensor network gains a mobile scout that can validate, enrich, and expand coverage. |
| **Companion-Assisted Missions** | A phone can provide GPS, storage relay, and MQTT bridging when the mission needs more reach. |
| **Autonomous Field Lab** | Spectre can operate without the phone, collect data, classify events, and sync later. |
| **Cheap + Modular** | Built around inexpensive ESP32-S3 hardware with GPIO expansion for new radios, sensors, and mission modules. |

---

## Core features

### Living RF Mapping

Spectre turns field observations into spatial intelligence. GPS-tagged collection and synchronized home uploads allow RF activity to be layered over satellite or other map imagery, creating a continuously improving view of the environment.

### Stationary Network Extension

Spectre is a mobile node for a broader sensor ecosystem. It brings field mobility to a stationary sensing architecture, helping expand coverage, validate detections, and improve the database behind the network.

### Companion Enrichment

The Android companion can provide GPS, relay data, extend storage, and bridge to MQTT. Spectre discovers the phone, connects, authenticates, encrypts the session, exchanges batches, and writes enriched records back to storage.

Recent bench result:

> **18-record encrypted BLE enrichment batch written back to storage in roughly 500 ms.**

That gets the radio back in the action instead of leaving the device stuck in housekeeping.

### Advanced Storage Engine

Spectre uses a compact binary spool for high-density field storage, with JSON exposed at the edges for debugging, export, and readability.

Highlights:

- binary event spool
- JSONL compatibility paths
- segment headers and summaries
- pressure-triggered compaction
- mission/noise lanes
- priority classes
- retention policy changes under storage pressure
- deduplicated event handling
- enrichment deltas
- session-aware upload watermarks

The result is a tiny embedded device that can hold a surprisingly large amount of useful RF history.

### Power-Aware Runtime

Spectre includes a custom power manager designed for real field use. It estimates battery state, charging state, voltage trend, discharge current, and remaining runtime.

Use a LiPo that fits your build, plug it into the JST connector, and Spectre handles the runtime logic automatically.

### Drone Tracking

Spectre includes OpenDroneID / Remote ID parsing support and is designed to fold drone observations into the same session, location, and database-aware workflow as the rest of the platform.

Instead of treating drone data as a one-off screen, Spectre treats it as part of a larger RF intelligence picture.

### Sub-GHz, LoRa, and Future Mesh

Spectre is not locked to WiFi and BLE. The platform already includes Sub-GHz and LoRa-oriented plumbing, with Meshtastic support planned.

The long-term idea is simple: Spectre should be a modular field node, not a single-purpose gadget.

### Dynamic Operator Tools

Spectre is designed for live field scenarios, not just passive logging.

Current and planned operator tools include:

- mission tagging
- session awareness
- location tagging
- on-device status and context
- BadUSB payload vault
- basic Ducky-style interpreter
- over-the-air script/payload workflows
- companion-assisted text/input flows

### Cyberpunk LVGL Interface

The UI is fully custom, built around a Cyberpunk 2077-inspired LVGL theme.

It includes:

- animated boot sequence
- system-check cascade
- Orbitron / Space Mono / Share Tech fonts
- neon yellow/cyan/red/green palette
- themed status panels
- mascot-driven feedback
- Halloween ghost mascot with props, moods, and animated states

The goal is not just to work. The goal is to feel like real field gear.

---

## Technical highlights

| System | Highlight |
|---|---|
| **BLE companion** | App-layer P-256 authentication with AES-GCM-protected data exchange. |
| **Enrichment speed** | 18-record encrypted companion batch written back to storage in about 500 ms. |
| **Storage density** | Custom binary compaction targets roughly 20k records in 8 MB of onboard storage. |
| **Storage resilience** | Segment summaries, audits, compaction, pressure modes, and retention policies. |
| **Data model** | Session, mission, location, enrichment, upload, and priority context are preserved. |
| **Power manager** | Battery SOC, voltage trend, charging/source detection, current estimate, and runtime estimate. |
| **UI system** | Full LVGL Cyberpunk theme with animated boot, status screens, and mascot state machine. |
| **Radio platform** | WiFi, BLE, Sub-GHz, LoRa, OpenDroneID, PMKID export, and future Meshtastic growth. |
| **Operator workflows** | Mission mode, companion relay, home sync, BadUSB vault, and dynamic field tooling. |

---
