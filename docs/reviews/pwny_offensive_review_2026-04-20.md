# Spectre Offensive Feature Review

**Date:** 2026-04-20
**Scope:** All offensive extras — pwny (deauth/PMKID), BadUSB, and BLE/LoRa/sub-GHz adjacent surfaces
**Baseline references:** [Ghost_ESP](https://github.com/Spooks4576/Ghost_ESP), [ESP32Marauder v7](https://github.com/justcallmekoko/ESP32Marauder/wiki/Marauder-v7)
**Constraint (from project memory):** *Spectre is a field edge sensor. The mission is capture → store → upload. Pwny/BadUSB are extras and must not compromise the core mission or the just-stabilized MQTT upload path.*

---

## TL;DR

Spectre's offensive layer is *different in kind* from Ghost_ESP and Marauder, not strictly smaller. The reference projects are interactive attack toolkits — operator sits at a screen, picks a target, fires a frame. Spectre is a passive behavioral-intelligence sensor with a small, phased, opportunistic attack loop bolted on for handshake harvesting. The right model for Spectre is **"sensor with teeth,"** not "pocket Marauder."

Given that framing, the highest-leverage improvements are:

1. **Close real gaps in capture quality** — PMKID path needs the 4-way-handshake completion state that Marauder/Ghost track, not more attack variety.
2. **Steal structural patterns** — scan→select→attack pipeline, CLI twin, save/load command scripting, universal stopscan — these help *without* expanding attack surface.
3. **Resist the urge to add noisy toys** — beacon spam, AP clone spam, evil portal, Flipper/Sour Apple BLE spam — these would *regress* Spectre's passive-intelligence strengths and fight the radio arbiter for airtime that should be spent listening.

The specific recommendations below are ordered by leverage, not by novelty.

---

## 1. What Spectre already does better than the reference projects

It is worth naming these explicitly because every suggestion below should preserve or extend them, not trade them away.

**Behavioral device fingerprinting.** `TrackedDevice` tracks IE fingerprint, IE order hash, MAC rotation via `physicalDeviceID`, SSID bloom filter, RSSI history with slope/trend, and vendor class. Neither reference project does any of this — they operate on MACs as ground truth. Spectre treats MAC as a noisy handle and reconstructs identity behaviorally. This is the single most valuable thing the platform has.

**Affinity and karma defense.** `AffinityPair` with Jaccard over SSID bloom filters, plus `KarmaAlert` surfacing probe-for-SSIDs-an-AP-just-claimed patterns, is qualitatively ahead of what either reference project ships. Marauder has Karma, Ghost_ESP has probe logging, but neither correlates probe history across identity-rotated devices.

**Phased, rate-limited pwny.** The `_pwnyTick` state machine's passive → deauth → cooldown phasing plus `PWNY_MAX_TX_PACKETS_PER_SEC` rate limiter is operationally saner than Ghost_ESP/Marauder's "fire until user stops" model. The phased approach also improves handshake capture odds by letting the STA settle before the next prompt.

**Radio arbiter with explicit leases.** `RadioArbiter` as a single-owner lease system is the right substrate and neither reference has anything like it. It is also what makes the rest of Spectre's mission-critical work possible (upload path, storage batch, BLE triggers). Do not let any offensive feature bypass it.

**Ownership contract enforcement.** `RuntimeContracts` with `CONTRACT_WARN_ONCE` already caught the MQTT worker disaster in spirit (the fix made the contract expressible). Worth leaning on heavily for any new offensive code.

**Mission system.** `MissionProfile` (RECON/PWNY/UPLINK) plus `MissionRuntime` is closer to how a serious operator actually runs a deployment than Marauder/Ghost_ESP's flat menu-of-tools UX.

---

## 2. Real gaps worth closing

Ordered by leverage per unit effort and risk to the capture→store→upload mission.

### 2.1 PMKID/handshake completion state (HIGH leverage, LOW risk)

**What the references do:** Marauder v7 and Ghost_ESP both track EAPOL message progression (M1/M2/M3/M4) explicitly and surface "captured handshake" vs "captured PMKID" vs "partial" as distinct states. Ghost_ESP even exposes per-BSSID crackability hints.

**What Spectre has:** `PwnyTarget` has an EAPOL msg mask (bits 0–3) and a `crackable` flag, and the code reads it. But it does not appear to be surfaced in UI or uploaded as a first-class `pmkid` vs `handshake` distinction in MQTT payloads. `MQTTManager.h` has a `TOPIC_PMKID` but I did not see a `TOPIC_HANDSHAKE` or an EAPOL completion field on the pmkid message.

**Suggestion:**
- Add an `eapolMask` byte to the PMKID MQTT record (non-breaking — servers that ignore unknown fields are fine).
- Add a UI glyph/ticker on the pwny mascot state showing "PMKID / M1+M2 / FULL 4-way" so the operator can tell at a glance whether to keep pressing or move on.
- In `_pwnyTick`, consider a soft early-exit: if `eapolMask == 0b1111` (full 4-way captured), advance to cooldown early rather than finishing the attack budget.

**Effort:** Small. The data is already tracked.
**Risk:** None — this is reporting richer information that is already collected.
**Alignment:** Directly serves the capture→store→upload mission.

### 2.2 SSID/BSSID scan → select → attack pipeline (MEDIUM leverage, LOW risk)

**What the references do:** Both expose a clean pattern: scan (populates a list), select (by index or MAC), attack (fire a specific mode at the selection). Marauder's CLI surface is `scanap` → `select -a <idx>` → `attack -t deauth`. Ghost_ESP does the same via menu.

**What Spectre has:** `_pwnyTick` picks targets implicitly via scoring, and the operator's agency is "turn pwny on / off." Good for autonomous field ops; worse for situations where the operator *knows* which AP they want to work.

**Suggestion:**
- Add a `pwnyPinTarget(const uint8_t bssid[6])` method on `WiFiManager` that, when set, constrains `_pwnyTick` to that BSSID only (skip scoring, skip rotation).
- Add a `pwnyClearPinnedTarget()` to return to autonomous mode.
- Expose it via BLE command and/or LoRa command, since you already have both transports.
- Keep the autonomous loop as default — the pin is a temporary operator override.

**Effort:** Small-to-medium. The state machine already supports per-target phase tracking; this is a constraint on target selection.
**Risk:** Low if the pin is additive (falls back to autonomous when clear).
**Alignment:** Fine — operator agency without breaking passive posture.

### 2.3 Universal stopscan / panic release (MEDIUM leverage, LOW risk)

**What the references do:** Marauder has a universal stopscan command that cancels any in-flight attack or scan. Ghost_ESP has a similar "stop everything" pattern.

**What Spectre has:** Implicit — turn off mission, let the state machines unwind. Works, but during a real-world "something's wrong, cut TX now" moment, the operator wants *one* handle.

**Suggestion:**
- Add `OffensiveAbort::now()` that: (a) cancels any pwny phase, (b) forces `RADIO_ARB.release()` if the current lease owner is an offensive op, (c) transitions MissionProfile to RECON, (d) leaves capture/store/upload untouched.
- Bind it to a long-press on whichever button is most accessible on the field case.
- Emit an `eventType=OFFENSIVE_ABORT` so the server side sees it.

**Effort:** Small.
**Risk:** Low if it only aborts offensive leases — do not let it preempt `RADIO_WIFI_UPLOAD`.
**Alignment:** Good. Improves field safety without changing capture behavior.

### 2.4 Save/load command scripting (MEDIUM leverage, MEDIUM risk)

**What the references do:** Marauder v7 added CLI save/load: write a sequence of commands to a file, load it, execute. Use case: canned recon pass tailored to a site.

**What Spectre has:** `MissionProfile` is close in spirit but is compiled in. No runtime definition of "the sequence of things I want to run on this deployment."

**Suggestion:**
- Define a minimal mission-script format on LittleFS: line-oriented, one directive per line, e.g. `wait 30s`, `mission recon`, `mission pwny`, `ble karma-on`, `abort`.
- Interpret from `TaskHardware` only, using the existing radio arbiter for every directive.
- Require a signed or at least checksummed header on scripts dropped over BLE/USB, to avoid a "plug in a malicious SD card and pwn your own sensor" foot-gun.

**Effort:** Medium. Parser + interpreter + persistence.
**Risk:** Medium — any runtime-defined behavior can misbehave. The single-task-caller contract on MQTT is a good pattern to repeat here: only TaskHardware executes directives, display only reads status mirrors.
**Alignment:** Good for field deployments; skippable if the operator surface is small.

### 2.5 Wardrive export in Wigle CSV format (LOW–MEDIUM leverage, LOW risk)

**What the references do:** Marauder writes Wigle-compatible CSV directly; Ghost_ESP has a similar export.

**What Spectre has:** Rich capture data that is uploaded as structured MQTT to your own backend, which is better — but if you ever want to share a dataset with the wardriving community, there is a format.

**Suggestion:** Add a one-shot "export last N AP observations as Wigle CSV" action that writes to LittleFS or streams over BLE. Low priority unless community sharing is a goal.

**Effort:** Small.
**Risk:** None — read-only export.
**Alignment:** Neutral. Ship only if there's a use case.

### 2.6 BadUSB scripting surface richness (LOW leverage for pen-test, LOW risk)

**What the references do:** Marauder and Ghost_ESP both include more Ducky-extension flavors (random delays, conditional waits, keystroke timing jitter) to evade naive EDRs.

**What Spectre has:** Ducky core (32ms chord hold, 12ms char delay, 3s countdown, 16 scripts). Functional but minimal.

**Suggestion:** Add `DELAY_RAND_MIN <ms>` / `DELAY_RAND_MAX <ms>` and `JITTER <ms>` directives. Small code change, small value unless BadUSB is actually part of the mission. My read of project memory is it is not.

**Effort:** Small.
**Risk:** Negligible (BadUSB only runs on explicit ARM + countdown).
**Alignment:** Low. Skip unless BadUSB graduates from "extra" to "mission."

---

## 3. Gaps that exist but aren't worth closing

I'd push back on implementing any of these under Spectre's current mission. Listing them explicitly so the "why not" is captured.

**Beacon / AP clone spam.** Ghost_ESP and Marauder both ship it. For Spectre it would (a) pollute the RF environment the sensor is simultaneously trying to observe, (b) burn radio arbiter airtime that should be spent listening, and (c) give the device away. Passive posture is the thing. *Skip.*

**Evil portal / JS keylogger / DNS spoof.** Ghost_ESP's flagship attack. Requires the ESP to become an AP, serve a portal, proxy DNS. Completely incompatible with promiscuous capture being the primary job. *Skip.*

**Flipper / Sour Apple / Swiftpair BLE spam.** Same argument as beacon spam, on the BLE side. Spectre's BLE radio is a command surface (BLE triggers for MQTT dump) and a passive observer. Spamming BLE advertisements fights both. *Skip.*

**DIAL / YouTube / Chromecast hijack, TP-Link exploit, network printer spam.** These are "party tricks" — high novelty, low operational value, and every one of them is a different protocol stack in the firmware. The maintenance cost is poor. *Skip.*

**SAE Commit Flood / Association Sleep Attack.** Both are WPA3/WPA2 DoS variants. They are interesting but pure denial; Spectre's pwny loop exists to *capture handshakes*, not to deny service. *Skip.*

**Card skimmer detection (Marauder).** Nice idea, but it's a BLE scan-and-match over a well-known skimmer signature list. Requires dataset maintenance. *Skip unless there's a concrete field use case.*

**AirTag / Flipper detection (Ghost_ESP).** This one I'd call *borderline* — it is passive, fits Spectre's posture, and just requires a signature list. If the operator community cares about it, implementation cost is low. Rated LOW-not-NO.

---

## 4. Structural / UX patterns worth borrowing

These are the things Marauder and Ghost_ESP do *as products* that Spectre could learn from without changing its mission.

**Scan → select → attack as a uniform shape.** Covered above in §2.2 for pwny. The same shape could apply to BLE (scan BLE devices → select → trigger targeted action) and sub-GHz once that pipeline matures.

**CLI twin for the GUI.** Marauder's CLI is coequal with the menu. Spectre already has BLE command and LoRa command transports; formalizing them as a documented command vocabulary (not ad-hoc per-feature) would make scripting in §2.4 free.

**Status bar / "what is this thing doing right now."** Both reference projects keep a persistent status line visible. Spectre has mascot state, which is richer but also more ambiguous. Consider a small, always-visible line of text under the mascot with "MODE: PWNY · LEASE: WIFI_PMKID · QUEUE: 47" so the operator always knows the machine state.

**One-command "I'm done, pack up."** Stopscan is part of this; a "pack up" sequence that drains pending captures, forces a final upload attempt, and then enters low-power standby is worth having.

**Documented fail-soft behaviors.** Ghost_ESP is better than Marauder here: every attack mode has a documented "what happens when I stop it." Spectre's mission architecture gives it a cleaner version of this; codifying it per mission would help future maintainers.

---

## 5. Prioritization matrix

| # | Item | Leverage | Effort | Risk | Alignment | Recommend |
|---|---|---|---|---|---|---|
| 2.1 | EAPOL mask in PMKID payload + mascot glyph | High | S | None | High | **Yes — do first** |
| 2.2 | Pinned pwny target via BLE/LoRa | Medium | S–M | Low | High | **Yes** |
| 2.3 | Universal offensive-abort handle | Medium | S | Low | High | **Yes** |
| 4.* | Status line under mascot | Medium | S | None | High | **Yes** |
| 4.* | Documented command vocabulary (BLE/LoRa) | Medium | M | Low | High | Yes, groundwork for 2.4 |
| 2.4 | Mission-script save/load | Medium | M | Medium | Medium | Later — after 4.* lands |
| 2.5 | Wigle CSV export | Low-Med | S | None | Neutral | Only if community sharing matters |
| 3.* | AirTag/Flipper signature detection | Low-Med | S | None | Medium | Optional |
| 2.6 | Ducky jitter directives | Low | S | None | Low | Skip unless BadUSB graduates |
| 3.* | Beacon/BLE spam, evil portal, DIAL, etc. | — | — | **High to mission** | Negative | **Do not add** |

---

## 6. Guardrails if any of this ships

Given the freshly-earned MQTT stability, any offensive feature work should obey the same pattern that made the MQTT fix stick:

**Single-task caller.** Any new offensive subsystem should be ticked only from `TaskHardware`. No detached FreeRTOS workers. If something *feels* like it needs a worker, that is the signal to instead add a phase to an existing state machine.

**RuntimeContracts from day one.** Every new ownership assumption gets a `CONTRACT_WARN_ONCE`. Past bugs were invisible until the contracts surfaced them.

**Radio arbiter lease or don't transmit.** No direct `esp_wifi_80211_tx` outside a held lease. The current `_sendPwnyMgmtFrame` path is fine; any new TX path needs the same discipline.

**Storage writes deferred via `beginUploadBatch`/`endUploadBatch` when radio is live.** This is already load-bearing for the upload path and the same argument applies to anything that streams records during active TX.

**Mission-gated, not menu-reachable.** Offensive features activate only under a matching `MissionProfile`. Do not add "just turn it on from the menu" shortcuts — they bypass the mission context the radio arbiter expects.

---

## 7. Suggested first PR

If you want a concrete next step that matches this review, I'd pick **§2.1 (EAPOL mask surfacing)** as the first PR. It is the highest-leverage item, it is entirely additive, it serves the capture→store→upload mission directly, and it gives the operator better feedback without needing any of the structural work. Everything else can wait until after that lands and the mascot glyph stabilizes.
