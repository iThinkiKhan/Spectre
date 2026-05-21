import React, {useCallback, useEffect, useRef, useState} from 'react';
import {Pressable, ScrollView, StyleSheet, Text, View} from 'react-native';

import {FieldPanel} from './FieldPanel';
import {
  CMD_OP_GET_DASHBOARD,
  CMD_OP_GET_HEALTH,
  CMD_OP_GET_LOG_TAIL,
  CMD_OP_GET_STATUS,
  CMD_OP_GET_STORAGE,
  CMD_OP_GET_WIO_STATUS,
  PHONE_DASHBOARD_STREAM_DEFAULT_LEASE_MS,
  PHONE_DASHBOARD_STREAM_INTERVAL_DEFAULT_MS,
  PHONE_LOG_STREAM_DEFAULT_LEASE_MS,
  PHONE_NOTIF_SEVERITY_CRITICAL,
  PHONE_NOTIF_SEVERITY_WARN,
  notificationTypeName,
} from '../protocol/contracts';
import type {
  CmdDashboardSnapshotV1,
} from '../protocol/types';
import type {
  DashboardStreamHandle,
  LogStreamHandle,
  SpectreCommandService,
} from '../services/peripheral/SpectreCommandService';
import {useSpectre} from '../state/SpectreContext';
import {theme} from '../theme/theme';

const MAX_STREAM_LINES = 200;

type ResultPhase = 'idle' | 'loading' | 'ok' | 'error';

type CommandResult = {
  phase: ResultPhase;
  body: string;
  ts: number;
};

const INITIAL: CommandResult = {phase: 'idle', body: '', ts: 0};

const TRANSPORT_NAMES = ['none', 'wio_ble', 'internal_ble'] as const;

function transportName(kind: number) {
  return TRANSPORT_NAMES[kind] ?? `unknown(${kind})`;
}

function formatMs(ms: number) {
  if (ms < 1000) return `${ms}ms`;
  if (ms < 60_000) return `${(ms / 1000).toFixed(1)}s`;
  if (ms < 3_600_000) return `${(ms / 60_000).toFixed(1)}m`;
  return `${(ms / 3_600_000).toFixed(1)}h`;
}

function formatBytes(n: number) {
  if (n < 1024) return `${n}B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)}KB`;
  return `${(n / (1024 * 1024)).toFixed(1)}MB`;
}

type RunArgs = {
  service: SpectreCommandService;
  setResult: (next: CommandResult) => void;
};

async function runStatus({service, setResult}: RunArgs) {
  const t = Date.now();
  setResult({phase: 'loading', body: '...', ts: t});
  try {
    const s = await service.getStatus();
    setResult({
      phase: 'ok',
      ts: Date.now(),
      body: `uptime ${formatMs(s.uptimeMs)} · mission ${s.missionProfile} · screen ${s.screenEnum} · radio ${s.radioOwner} · xport ${transportName(s.transportKind)}`,
    });
  } catch (err: any) {
    setResult({phase: 'error', body: err?.message ?? String(err), ts: Date.now()});
  }
}

async function runHealth({service, setResult}: RunArgs) {
  const t = Date.now();
  setResult({phase: 'loading', body: '...', ts: t});
  try {
    const h = await service.getHealth();
    setResult({
      phase: 'ok',
      ts: Date.now(),
      body: `batt ${h.batteryPct}% ${h.charging ? 'CHG ' : ''}${h.batteryMv}mV · heap ${formatBytes(h.freeHeap)} (min ${formatBytes(h.minFreeHeap)}) · uptime ${formatMs(h.uptimeMs)}`,
    });
  } catch (err: any) {
    setResult({phase: 'error', body: err?.message ?? String(err), ts: Date.now()});
  }
}

async function runStorage({service, setResult}: RunArgs) {
  const t = Date.now();
  setResult({phase: 'loading', body: '...', ts: t});
  try {
    const s = await service.getStorage();
    setResult({
      phase: 'ok',
      ts: Date.now(),
      body: `${s.usedPct}% used · free ${formatBytes(s.freeBytes)} · mission ${s.missionTotal} · noise ${s.noiseTotal} · upload(m=${s.pendingUploadMission},n=${s.pendingUploadNoise}) · enrich(m=${s.pendingEnrichMission},n=${s.pendingEnrichNoise})`,
    });
  } catch (err: any) {
    setResult({phase: 'error', body: err?.message ?? String(err), ts: Date.now()});
  }
}

