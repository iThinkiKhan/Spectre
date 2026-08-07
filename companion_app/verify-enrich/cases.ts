/**
 * Enrichment CORRECTNESS verification.
 *
 * Goal (the thing throughput metrics never proved): the lat/lon written to a
 * record is the phone's GPS fix at that record's TRUE capture time, matched by
 * absolute UTC. We drive the REAL companion-app matching + encoding code with
 * hand-built data where the right answer is known, and we mirror the device's
 * C++ decode + epoch-derivation math (verified by static read of
 * StorageManager.cpp / BLEManager.cpp) to close the loop end to end.
 *
 * Trusted by assumption (per user): phone GPS coordinate accuracy + NTP/clock
 * accuracy. We are testing the PIPELINE that joins coord<->record by time.
 */
import {
  eventTimestampToUnixMs,
  encodeEnrichmentRecords,
  PHONE_ENRICH_FLAG_NO_DATA,
} from '../src/protocol/binary.ts';
import type {EnrichmentRecord, EventBatchRecord} from '../src/protocol/types.ts';
import {
  buildLocationCandidates,
  locationForUnixMsFromCandidates,
  rememberLocationSample,
  LOCATION_BOOTSTRAP_MAX_DRIFT_MS,
} from '../src/services/enrichment/LocationHistoryStore.ts';
import type {LocationHistoryFix} from '../src/services/enrichment/LocationHistoryStore.ts';

// ───────────────────────── tiny assert harness ──────────────────────────────
let passed = 0;
let failed = 0;
const lines: string[] = [];
function check(name: string, cond: boolean, detail = '') {
  if (cond) {
    passed += 1;
    lines.push(`  PASS  ${name}${detail ? `  (${detail})` : ''}`);
  } else {
    failed += 1;
    lines.push(`  FAIL  ${name}${detail ? `  (${detail})` : ''}`);
  }
}
function approx(a: number, b: number, eps: number) {
  return Math.abs(a - b) <= eps;
}

// ─── device-side mirrors (NOT under test; they replicate the C++ I read) ─────
// BLEManager.cpp:3351  out.lat = latE7 / 1e7 ; out.gpsEpochUtc = record.epochUtc
function deviceDecodeEnrichment(b64: string) {
  // mirror of encodeEnrichmentRecords layout (binary.ts) + C++ field reads
  const bin = Buffer.from(b64, 'base64');
  const dv = new DataView(bin.buffer, bin.byteOffset, bin.byteLength);
  return {
    eventId: dv.getUint32(0, true),
    lat: dv.getInt32(4, true) / 1e7,
    lon: dv.getInt32(8, true) / 1e7,
    altCm: dv.getInt32(12, true),
    accuracyDm: dv.getUint16(16, true),
    gpsEpochUtc: dv.getUint32(18, true),
    flags: dv.getUint8(22),
  };
}
// StorageManager.cpp:635  _epochFromBaseDelta(deltaMs, base) = base + deltaMs/1000  (integer)
function epochFromBaseDelta(deltaMs: number, baseEpochUtc: number) {
  if (baseEpochUtc === 0) return 0;
  return baseEpochUtc + Math.floor(deltaMs / 1000);
}

// ─── phone-side glue (copied 1:1 from SpectreContext.tsx so we exercise the
//     same path: eventTimestampToUnixMs -> locationForUnixMsFromCandidates ->
//     E7 round + Math.floor(ts/1000), then real encodeEnrichmentRecords) ─────
function buildWireRecordForEvent(
  event: EventBatchRecord,
  history: LocationHistoryFix[],
): EnrichmentRecord | null {
  const candidates = buildLocationCandidates(null, history);
  const eventUnixMs = eventTimestampToUnixMs(event.timestampMs);
  if (!eventUnixMs) return null;
  const fix = locationForUnixMsFromCandidates(eventUnixMs, candidates);
  if (!fix) return null;
  return {
    eventId: event.eventId,
    latE7: Math.round(fix.lat * 10_000_000),
    lonE7: Math.round(fix.lon * 10_000_000),
    altCm: Math.round(fix.alt * 100),
    accuracyDm: Math.max(0, Math.round(fix.accuracy * 10)),
    epochUtc: Math.max(0, Math.floor(fix.timestamp / 1000)),
    flags: 0,
    tag: '',
  };
}

function devFix(timestampMs: number, lat: number, lon: number): LocationHistoryFix {
  return {lat, lon, alt: 0, accuracy: 5, timestamp: timestampMs, source: 'device', provider: 'fused'};
}

const BASE_SEC = Math.floor(Date.UTC(2026, 7, 7, 12, 0, 0) / 1000); // current test day
const BASE_MS = BASE_SEC * 1000;

