import React, {useCallback, useEffect, useRef, useState} from 'react';
import {
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';

import {FieldPanel} from './FieldPanel';
import {PHONE_LOG_STREAM_DEFAULT_LEASE_MS} from '../protocol/contracts';
import type {LogStreamHandle} from '../services/peripheral/SpectreCommandService';
import {useSpectre} from '../state/SpectreContext';
import {theme} from '../theme/theme';

// Interactive console over the existing structured command channel.  No
// arbitrary device-console exec here — every typed verb maps to an opcode the
// firmware already exposes via CommandDispatcher.  Output (command echoes,
// results, and the live log stream) all land in one scrolling buffer.

const MAX_CONSOLE_LINES = 500;
const MAX_HISTORY = 30;

type LineKind = 'cmd' | 'out' | 'err' | 'log' | 'sys';

type ConsoleLine = {
  id: string;
  kind: LineKind;
  text: string;
};

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

// Screens the device honors via CMD_OP_SCREEN_CHANGE — matches the whitelist
// in CommandDispatcher::handleScreenChange and DeviceCommandsPanel.
const SCREEN_NAMES: Record<string, number> = {
  lora: 0,
  meshtastic: 1,
  wifi: 2,
  recon: 5,
  system: 6,
  mission_summary: 7,
  summary: 7,
  ble: 8,
  phone: 8,
};

const HELP_LINES = [
  'commands:',
  '  status                 device status snapshot',
  '  health                 battery / heap / uptime',
  '  storage                vault usage + pending queues',
  '  wio                    transport / WIO accessory state',
  '  dashboard | dash       full dashboard snapshot',
  '  logtail                last buffered log lines (one-shot)',
  '  log [start|stop]       toggle the live log stream',
  '  enrich                 request enrich-now',
  '  upload                 resume upload',
  '  tag <text>             tag the active session',
  '  saveloc <text>         save a named location',
  '  screen <name|index>    change device screen',
  '  debrief                open debrief view on device',
  '  clear                  clear this console',
  '  help | ?               show this list',
];

let lineSeq = 0;
function nextLineId() {
  lineSeq += 1;
  return `cl-${Date.now().toString(36)}-${lineSeq}`;
}

export function DeviceConsolePanel() {
  const spectre = useSpectre();
  const service = spectre.commandService;
  const channelAvailable = !!service;

  const [lines, setLines] = useState<ConsoleLine[]>([
    {id: nextLineId(), kind: 'sys', text: 'Spectre console · type "help" for commands'},
  ]);
  const [input, setInput] = useState('');
  const [busy, setBusy] = useState(false);
  const [linking, setLinking] = useState(false);
  const [history, setHistory] = useState<string[]>([]);

  const scrollRef = useRef<ScrollView | null>(null);
  const logHandleRef = useRef<LogStreamHandle | null>(null);
  const [logStreaming, setLogStreaming] = useState(false);

  const append = useCallback((kind: LineKind, text: string) => {
    setLines(prev => {
      // A single result may be multi-line; split so each row styles cleanly.
      const incoming = text.split('\n').map(t => ({id: nextLineId(), kind, text: t}));
      const next = prev.concat(incoming);
      return next.length > MAX_CONSOLE_LINES
        ? next.slice(next.length - MAX_CONSOLE_LINES)
        : next;
    });
  }, []);

  // Tear the log stream down if the panel unmounts mid-stream.
  useEffect(() => {
    return () => {
      logHandleRef.current?.stop().catch(() => undefined);
    };
  }, []);

  const startLogStream = useCallback(async () => {
    if (!service || logHandleRef.current) {
      return;
    }
    append('sys', '> log stream starting…');
    try {
      const handle = await service.startLogStream({
        durationMs: PHONE_LOG_STREAM_DEFAULT_LEASE_MS,
        onChunk: chunk => {
          if (chunk.dropped > 0) {
            append('sys', `… ${chunk.dropped} log lines dropped (device buffer)`);
          }
          chunk.lines.forEach(l => append('log', l));
        },
        onEnd: reason => {
          logHandleRef.current = null;
          setLogStreaming(false);
          append('sys', `> log stream ended (${reason})`);
        },
      });
      logHandleRef.current = handle;
      setLogStreaming(true);
      append('sys', `> log stream live · lease ${(handle.grantedDurationMs / 1000).toFixed(0)}s`);
    } catch (err: any) {
      append('err', `log stream error: ${err?.message ?? String(err)}`);
    }
  }, [service, append]);

  const stopLogStream = useCallback(async () => {
    const handle = logHandleRef.current;
    if (!handle) {
      return;
    }
    append('sys', '> log stream stopping…');
    try {
      await handle.stop();
    } catch (err: any) {
      append('err', `log stop error: ${err?.message ?? String(err)}`);
    }
  }, [append]);

  const runCommand = useCallback(
    async (raw: string) => {
      if (!service) {
        append('err', 'command channel unavailable — start Field Mode first');
        return;
      }
      const trimmed = raw.trim();
      if (!trimmed) {
        return;
      }
      const [verb, ...rest] = trimmed.split(/\s+/);
      const arg = rest.join(' ');
      const cmd = verb.toLowerCase();

      // Local-only verbs that never hit the device.
      if (cmd === 'help' || cmd === '?') {
        HELP_LINES.forEach(l => append('out', l));
        return;
      }
      if (cmd === 'clear' || cmd === 'cls') {
        setLines([]);
        return;
      }
      if (cmd === 'log' || cmd === 'logstream') {
        const mode = arg.trim().toLowerCase();
        if (mode === 'stop') {
          await stopLogStream();
        } else if (mode === 'start' || mode === '') {
          if (logHandleRef.current) {
            append('sys', '> log stream already live');
          } else {
            await startLogStream();
          }
        } else {
          append('err', `usage: log [start|stop]`);
        }
        return;
      }

      setBusy(true);
      try {
        switch (cmd) {
          case 'status': {
            const s = await service.getStatus();
            append(
              'out',
              `uptime ${formatMs(s.uptimeMs)} · mission ${s.missionProfile} · screen ${s.screenEnum} · radio ${s.radioOwner} · xport ${transportName(s.transportKind)}`,
            );
            break;
          }
          case 'health': {
            const h = await service.getHealth();
            append(
              'out',
              `batt ${h.batteryPct}% ${h.charging ? 'CHG ' : ''}${h.batteryMv}mV · heap ${formatBytes(h.freeHeap)} (min ${formatBytes(h.minFreeHeap)}) · uptime ${formatMs(h.uptimeMs)}`,
            );
            break;
          }
          case 'storage': {
            const s = await service.getStorage();
            append(
              'out',
              `${s.usedPct}% used · free ${formatBytes(s.freeBytes)} · mission ${s.missionTotal} · noise ${s.noiseTotal} · upload(m=${s.pendingUploadMission},n=${s.pendingUploadNoise}) · enrich(m=${s.pendingEnrichMission},n=${s.pendingEnrichNoise})`,
            );
            break;
          }
          case 'wio': {
            const w = await service.getWioStatus();
            append(
              'out',
              `active ${transportName(w.transportKind)} (prev ${transportName(w.previousKind)}, ${w.transitions} flips) · wio seen ${formatMs(w.wioLastSeenAgeMs)} ago · phone ${w.phoneConnected ? 'on' : 'off'} rssi ${w.phoneRssi}`,
            );
            break;
          }
          case 'logtail': {
            const log = await service.getLogTail();
            append('out', `${log.lineCount} lines · ${formatBytes(log.totalBytes)}`);
            log.lines.forEach(l => append('out', l));
            break;
          }
          case 'dashboard':
          case 'dash': {
            const d = await service.getDashboard();
            append('out', `uptime ${formatMs(d.uptimeMs)} · mission ${d.missionProfile} · screen ${d.screenEnum} · xport ${transportName(d.transportKind)}`);
            append('out', `ble ${d.bleConnected ? 'on' : 'off'} · wifi ${d.wifiConnected ? 'on' : 'off'} nets ${d.wifiNetworkCount} · lora ${d.loraReady ? 'on' : 'off'} (${d.subGhzNodeCount} nodes)`);
            append('out', `upload ${d.uploadActive ? `${d.uploadPercent}%` : 'idle'} (${d.uploadPublished}/${d.uploadTotal}) · session nets=${d.sessionNetworks} devs=${d.sessionDevices} probes=${d.sessionProbes} pmkid=${d.sessionPMKIDs} drones=${d.sessionDrones}`);
            break;
          }
          case 'enrich': {
            await service.enrichNow();
            append('out', 'enrich requested');
            break;
          }
          case 'upload': {
            await service.uploadNow();
            append('out', 'upload resume requested');
            break;
          }
          case 'tag': {
            if (!arg) {
              append('err', 'usage: tag <text>');
              break;
            }
            await service.tagSession(arg);
            append('out', `tag="${arg}"`);
            break;
          }
          case 'saveloc':
          case 'savelocation':
          case 'loc': {
            if (!arg) {
              append('err', 'usage: saveloc <text>');
              break;
            }
            await service.saveLocation(arg);
            append('out', `saved location "${arg}"`);
            break;
          }
          case 'screen': {
            const key = arg.trim().toLowerCase();
            const target = /^\d+$/.test(key) ? Number(key) : SCREEN_NAMES[key];
            if (target === undefined) {
              append('err', `unknown screen "${arg}" · try: ${Object.keys(SCREEN_NAMES).join(', ')}`);
              break;
            }
            await service.setScreen(target);
            append('out', `screen → ${target}`);
            break;
          }
          case 'debrief': {
            await service.requestDebrief();
            append('out', 'debrief view opened on device');
            break;
          }
          default:
            append('err', `unknown command "${verb}" · type "help"`);
        }
      } catch (err: any) {
        append('err', err?.message ?? String(err));
      } finally {
        setBusy(false);
      }
    },
    [service, append, startLogStream, stopLogStream],
  );

  const submit = useCallback(() => {
    const raw = input;
    if (!raw.trim() || busy) {
      return;
    }
    append('cmd', `$ ${raw.trim()}`);
    setHistory(prev => {
      const next = [raw.trim(), ...prev.filter(h => h !== raw.trim())];
      return next.slice(0, MAX_HISTORY);
    });
    setInput('');
    void runCommand(raw);
  }, [input, busy, append, runCommand]);

  const lineStyle = (kind: LineKind) => {
    switch (kind) {
      case 'cmd':
        return styles.lineCmd;
      case 'err':
        return styles.lineErr;
      case 'out':
        return styles.lineOut;
      case 'log':
        return styles.lineLog;
      case 'sys':
      default:
        return styles.lineSys;
    }
  };

  return (
    <FieldPanel
      title="Device Console"
      eyebrow="Interactive // command channel + log stream"
      tone="accent"
      action={
        <View style={[styles.pill, logStreaming ? styles.pillLive : styles.pillIdle]}>
          <Text style={styles.pillText}>{logStreaming ? 'log live' : 'idle'}</Text>
        </View>
      }>
      {!channelAvailable ? (
        <View style={styles.linkPrompt}>
          <Text style={styles.warn}>
            Command channel needs the BLE link. Power it on to open the console —
            you&apos;re plugged in, so battery isn&apos;t a concern here.
          </Text>
          <Pressable
            style={[styles.linkButton, linking ? styles.sendDisabled : null]}
            disabled={linking}
            onPress={() => {
              setLinking(true);
              append('sys', '> powering on BLE link…');
              spectre
                .ensureBleLink()
                .catch((err: any) =>
                  append('err', `link error: ${err?.message ?? String(err)}`),
                )
                .finally(() => setLinking(false));
            }}>
            <Text style={styles.linkButtonText}>
              {linking ? 'Linking…' : 'Power On Link'}
            </Text>
          </Pressable>
        </View>
      ) : null}

      <ScrollView
        ref={scrollRef}
        style={styles.buffer}
        onContentSizeChange={() => scrollRef.current?.scrollToEnd({animated: true})}>
        {lines.map(line => (
          <Text key={line.id} style={[styles.line, lineStyle(line.kind)]}>
            {line.text || ' '}
          </Text>
        ))}
      </ScrollView>

      <View style={styles.inputRow}>
        <Text style={styles.prompt}>$</Text>
        <TextInput
          value={input}
          onChangeText={setInput}
          onSubmitEditing={submit}
          editable={channelAvailable && !busy}
          style={styles.input}
          placeholder={busy ? 'running…' : 'type a command — help'}
          placeholderTextColor={theme.colors.textDim}
          autoCapitalize="none"
          autoCorrect={false}
          returnKeyType="send"
          blurOnSubmit={false}
        />
        <Pressable
          style={[
            styles.sendButton,
            !channelAvailable || busy || !input.trim() ? styles.sendDisabled : null,
          ]}
          disabled={!channelAvailable || busy || !input.trim()}
          onPress={submit}>
          <Text style={styles.sendText}>Send</Text>
        </Pressable>
      </View>

      <View style={styles.chipRow}>
        {['status', 'health', 'dashboard', logStreaming ? 'log stop' : 'log start', 'clear', 'help'].map(
          chip => (
            <Pressable
              key={chip}
              style={[styles.chip, !channelAvailable ? styles.chipDisabled : null]}
              disabled={!channelAvailable && chip !== 'clear' && chip !== 'help'}
              onPress={() => {
                append('cmd', `$ ${chip}`);
                void runCommand(chip);
              }}>
              <Text style={styles.chipText}>{chip}</Text>
            </Pressable>
          ),
        )}
      </View>

      {history.length > 0 ? (
        <View style={styles.historyRow}>
          {history.slice(0, 6).map(h => (
            <Pressable key={h} style={styles.histChip} onPress={() => setInput(h)}>
              <Text style={styles.histText} numberOfLines={1}>
                {h}
              </Text>
            </Pressable>
          ))}
        </View>
      ) : null}
    </FieldPanel>
  );
}

const styles = StyleSheet.create({
  warn: {
    color: theme.colors.amber,
    fontSize: 11,
    fontFamily: theme.fonts.label,
    marginBottom: theme.spacing.xs,
  },
  linkPrompt: {
    gap: theme.spacing.xs,
    marginBottom: theme.spacing.xs,
  },
  linkButton: {
    alignSelf: 'flex-start',
    paddingVertical: 8,
    paddingHorizontal: 16,
    borderRadius: theme.radius.pill,
    backgroundColor: theme.colors.cyanDim,
    borderColor: theme.colors.cyan,
    borderWidth: 1,
  },
  linkButtonText: {
    color: theme.colors.textOnAccent,
    fontSize: 11,
    fontFamily: theme.fonts.label,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.6,
  },
  buffer: {
    height: 260,
    backgroundColor: theme.colors.bgAlt,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.sm,
    paddingHorizontal: theme.spacing.xs,
    paddingVertical: 4,
  },
  line: {
    fontSize: 11,
    lineHeight: 15,
    fontFamily: theme.fonts.body,
  },
  lineCmd: {
    color: theme.colors.cyan,
    fontWeight: '700',
  },
  lineOut: {
    color: theme.colors.textStrong,
  },
  lineErr: {
    color: theme.colors.amber,
  },
  lineLog: {
    color: theme.colors.textSoft,
  },
  lineSys: {
    color: theme.colors.textDim,
  },
  inputRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: theme.spacing.xs,
    marginTop: theme.spacing.xs,
  },
  prompt: {
    color: theme.colors.cyan,
    fontSize: 14,
    fontWeight: '700',
    fontFamily: theme.fonts.body,
  },
  input: {
    flex: 1,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    backgroundColor: theme.colors.bgAlt,
    color: theme.colors.textStrong,
    paddingHorizontal: theme.spacing.sm,
    paddingVertical: 8,
    fontFamily: theme.fonts.body,
    fontSize: 13,
  },
  sendButton: {
    paddingVertical: 8,
    paddingHorizontal: 16,
    borderRadius: theme.radius.pill,
    backgroundColor: theme.colors.cyanDim,
    borderColor: theme.colors.cyan,
    borderWidth: 1,
  },
  sendDisabled: {
    opacity: 0.45,
  },
  sendText: {
    color: theme.colors.textOnAccent,
    fontSize: 11,
    fontFamily: theme.fonts.label,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.6,
  },
  chipRow: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 6,
    marginTop: theme.spacing.xs,
  },
  chip: {
    paddingVertical: 5,
    paddingHorizontal: 10,
    borderRadius: theme.radius.pill,
    backgroundColor: theme.colors.bg,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
  },
  chipDisabled: {
    opacity: 0.5,
  },
  chipText: {
    color: theme.colors.textSoft,
    fontSize: 10,
    fontFamily: theme.fonts.label,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.6,
  },
  historyRow: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 6,
    marginTop: 6,
  },
  histChip: {
    paddingVertical: 4,
    paddingHorizontal: 8,
    borderRadius: theme.radius.sm,
    backgroundColor: theme.colors.bgAlt,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    maxWidth: 140,
  },
  histText: {
    color: theme.colors.textDim,
    fontSize: 10,
    fontFamily: theme.fonts.body,
  },
  pill: {
    borderRadius: theme.radius.pill,
    paddingHorizontal: 10,
    paddingVertical: 6,
    borderWidth: 1,
  },
  pillIdle: {
    backgroundColor: theme.colors.bgAlt,
    borderColor: theme.colors.panelEdge,
  },
  pillLive: {
    backgroundColor: theme.colors.limeDim,
    borderColor: theme.colors.lime,
  },
  pillText: {
    color: theme.colors.textStrong,
    fontSize: 10,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 1,
    fontFamily: theme.fonts.label,
  },
});