async function runWioStatus({service, setResult}: RunArgs) {
  const t = Date.now();
  setResult({phase: 'loading', body: '...', ts: t});
  try {
    const w = await service.getWioStatus();
    setResult({
      phase: 'ok',
      ts: Date.now(),
      body: `active ${transportName(w.transportKind)} (prev ${transportName(w.previousKind)}, age ${formatMs(w.lastChangeAgeMs)}, ${w.transitions} flips) · wio seen ${formatMs(w.wioLastSeenAgeMs)} ago · phone ${w.phoneConnected ? 'on' : 'off'} rssi ${w.phoneRssi} · proxy=${w.bleProxy ? 1 : 0} sx1262=${w.sx1262Present ? 1 : 0}`,
    });
  } catch (err: any) {
    setResult({phase: 'error', body: err?.message ?? String(err), ts: Date.now()});
  }
}

async function runLogTail({service, setResult}: RunArgs) {
  const t = Date.now();
  setResult({phase: 'loading', body: '...', ts: t});
  try {
    const log = await service.getLogTail();
    setResult({
      phase: 'ok',
      ts: Date.now(),
      body: log.lines.length
        ? `${log.lineCount} lines · ${formatBytes(log.totalBytes)}\n${log.lines.join('\n')}`
        : `${log.lineCount} lines · ${formatBytes(log.totalBytes)} (empty)`,
    });
  } catch (err: any) {
    setResult({phase: 'error', body: err?.message ?? String(err), ts: Date.now()});
  }
}

type SafeRunArgs = RunArgs & {
  tag: string;
  screenIndex: number;
  advanceScreen: () => void;
};

async function runEnrich({service, setResult}: SafeRunArgs) {
  const t = Date.now();
  setResult({phase: 'loading', body: '...', ts: t});
  try {
    await service.enrichNow();
    setResult({phase: 'ok', ts: Date.now(), body: 'enrich requested'});
  } catch (err: any) {
    setResult({phase: 'error', body: err?.message ?? String(err), ts: Date.now()});
  }
}

async function runUpload({service, setResult}: SafeRunArgs) {
  const t = Date.now();
  setResult({phase: 'loading', body: '...', ts: t});
  try {
    await service.uploadNow();
    setResult({phase: 'ok', ts: Date.now(), body: 'upload resume requested'});
  } catch (err: any) {
    setResult({phase: 'error', body: err?.message ?? String(err), ts: Date.now()});
  }
}

async function runTag({service, setResult, tag}: SafeRunArgs) {
  const t = Date.now();
  setResult({phase: 'loading', body: '...', ts: t});
  try {
    await service.tagSession(tag);
    setResult({phase: 'ok', ts: Date.now(), body: `tag="${tag}"`});
  } catch (err: any) {
    setResult({phase: 'error', body: err?.message ?? String(err), ts: Date.now()});
  }
}

async function runLocation({service, setResult, tag}: SafeRunArgs) {
  const t = Date.now();
  setResult({phase: 'loading', body: '...', ts: t});
  try {
    await service.saveLocation(tag);
    setResult({phase: 'ok', ts: Date.now(), body: `saved as "${tag}"`});
  } catch (err: any) {
    setResult({phase: 'error', body: err?.message ?? String(err), ts: Date.now()});
  }
}

async function runScreen({service, setResult, screenIndex, advanceScreen}: SafeRunArgs) {
  const target = SAFE_SCREENS[screenIndex];
  const t = Date.now();
  setResult({phase: 'loading', body: `→ ${target.label}`, ts: t});
  try {
    await service.setScreen(target.value);
    advanceScreen();
    setResult({phase: 'ok', ts: Date.now(), body: `now showing ${target.label}`});
  } catch (err: any) {
    setResult({phase: 'error', body: err?.message ?? String(err), ts: Date.now()});
  }
}