// ═════════════════════════════ TEST A: units ════════════════════════════════
lines.push('A. eventTimestampToUnixMs — wire field is epoch SECONDS, ×1000 -> ms');
check('seconds -> ms', eventTimestampToUnixMs(BASE_SEC) === BASE_MS, `${BASE_SEC}s -> ${BASE_MS}ms`);
check('rejects sub-2020 / boot-relative millis', eventTimestampToUnixMs(123456) === null, 'value < 1.6e9 -> null');
check('uint32 max still seconds (no ms overflow possible)', eventTimestampToUnixMs(4_000_000_000) === 4_000_000_000_000);

// ═══════════════════ TEST B/C: nearest-in-time picks RIGHT fix ═══════════════
lines.push('');
lines.push('B/C. Nearest-neighbor selects the temporally closest fix');
{
  const history = [
    devFix(BASE_MS - 0, 47.610000, -122.330000),
    devFix(BASE_MS + 60_000, 47.620000, -122.340000),
    devFix(BASE_MS + 120_000, 47.630000, -122.350000),
  ];
  const cand = buildLocationCandidates(null, history);
  const exact = locationForUnixMsFromCandidates(BASE_MS + 60_000, cand);
  check('exact match -> exact fix', !!exact && exact.lat === 47.62, `lat=${exact?.lat}`);
  const closerToMid = locationForUnixMsFromCandidates(BASE_MS + 61_000, cand); // 1s after mid
  check('t+61s -> mid fix (1s vs 59s)', !!closerToMid && closerToMid.lat === 47.62, `lat=${closerToMid?.lat}`);
  const closerToLast = locationForUnixMsFromCandidates(BASE_MS + 95_000, cand); // 35s vs 25s
  check('t+95s -> last fix (25s vs 35s)', !!closerToLast && closerToLast.lat === 47.63, `lat=${closerToLast?.lat}`);
}

// ═══════════════════════ TEST D: ±5min gate boundary ════════════════════════
lines.push('');
lines.push(`D. Match gate is +/-${LOCATION_BOOTSTRAP_MAX_DRIFT_MS / 1000}s (defer beyond it, never wrong coords)`);
{
  const history = [devFix(BASE_MS, 47.6, -122.3)];
  const cand = buildLocationCandidates(null, history);
  const inWindow = locationForUnixMsFromCandidates(BASE_MS + 4 * 60_000, cand);
  check('4 min away -> matched', !!inWindow, 'within 300s gate');
  const atEdge = locationForUnixMsFromCandidates(BASE_MS + 300_000, cand);
  check('exactly 300s -> matched (<=)', !!atEdge);
  const beyond = locationForUnixMsFromCandidates(BASE_MS + 301_000, cand);
  check('301s away -> DEFERRED (null), not a wrong fix', beyond === null);
}

{
  const noData: EnrichmentRecord = {
    eventId: 77,
    latE7: 0,
    lonE7: 0,
    altCm: 0,
    accuracyDm: 0,
    epochUtc: 0,
    flags: PHONE_ENRICH_FLAG_NO_DATA,
    tag: '',
  };
  const decoded = deviceDecodeEnrichment(encodeEnrichmentRecords([noData]));
  check('terminal no-data preserves event ID', decoded.eventId === 77);
  check('terminal no-data flag survives wire encoding',
    (decoded.flags & PHONE_ENRICH_FLAG_NO_DATA) !== 0);
}

// ═════════════ TEST E: full round trip — phone match -> wire -> device ═══════
lines.push('');
lines.push('E. END-TO-END: capture@T -> match -> E7 encode -> device decode == true fix');
{
  const TRUE_LAT = 47.611234;
  const TRUE_LON = -122.337891;
  const history = [
    devFix(BASE_MS - 30_000, 47.600000, -122.300000),
    devFix(BASE_MS + 0, TRUE_LAT, TRUE_LON), // the fix AT capture time
    devFix(BASE_MS + 30_000, 47.650000, -122.380000),
  ];
  const event: EventBatchRecord = {
    eventId: 4242,
    timestampMs: BASE_SEC, // device sends epoch SECONDS (pending.epochUtc)
    type: 0, status: 0, lane: 1, priority: 3,
  };
  const wire = buildWireRecordForEvent(event, history);
  check('event produced an enrichment record', !!wire);
  if (wire) {
    const b64 = encodeEnrichmentRecords([wire]);
    const dec = deviceDecodeEnrichment(b64);
    // E7 precision is ~1.1cm => 1e-7 deg tolerance
    check('decoded lat == true fix', approx(dec.lat, TRUE_LAT, 1e-7), `${dec.lat} vs ${TRUE_LAT}`);
    check('decoded lon == true fix', approx(dec.lon, TRUE_LON, 1e-7), `${dec.lon} vs ${TRUE_LON}`);
    check('decoded eventId preserved', dec.eventId === 4242);
    check('persisted gps_ts == matched fix epoch (s)', dec.gpsEpochUtc === BASE_SEC, `${dec.gpsEpochUtc}`);
    const matchErrorSec = dec.gpsEpochUtc - event.timestampMs;
    check('match error == 0 for an exact-time fix', matchErrorSec === 0, `Δt=${matchErrorSec}s`);
  }
}