async function runDebrief({service, setResult}: SafeRunArgs) {
  const t = Date.now();
  setResult({phase: 'loading', body: '...', ts: t});
  try {
    await service.requestDebrief();
    setResult({phase: 'ok', ts: Date.now(), body: 'debrief view opened on device'});
  } catch (err: any) {
    setResult({phase: 'error', body: err?.message ?? String(err), ts: Date.now()});
  }
}

const SAFE_ACTIONS: Array<{
  key: SafeActionKey;
  label: string;
  opcode: number;
  run: (args: SafeRunArgs) => Promise<void>;
}> = [
  {key: 'enrich',   label: 'Enrich now',     opcode: 0x20, run: runEnrich},
  {key: 'upload',   label: 'Upload resume',  opcode: 0x21, run: runUpload},
  {key: 'tag',      label: 'Tag session',    opcode: 0x22, run: runTag},
  {key: 'location', label: 'Save location',  opcode: 0x23, run: runLocation},
  {key: 'screen',   label: 'Next screen',    opcode: 0x24, run: runScreen},
  {key: 'debrief',  label: 'Open debrief',   opcode: 0x25, run: runDebrief},
];

async function runDashboard({service, setResult}: RunArgs) {
  const t = Date.now();
  setResult({phase: 'loading', body: '...', ts: t});
  try {
    const d = await service.getDashboard();
    const lines = [
      `uptime ${formatMs(d.uptimeMs)} · mission ${d.missionProfile} · screen ${d.screenEnum} · xport ${transportName(d.transportKind)}`,
      `companion ${d.companionEnabled ? 'on' : 'off'}/phone=${d.companionPhone}/work=${d.companionWork} · ble ${d.bleConnected ? 'on' : 'off'}`,
      `wifi ${d.wifiConnected ? 'on' : 'off'} · nets ${d.wifiNetworkCount} · probes ${d.probePacketCount} · pmkid ${d.pmkidCaptured}`,
      `lora ${d.loraReady ? 'on' : 'off'} · nodes ${d.subGhzNodeCount} · rssi ${d.loraRssi}/snr ${d.loraSnr} · pkts ${d.loraPacketCount}`,
      `upload ${d.uploadActive ? 'on' : 'off'} ${d.uploadPercent}% (${d.uploadPublished}/${d.uploadTotal})`,
      `session nets=${d.sessionNetworks} devs=${d.sessionDevices} probes=${d.sessionProbes} pmkid=${d.sessionPMKIDs} drones=${d.sessionDrones}`,
      `drones ${d.droneCount}${d.droneAlert ? ' ⚠' : ''}`,
    ];
    setResult({phase: 'ok', ts: Date.now(), body: lines.join('\n')});
  } catch (err: any) {
    setResult({phase: 'error', body: err?.message ?? String(err), ts: Date.now()});
  }
}

type CommandKey = 'status' | 'health' | 'storage' | 'wio' | 'log' | 'dashboard';
type SafeActionKey = 'enrich' | 'upload' | 'tag' | 'location' | 'screen' | 'debrief';

// Screens the device honors via CMD_OP_SCREEN_CHANGE — matches the whitelist
// in CommandDispatcher::handleScreenChange.  Cycled by the "Screen" button.
const SAFE_SCREENS: Array<{value: number; label: string}> = [
  {value: 0, label: 'LORA'},
  {value: 1, label: 'MESHTASTIC'},
  {value: 2, label: 'WIFI'},
  {value: 5, label: 'RECON'},
  {value: 6, label: 'SYSTEM'},
  {value: 7, label: 'MISSION_SUMMARY'},
];

const COMMANDS: Array<{
  key: CommandKey;
  label: string;
  opcode: number;
  run: (args: RunArgs) => Promise<void>;
}> = [
  {key: 'status', label: 'Status', opcode: CMD_OP_GET_STATUS, run: runStatus},
  {key: 'health', label: 'Health', opcode: CMD_OP_GET_HEALTH, run: runHealth},
  {key: 'storage', label: 'Storage', opcode: CMD_OP_GET_STORAGE, run: runStorage},
  {key: 'wio', label: 'WIO status', opcode: CMD_OP_GET_WIO_STATUS, run: runWioStatus},
  {key: 'log', label: 'Log tail', opcode: CMD_OP_GET_LOG_TAIL, run: runLogTail},
  {key: 'dashboard', label: 'Dashboard', opcode: CMD_OP_GET_DASHBOARD, run: runDashboard},
];