// ═══════ TEST F: the analysis nobody ran — Δt distribution + offset detector ═
lines.push('');
lines.push('F. Match-tightness Δt = gps_ts − capture, and systematic-offset detection');
{
  // Dense, realistic history: a fix every 10s for an hour, each coord unique.
  const history: LocationHistoryFix[] = [];
  for (let i = 0; i < 360; i += 1) {
    history.push(devFix(BASE_MS + i * 10_000, 47.6 + i * 1e-4, -122.3 - i * 1e-4));
  }
  const cand = buildLocationCandidates(null, history);
  // 20 events at arbitrary capture times within the hour.
  const drifts: number[] = [];
  for (let k = 0; k < 20; k += 1) {
    const captureSec = BASE_SEC + 137 * k + 5; // not aligned to 10s grid
    const fix = locationForUnixMsFromCandidates(captureSec * 1000, cand);
    if (fix) drifts.push(fix.timestamp / 1000 - captureSec);
  }
  const absMax = Math.max(...drifts.map(Math.abs));
  const median = drifts.slice().sort((a, b) => a - b)[Math.floor(drifts.length / 2)];
  check('all matched within half the 10s sample spacing', absMax <= 5, `maxΔ=${absMax}s`);
  check('median drift ≈ 0 (no systematic offset)', Math.abs(median) <= 5, `median=${median}s`);

  // Inject a +1h offset into the HISTORY clock (simulating a TZ/epoch bug on the
  // fix timestamps). Correct behavior: every event now DEFERS (nearest fix is
  // an hour away, well beyond the 300s gate) — a gross bug surfaces as mass
  // deferral, NOT as silently-wrong coordinates.
  const shifted = history.map(f => devFix(f.timestamp + 3_600_000, f.lat, f.lon));
  const shiftedCand = buildLocationCandidates(null, shifted);
  let deferred = 0;
  for (let k = 0; k < 20; k += 1) {
    const captureSec = BASE_SEC + 137 * k + 5;
    if (!locationForUnixMsFromCandidates(captureSec * 1000, shiftedCand)) deferred += 1;
  }
  check('+1h history offset -> ALL defer (gate refuses wrong coords)', deferred === 20, `${deferred}/20 deferred`);

  // LIMITATION (important): a sub-gate, one-sided clock offset is INVISIBLE to
  // Δt. The phone matches against whatever timestamp the device SENT and reports
  // the matched fix's own timestamp, so a +120s skew just selects a fix ~120s of
  // travel away while gps_ts tracks the (skewed) sent time -> Δt stays ~0. The
  // coords are wrong but the record's own numbers look perfect. Δt validates
  // matcher TIGHTNESS, not cross-clock CORRECTNESS. (Earlier this asserted Δt
  // would catch it — it cannot; that was the bug, in the test, not the pipeline.)
  const small = history.map(f => devFix(f.timestamp + 120_000, f.lat, f.lon));
  const smallCand = buildLocationCandidates(null, small);
  const biased: number[] = [];
  for (let k = 0; k < 20; k += 1) {
    const captureSec = BASE_SEC + 137 * k + 5;
    const fix = locationForUnixMsFromCandidates(captureSec * 1000, smallCand);
    if (fix) biased.push(fix.timestamp / 1000 - captureSec);
  }
  const biasedMedian = biased.slice().sort((a, b) => a - b)[Math.floor(biased.length / 2)];
  check('sub-gate offset is INVISIBLE to Δt (matcher tightness ≠ cross-clock skew)', Math.abs(biasedMedian) <= 5, `median=${biasedMedian}s — only the ±300s gate or external truth catches a one-sided skew`);
}