export function DeviceCommandsPanel() {
  const spectre = useSpectre();
  const service = spectre.commandService;

  const [results, setResults] = useState<Record<CommandKey, CommandResult>>(() => ({
    status: INITIAL,
    health: INITIAL,
    storage: INITIAL,
    wio: INITIAL,
    log: INITIAL,
    dashboard: INITIAL,
  }));

  const [safeResults, setSafeResults] = useState<Record<SafeActionKey, CommandResult>>(() => ({
    enrich: INITIAL,
    upload: INITIAL,
    tag: INITIAL,
    location: INITIAL,
    screen: INITIAL,
    debrief: INITIAL,
  }));
  const [screenIndex, setScreenIndex] = useState(0);

  const updateSafeResult = useCallback((key: SafeActionKey, next: CommandResult) => {
    setSafeResults(prev => ({...prev, [key]: next}));
  }, []);
  const advanceScreen = useCallback(() => {
    setScreenIndex(prev => (prev + 1) % SAFE_SCREENS.length);
  }, []);

  const updateResult = useCallback((key: CommandKey, next: CommandResult) => {
    setResults(prev => ({...prev, [key]: next}));
  }, []);

  const channelAvailable = !!service;

  // Log stream state -------------------------------------------------------
  const [streamHandle, setStreamHandle] = useState<LogStreamHandle | null>(null);
  const [streamLines, setStreamLines] = useState<string[]>([]);
  const [streamDropped, setStreamDropped] = useState(0);
  const [streamStatus, setStreamStatus] = useState<string>('idle');
  const [streamBusy, setStreamBusy] = useState(false);
  const handleRef = useRef<LogStreamHandle | null>(null);

  // Keep the ref in sync so cleanup can read the latest handle.
  useEffect(() => {
    handleRef.current = streamHandle;
  }, [streamHandle]);

  useEffect(() => {
    return () => {
      const handle = handleRef.current;
      if (handle) {
        handle.stop().catch(() => undefined);
      }
    };
  }, []);

  const startStream = useCallback(async () => {
    if (!service || streamBusy || streamHandle) return;
    setStreamBusy(true);
    setStreamLines([]);
    setStreamDropped(0);
    setStreamStatus('starting');
    try {
      const handle = await service.startLogStream({
        durationMs: PHONE_LOG_STREAM_DEFAULT_LEASE_MS,
        onChunk: chunk => {
          if (chunk.dropped > 0) {
            setStreamDropped(prev => prev + chunk.dropped);
          }
          if (chunk.lines.length > 0) {
            setStreamLines(prev => {
              const next = prev.concat(chunk.lines);
              return next.length > MAX_STREAM_LINES
                ? next.slice(next.length - MAX_STREAM_LINES)
                : next;
            });
          }
        },
        onEnd: reason => {
          setStreamHandle(null);
          setStreamStatus(`ended (${reason})`);
        },
      });
      setStreamHandle(handle);
      setStreamStatus(`live · lease ${(handle.grantedDurationMs / 1000).toFixed(0)}s`);
    } catch (err: any) {
      setStreamStatus(`error: ${err?.message ?? String(err)}`);
    } finally {
      setStreamBusy(false);
    }
  }, [service, streamBusy, streamHandle]);

  const stopStream = useCallback(async () => {
    const handle = streamHandle;
    if (!handle || streamBusy) return;
    setStreamBusy(true);
    setStreamStatus('stopping');
    try {
      await handle.stop();
    } catch (err: any) {
      setStreamStatus(`stop error: ${err?.message ?? String(err)}`);
    } finally {
      setStreamBusy(false);
    }
  }, [streamHandle, streamBusy]);

  const isStreaming = !!streamHandle;

  // Dashboard stream state -------------------------------------------------
  const [dashHandle, setDashHandle] = useState<DashboardStreamHandle | null>(null);
  const [dashSnapshot, setDashSnapshot] = useState<CmdDashboardSnapshotV1 | null>(null);
  const [dashStatus, setDashStatus] = useState<string>('idle');
  const [dashBusy, setDashBusy] = useState(false);
  const [dashChunkCount, setDashChunkCount] = useState(0);
  const dashHandleRef = useRef<DashboardStreamHandle | null>(null);

  useEffect(() => {
    dashHandleRef.current = dashHandle;
  }, [dashHandle]);

  useEffect(() => {
    return () => {
      const h = dashHandleRef.current;
      if (h) h.stop().catch(() => undefined);
    };
  }, []);

  const startDashStream = useCallback(async () => {
    if (!service || dashBusy || dashHandle) return;
    setDashBusy(true);
    setDashChunkCount(0);
    setDashStatus('starting');
    try {
      const handle = await service.startDashboardStream({
        durationMs: PHONE_DASHBOARD_STREAM_DEFAULT_LEASE_MS,
        intervalMs: PHONE_DASHBOARD_STREAM_INTERVAL_DEFAULT_MS,
        onSnapshot: snapshot => {
          setDashSnapshot(snapshot);
          setDashChunkCount(prev => prev + 1);
        },
        onEnd: reason => {
          setDashHandle(null);
          setDashStatus(`ended (${reason})`);
        },
      });
      setDashHandle(handle);
      setDashStatus(
        `live · ${(handle.grantedIntervalMs / 1000).toFixed(1)}s every ${(handle.grantedDurationMs / 1000).toFixed(0)}s`,
      );
    } catch (err: any) {
      setDashStatus(`error: ${err?.message ?? String(err)}`);
    } finally {
      setDashBusy(false);
    }
  }, [service, dashBusy, dashHandle]);

  const stopDashStream = useCallback(async () => {
    const h = dashHandle;
    if (!h || dashBusy) return;
    setDashBusy(true);
    setDashStatus('stopping');
    try {
      await h.stop();
    } catch (err: any) {
      setDashStatus(`stop error: ${err?.message ?? String(err)}`);
    } finally {
      setDashBusy(false);
    }
  }, [dashHandle, dashBusy]);

  const isDashStreaming = !!dashHandle;

  const notifs = spectre.notifications;

  return (
    <FieldPanel
      title="Device Commands"
      eyebrow="Read-only request/response // command channel"
      tone="accent">
      <Text style={styles.intro}>
        Phone-issued queries over the COMMAND characteristic.  Each tap sends one
        request and resolves on the device's reply.  Read-only; safe to spam.
      </Text>

      {!channelAvailable ? (
        <Text style={styles.warn}>
          Command service unavailable — start the peripheral first.
        </Text>
      ) : null}

      <View style={styles.streamSection}>
        <View style={styles.headerRow}>
          <Text style={styles.opLabel}>
            Notifications{notifs.length > 0 ? ` · ${notifs.length}` : ''}
          </Text>
          <Text style={styles.opcode}>ch 0x0a</Text>
          <Pressable
            style={[styles.runButton, notifs.length === 0 ? styles.runButtonDisabled : null]}
            disabled={notifs.length === 0}
            onPress={() => spectre.clearNotifications()}>
            <Text style={styles.runButtonText}>Clear</Text>
          </Pressable>
        </View>
        {notifs.length === 0 ? (
          <Text style={styles.body}>none received yet</Text>
        ) : (
          <ScrollView style={styles.streamBuffer}>
            {notifs.slice(0, 25).map(n => {
              const severityStyle =
                n.severity === PHONE_NOTIF_SEVERITY_CRITICAL
                  ? styles.bodyError
                  : n.severity === PHONE_NOTIF_SEVERITY_WARN
                    ? styles.bodyOk
                    : null;
              // Device uptime at emit time — phone has no shared wall clock
              // with the device (clock-skew safe across reboots).
              const stamp = `+${(n.deviceUptimeMs / 1000).toFixed(0)}s`;
              const collapsed =
                n.collapsedCount > 0 ? ` ×${n.collapsedCount + 1}` : '';
              return (
                <Text key={`${n.seq}-${n.type}`} style={[styles.streamLine, severityStyle]}>
                  {`#${n.seq} ${stamp} · ${notificationTypeName(n.type)}${collapsed} · ${n.text}`}
                </Text>
              );
            })}
          </ScrollView>
        )}
      </View>

      <View style={styles.streamSection}>
        <View style={styles.headerRow}>
          <Text style={styles.opLabel}>Log stream</Text>
          <Text style={styles.opcode}>0x10 / 0x11</Text>
          <Pressable
            style={[
              styles.runButton,
              (!service || streamBusy) ? styles.runButtonDisabled : null,
              isStreaming ? styles.streamButtonActive : null,
            ]}
            disabled={!service || streamBusy}
            onPress={() => {
              if (isStreaming) {
                void stopStream();
              } else {
                void startStream();
              }
            }}>
            <Text style={styles.runButtonText}>
              {isStreaming ? 'Stop' : 'Start 60s'}
            </Text>
          </Pressable>
        </View>
        <Text style={styles.body}>
          {streamStatus}
          {streamDropped > 0 ? ` · ${streamDropped} dropped` : ''}
          {streamLines.length > 0 ? ` · ${streamLines.length} lines` : ''}
        </Text>
        {streamLines.length > 0 ? (
          <ScrollView style={styles.streamBuffer}>
            {streamLines.map((line, idx) => (
              <Text key={`${idx}-${line.length}`} style={styles.streamLine}>
                {line}
              </Text>
            ))}
          </ScrollView>
        ) : null}
      </View>

      <View style={styles.streamSection}>
        <View style={styles.headerRow}>
          <Text style={styles.opLabel}>Dashboard stream</Text>
          <Text style={styles.opcode}>0x12 / 0x13</Text>
          <Pressable
            style={[
              styles.runButton,
              (!service || dashBusy) ? styles.runButtonDisabled : null,
              isDashStreaming ? styles.streamButtonActive : null,
            ]}
            disabled={!service || dashBusy}
            onPress={() => {
              if (isDashStreaming) {
                void stopDashStream();
              } else {
                void startDashStream();
              }
            }}>
            <Text style={styles.runButtonText}>
              {isDashStreaming ? 'Stop' : 'Start 60s'}
            </Text>
          </Pressable>
        </View>
        <Text style={styles.body}>
          {dashStatus}
          {dashChunkCount > 0 ? ` · ${dashChunkCount} chunks` : ''}
        </Text>
        {dashSnapshot ? (
          <Text style={[styles.body, styles.bodyOk]}>
            {`uptime ${formatMs(dashSnapshot.uptimeMs)} · ble ${dashSnapshot.bleConnected ? 'on' : 'off'} · wifi ${dashSnapshot.wifiConnected ? 'on' : 'off'} · lora ${dashSnapshot.loraReady ? 'on' : 'off'} (${dashSnapshot.subGhzNodeCount} nodes)\n`}
            {`upload ${dashSnapshot.uploadActive ? `${dashSnapshot.uploadPercent}%` : 'idle'} · session probes=${dashSnapshot.sessionProbes} pmkid=${dashSnapshot.sessionPMKIDs} drones=${dashSnapshot.sessionDrones}`}
          </Text>
        ) : null}
      </View>

      <Text style={styles.intro}>Safe actions — phone-issued writes.</Text>
      {SAFE_ACTIONS.map(action => {
        const result = safeResults[action.key];
        const isLoading = result.phase === 'loading';
        const nextScreenLabel =
          action.key === 'screen' ? ` → ${SAFE_SCREENS[screenIndex].label}` : '';
        return (
          <View key={action.key} style={styles.row}>
            <View style={styles.headerRow}>
              <Text style={styles.opLabel}>{action.label}{nextScreenLabel}</Text>
              <Text style={styles.opcode}>
                0x{action.opcode.toString(16).padStart(2, '0')}
              </Text>
              <Pressable
                style={[
                  styles.runButton,
                  (!service || isLoading) ? styles.runButtonDisabled : null,
                ]}
                disabled={!service || isLoading}
                onPress={() => {
                  if (!service) return;
                  void action.run({
                    service,
                    setResult: r => updateSafeResult(action.key, r),
                    tag: spectre.activeTag || 'FIELD',
                    screenIndex,
                    advanceScreen,
                  });
                }}>
                <Text style={styles.runButtonText}>
                  {isLoading ? '...' : 'Run'}
                </Text>
              </Pressable>
            </View>
            <Text
              style={[
                styles.body,
                result.phase === 'error' ? styles.bodyError : null,
                result.phase === 'ok' ? styles.bodyOk : null,
              ]}>
              {result.phase === 'idle' ? '—' : result.body}
            </Text>
          </View>
        );
      })}

      {COMMANDS.map(cmd => {
        const result = results[cmd.key];
        const isLoading = result.phase === 'loading';
        return (
          <View key={cmd.key} style={styles.row}>
            <View style={styles.headerRow}>
              <Text style={styles.opLabel}>{cmd.label}</Text>
              <Text style={styles.opcode}>
                0x{cmd.opcode.toString(16).padStart(2, '0')}
              </Text>
              <Pressable
                style={[
                  styles.runButton,
                  (!service || isLoading) ? styles.runButtonDisabled : null,
                ]}
                disabled={!service || isLoading}
                onPress={() => {
                  if (!service) return;
                  void cmd.run({service, setResult: r => updateResult(cmd.key, r)});
                }}>
                <Text style={styles.runButtonText}>
                  {isLoading ? '...' : 'Run'}
                </Text>
              </Pressable>
            </View>
            <Text
              style={[
                styles.body,
                result.phase === 'error' ? styles.bodyError : null,
                result.phase === 'ok' ? styles.bodyOk : null,
              ]}>
              {result.phase === 'idle' ? '—' : result.body}
            </Text>
          </View>
        );
      })}
    </FieldPanel>
  );
}

const styles = StyleSheet.create({
  intro: {
    color: theme.colors.textSoft,
    fontSize: 11,
    fontFamily: theme.fonts.body,
    marginBottom: theme.spacing.xs,
  },
  warn: {
    color: theme.colors.amber,
    fontSize: 11,
    fontFamily: theme.fonts.label,
    marginBottom: theme.spacing.xs,
  },
  row: {
    paddingVertical: theme.spacing.xs,
    borderBottomColor: theme.colors.panelEdge,
    borderBottomWidth: 1,
    gap: 4,
  },
  headerRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: theme.spacing.sm,
  },
  opLabel: {
    color: theme.colors.textStrong,
    fontSize: 12,
    fontFamily: theme.fonts.label,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.6,
    flex: 1,
  },
  opcode: {
    color: theme.colors.textDim,
    fontSize: 10,
    fontFamily: theme.fonts.label,
    letterSpacing: 0.8,
  },
  runButton: {
    paddingVertical: 6,
    paddingHorizontal: 14,
    borderRadius: theme.radius.pill,
    backgroundColor: theme.colors.cyanDim,
    borderColor: theme.colors.cyan,
    borderWidth: 1,
  },
  runButtonDisabled: {
    opacity: 0.45,
  },
  runButtonText: {
    color: theme.colors.textOnAccent,
    fontSize: 11,
    fontFamily: theme.fonts.label,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.6,
  },
  body: {
    color: theme.colors.textSoft,
    fontSize: 11,
    fontFamily: theme.fonts.body,
    lineHeight: 14,
  },
  bodyOk: {
    color: theme.colors.textStrong,
  },
  bodyError: {
    color: theme.colors.amber,
  },
  streamSection: {
    paddingVertical: theme.spacing.xs,
    borderBottomColor: theme.colors.panelEdge,
    borderBottomWidth: 1,
    gap: 4,
    marginBottom: theme.spacing.xs,
  },
  streamButtonActive: {
    backgroundColor: theme.colors.amber,
    borderColor: theme.colors.amber,
  },
  streamBuffer: {
    maxHeight: 180,
    backgroundColor: theme.colors.bgAlt,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.sm,
    paddingHorizontal: theme.spacing.xs,
    paddingVertical: 4,
  },
  streamLine: {
    color: theme.colors.textSoft,
    fontSize: 10,
    lineHeight: 13,
    fontFamily: theme.fonts.body,
  },
});