// ═══════════ TEST G: device epoch derivation (C++ formula, mirrored) ═════════
lines.push('');
lines.push('G. Device epoch = createdEpochUtc + tsDelta/1000 (verify formula + truncation bound)');
{
  const createdEpochUtc = BASE_SEC; // segment base stamped at createdMs
  check('delta 0 -> base', epochFromBaseDelta(0, createdEpochUtc) === BASE_SEC);
  check('delta 60_000ms -> base+60s', epochFromBaseDelta(60_000, createdEpochUtc) === BASE_SEC + 60);
  check('delta 1500ms -> base+1s (sub-second TRUNCATES, ≤1s loss)', epochFromBaseDelta(1500, createdEpochUtc) === BASE_SEC + 1);
  check('delta 999ms -> base+0s (max truncation < 1s)', epochFromBaseDelta(999, createdEpochUtc) === BASE_SEC);
  check('base 0 (no trusted clock) -> 0 / unenrichable', epochFromBaseDelta(123456, 0) === 0);
}

// ═══════ TEST H: stationary averaging cuts single-fix GPS jitter ═════════════
lines.push('');
lines.push('H. Stationary accuracy-weighted averaging (rememberLocationSample)');
{
  const TRUE_LAT = 47.620000;
  const TRUE_LON = -122.350000;
  const M_PER_DEG_LAT = 111_320;
  const mPerDegLon = M_PER_DEG_LAT * Math.cos((TRUE_LAT * Math.PI) / 180);
  const errMeters = (lat: number, lon: number) => {
    const dy = (lat - TRUE_LAT) * M_PER_DEG_LAT;
    const dx = (lon - TRUE_LON) * mPerDegLon;
    return Math.sqrt(dx * dx + dy * dy);
  };
  // Seeded LCG + Box–Muller so the jitter is deterministic across runs.
  let seed = 1234567;
  const rand = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  const gauss = () =>
    Math.sqrt(-2 * Math.log(rand() + 1e-12)) * Math.cos(2 * Math.PI * rand());
  const SIGMA_M = 5; // typical horizontal jitter

  let history: LocationHistoryFix[] = [];
  const rawErrors: number[] = [];
  const N = 40;
  for (let i = 0; i < N; i += 1) {
    const lat = TRUE_LAT + (gauss() * SIGMA_M) / M_PER_DEG_LAT;
    const lon = TRUE_LON + (gauss() * SIGMA_M) / mPerDegLon;
    rawErrors.push(errMeters(lat, lon));
    history = rememberLocationSample(history, {
      lat, lon, alt: 0, accuracy: SIGMA_M, timestamp: BASE_MS + i * 10_000,
      source: 'device', provider: 'fused',
    });
  }
  const rawRms = Math.sqrt(rawErrors.reduce((s, e) => s + e * e, 0) / rawErrors.length);
  // The latest device marker holds the running average.
  const last = history.filter(f => f.source === 'device').at(-1)!;
  const avgErr = errMeters(last.lat, last.lon);
  check('averaged marker beats single-fix RMS jitter', avgErr < rawRms * 0.6, `avg=${avgErr.toFixed(2)}m vs rawRMS=${rawRms.toFixed(2)}m`);
  check('reported accuracy reflects √N reduction (≥ floor)', last.accuracy >= 2.5 && last.accuracy < SIGMA_M, `acc=${last.accuracy.toFixed(2)}m`);
  const markerCount = history.filter(f => f.source === 'device').length;
  // ~1 heartbeat marker per 60s over 400s (≈7); NOT one-per-fix (40 = cluster
  // fragmented). The bounded range catches both regressions.
  check('heartbeat coverage without fragmentation', markerCount >= 6 && markerCount <= 10, `${markerCount} markers over ${((N * 10) / 60).toFixed(1)}min (expect ~7)`);

  // Moving away splits the cluster: a fix well beyond the motion radius becomes
  // its own marker (not folded into the stationary mean).
  const beforeCount = history.filter(f => f.source === 'device').length;
  history = rememberLocationSample(history, {
    lat: TRUE_LAT + 0.002, lon: TRUE_LON + 0.002, alt: 0, accuracy: SIGMA_M,
    timestamp: BASE_MS + N * 10_000 + 10_000, source: 'device', provider: 'fused',
  });
  const movedMarker = history.filter(f => f.source === 'device').at(-1)!;
  check('motion (>200m) starts a new marker, not folded', history.filter(f => f.source === 'device').length === beforeCount + 1 && errMeters(movedMarker.lat, movedMarker.lon) > 100, `moved err=${errMeters(movedMarker.lat, movedMarker.lon).toFixed(0)}m`);
}

// ───────────────────────────────── report ───────────────────────────────────
console.log('\n══════════ ENRICHMENT CORRECTNESS VERIFICATION ══════════\n');
console.log(lines.join('\n'));
console.log(`\n──────────────────────────────────────────────────────────`);
console.log(`  ${passed} passed, ${failed} failed`);
console.log(`──────────────────────────────────────────────────────────\n`);
process.exit(failed === 0 ? 0 : 1);
