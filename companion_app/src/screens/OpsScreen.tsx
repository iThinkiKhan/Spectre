import React, {useMemo, useState} from 'react';
import {
  Pressable,
  ScrollView,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  View,
} from 'react-native';

import {FieldPanel} from '../components/FieldPanel';
import {
  normalizeBadUsbScriptDraft,
  type BadUsbScriptDraft,
  type BadUsbUploadPhase,
} from '../protocol/badusb';
import {
  spectreFileImportBridge,
  type ImportedBadUsbFile,
} from '../services/files/SpectreFileImportBridge';
import {useSpectre} from '../state/SpectreContext';
import {theme} from '../theme/theme';

const starterDraft: BadUsbScriptDraft = {
  fileName: 'hello_spectre',
  displayName: 'Hello Spectre',
  description: 'Quick terminal smoke test',
  body: [
    'REM Spectre field check',
    'DELAY 800',
    'GUI r',
    'DELAY 300',
    'STRING notepad',
    'ENTER',
    'DELAY 500',
    'STRING Spectre vault link live.',
    'ENTER',
  ].join('\n'),
};

function uploadTone(phase: BadUsbUploadPhase) {
  switch (phase) {
    case 'ready':
    case 'uploading':
    case 'committing':
      return 'accent' as const;
    case 'complete':
      return 'success' as const;
    case 'error':
      return 'danger' as const;
    case 'cancelled':
      return 'warn' as const;
    default:
      return 'default' as const;
  }
}

function uploadLabel(phase: BadUsbUploadPhase) {
  switch (phase) {
    case 'ready':
      return 'staged';
    case 'uploading':
      return 'uplink';
    case 'committing':
      return 'commit';
    case 'complete':
      return 'stored';
    case 'error':
      return 'fault';
    case 'cancelled':
      return 'cancel';
    default:
      return 'idle';
  }
}

function phaseStyleForUpload(phase: BadUsbUploadPhase) {
  switch (phase) {
    case 'ready':
    case 'uploading':
      return styles.phaseAccent;
    case 'committing':
    case 'cancelled':
      return styles.phaseWarn;
    case 'complete':
      return styles.phaseSuccess;
    case 'error':
      return styles.phaseDanger;
    case 'idle':
    default:
      return styles.phaseIdle;
  }
}

function formatProgress(current: number, total: number) {
  if (total <= 0) {
    return '--';
  }
  return `${current}/${total}`;
}

function formatRelativeTime(timestamp?: number | null) {
  if (!timestamp) {
    return 'never';
  }

  const elapsedSeconds = Math.max(0, Math.floor((Date.now() - timestamp) / 1000));
  if (elapsedSeconds < 60) {
    return `${elapsedSeconds}s ago`;
  }

  const elapsedMinutes = Math.floor(elapsedSeconds / 60);
  if (elapsedMinutes < 60) {
    return `${elapsedMinutes}m ago`;
  }

  const elapsedHours = Math.floor(elapsedMinutes / 60);
  if (elapsedHours < 24) {
    return `${elapsedHours}h ago`;
  }

  return `${Math.floor(elapsedHours / 24)}d ago`;
}

function formatBytes(value?: number | null) {
  if (!value) {
    return '0B';
  }
  if (value < 1024) {
    return `${value}B`;
  }
  return `${(value / 1024).toFixed(1)}KB`;
}

function formatCount(value?: number | null) {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    return '--';
  }
  return String(Math.round(value));
}

function progressFraction(current: number, total: number) {
  if (total <= 0) {
    return 0;
  }
  return Math.max(0, Math.min(1, current / total));
}

function PeripheralMetric({
  label,
  value,
  detail,
}: {
  label: string;
  value: string;
  detail: string;
}) {
  return (
    <View style={styles.peripheralMetric}>
      <Text style={styles.peripheralLabel}>{label}</Text>
      <Text numberOfLines={1} style={styles.peripheralValue}>
        {value}
      </Text>
      <Text numberOfLines={1} style={styles.peripheralDetail}>
        {detail}
      </Text>
    </View>
  );
}

function stemFromImportedFileName(fileName: string) {
  const trimmed = fileName.trim();
  if (!trimmed.length) {
    return 'payload';
  }

  return trimmed.replace(/\.[^.]+$/g, '') || 'payload';
}

function displayNameFromStem(stem: string) {
  return stem.replace(/[_-]+/g, ' ').trim() || 'Imported Payload';
}

export function OpsScreen() {
  const spectre = useSpectre();
  const [draft, setDraft] = useState<BadUsbScriptDraft>(starterDraft);
  const [importSource, setImportSource] = useState<ImportedBadUsbFile | null>(null);
  const [importBusy, setImportBusy] = useState(false);
  const [importError, setImportError] = useState<string | null>(null);

  let normalizedDraft:
    | ReturnType<typeof normalizeBadUsbScriptDraft>
    | null = null;
  let validationError: string | null = null;

  try {
    normalizedDraft = normalizeBadUsbScriptDraft(draft);
  } catch (error: any) {
    validationError = error?.message || 'BadUSB draft is invalid.';
  }

  const uploadState = spectre.badUsbUploadState;
  const uploadActive =
    uploadState.phase === 'uploading' || uploadState.phase === 'committing';
  const uploadSupported = !!spectre.connectedDevice && uploadState.available;
  const peripheral = spectre.peripheralState;
  const companionTone = peripheral.error
    ? 'danger'
    : peripheral.advertising
      ? 'success'
      : peripheral.running
        ? 'warn'
        : 'default';
  const lastTransferRecords =
    spectre.lastPublishedBatch?.records ?? peripheral.lastBatchRecords ?? 0;
  const lastTransferBytes =
    spectre.lastPublishedBatch?.bytes ?? peripheral.lastBatchBytes ?? 0;
  const lastTransferAt =
    spectre.lastPublishedBatch?.sentAt ?? peripheral.lastBatchReceivedAt;
  const storage = spectre.storageSnapshot;
  const pendingUpload =
    (storage?.pendingUploadMission ?? 0) + (storage?.pendingUploadNoise ?? 0);
  const pendingEnrich =
    (storage?.pendingEnrichMission ?? 0) + (storage?.pendingEnrichNoise ?? 0);
  const targetPath = normalizedDraft
    ? `/config/vault/badusb/${normalizedDraft.fileName}`
    : '/config/vault/badusb';

  const vaultProgress = useMemo(
    () =>
      progressFraction(
        uploadState.totalBytes > 0
          ? uploadState.receivedBytes
          : uploadState.receivedChunks,
        uploadState.totalBytes > 0
          ? uploadState.totalBytes
          : uploadState.totalChunks,
      ),
    [
      uploadState.receivedBytes,
      uploadState.totalBytes,
      uploadState.receivedChunks,
      uploadState.totalChunks,
    ],
  );

  const handleImportFile = async () => {
    if (!spectreFileImportBridge.isAvailable()) {
      setImportError('Android file import is unavailable on this build.');
      return;
    }

    try {
      setImportBusy(true);
      setImportError(null);
      const imported = await spectreFileImportBridge.pickTextFile();
      if (!imported) {
        return;
      }

      const stem = stemFromImportedFileName(imported.name || 'payload.txt');
      const displayName = displayNameFromStem(stem);

      setDraft({
        fileName: stem,
        displayName,
        description: `Imported from ${imported.name || 'local file'}`,
        body: imported.text,
      });
      setImportSource(imported);
    } catch (error: any) {
      setImportError(error?.message || 'Failed to import file.');
    } finally {
      setImportBusy(false);
    }
  };

  return (
    <ScrollView
      style={styles.scroll}
      contentContainerStyle={styles.content}
      showsVerticalScrollIndicator={false}>
      <FieldPanel
        title="Vault Uplink"
        eyebrow="BadUSB // target /config/vault/badusb"
        tone={uploadTone(uploadState.phase)}
        action={
          <View
            style={[
              styles.phasePill,
              phaseStyleForUpload(uploadState.phase),
            ]}>
            <Text style={styles.phasePillText}>{uploadLabel(uploadState.phase)}</Text>
          </View>
        }>
        <Text style={styles.bodyText}>
          The phone acts as a field-side BadUSB uplink. Type here or pull in a
          local text file, validate to the live contract, then push straight
          into Spectre&apos;s vault without disturbing the prompt channel.
        </Text>

        <View style={styles.summaryRow}>
          <View style={styles.summaryChipWide}>
            <Text style={styles.summaryLabel}>Destination</Text>
            <Text numberOfLines={1} style={styles.summaryValue}>
              {targetPath}
            </Text>
          </View>
          <View style={styles.summaryChip}>
            <Text style={styles.summaryLabel}>Chunks</Text>
            <Text style={styles.summaryValue}>
              {formatProgress(uploadState.receivedChunks, uploadState.totalChunks)}
            </Text>
          </View>
          <View style={styles.summaryChip}>
            <Text style={styles.summaryLabel}>Bytes</Text>
            <Text style={styles.summaryValue}>
              {formatProgress(uploadState.receivedBytes, uploadState.totalBytes)}
            </Text>
          </View>
        </View>

        <View style={styles.progressTrack}>
          <View
            style={[
              styles.progressFill,
              {width: `${Math.round(vaultProgress * 100)}%`},
            ]}
          />
        </View>

        {!!uploadState.message && (
          <Text
            style={[
              styles.uploadMessage,
              uploadState.phase === 'error' ? styles.errorText : null,
            ]}>
            {uploadState.message}
          </Text>
        )}

        {importSource ? (
          <Text style={styles.importMeta}>
            Loaded file: {importSource.name} · {Math.round(importSource.size)} bytes
          </Text>
        ) : null}

        {!spectre.connectedDevice ? (
          <Text style={styles.warnText}>
            Connect the text link first. The vault uplink rides the same Spectre
            service but on a separate upload contract.
          </Text>
        ) : !uploadState.available ? (
          <Text style={styles.warnText}>
            Connected firmware is not exposing the BadUSB vault endpoint yet.
          </Text>
        ) : null}

        <View style={styles.metaGrid}>
          <View style={styles.metaField}>
            <Text style={styles.inputLabel}>File stem</Text>
            <TextInput
              value={draft.fileName}
              onChangeText={fileName =>
                setDraft(previous => ({...previous, fileName}))
              }
              style={styles.input}
              placeholder="hello_spectre"
              placeholderTextColor={theme.colors.textDim}
              autoCapitalize="none"
              autoCorrect={false}
            />
          </View>
          <View style={styles.metaField}>
            <Text style={styles.inputLabel}>Display name</Text>
            <TextInput
              value={draft.displayName}
              onChangeText={displayName =>
                setDraft(previous => ({...previous, displayName}))
              }
              style={styles.input}
              placeholder="Hello Spectre"
              placeholderTextColor={theme.colors.textDim}
              autoCapitalize="words"
              autoCorrect={false}
            />
          </View>
        </View>

        <View style={styles.metaField}>
          <Text style={styles.inputLabel}>Description</Text>
          <TextInput
            value={draft.description}
            onChangeText={description =>
              setDraft(previous => ({...previous, description}))
            }
            style={styles.input}
            placeholder="Quick terminal smoke test"
            placeholderTextColor={theme.colors.textDim}
            autoCapitalize="sentences"
            autoCorrect={false}
          />
        </View>

        <View style={styles.editorHeader}>
          <Text style={styles.inputLabel}>Script body</Text>
          {normalizedDraft ? (
            <Text style={styles.editorMeta}>
              {normalizedDraft.lineCount} lines · {normalizedDraft.totalBytes} bytes
            </Text>
          ) : null}
        </View>

        <TextInput
          value={draft.body}
          onChangeText={body => setDraft(previous => ({...previous, body}))}
          style={styles.editor}
          placeholder="REM Spectre payload"
          placeholderTextColor={theme.colors.textDim}
          multiline
          textAlignVertical="top"
          autoCapitalize="none"
          autoCorrect={false}
        />

        {validationError ? (
          <Text style={styles.errorText}>{validationError}</Text>
        ) : normalizedDraft ? (
          <View style={styles.metricsRow}>
            <View style={styles.metricBox}>
              <Text style={styles.metricLabel}>Normalized file</Text>
              <Text numberOfLines={1} style={styles.metricValue}>
                {normalizedDraft.fileName}
              </Text>
            </View>
            <View style={styles.metricBox}>
              <Text style={styles.metricLabel}>Chunk budget</Text>
              <Text style={styles.metricValue}>
                {normalizedDraft.totalChunks} x {normalizedDraft.chunkSize}B
              </Text>
            </View>
          </View>
        ) : null}

        {!!importError && <Text style={styles.errorText}>{importError}</Text>}

        {normalizedDraft?.warnings.length ? (
          <View style={styles.warningBox}>
            {normalizedDraft.warnings.map(warning => (
              <Text key={warning} style={styles.warnText}>
                {warning}
              </Text>
            ))}
          </View>
        ) : null}

        <View style={styles.buttonRow}>
          <Pressable
            style={[
              styles.secondaryButton,
              importBusy || uploadActive ? styles.buttonDisabled : null,
            ]}
            disabled={importBusy || uploadActive}
            onPress={() => {
              handleImportFile().catch(() => {});
            }}>
            <Text style={styles.secondaryText}>
              {importBusy ? 'Loading File...' : 'Import File'}
            </Text>
          </Pressable>

          <Pressable
            style={[
              styles.primaryButton,
              !uploadSupported || !!validationError || uploadActive
                ? styles.buttonDisabled
                : null,
            ]}
            disabled={!uploadSupported || !!validationError || uploadActive}
            onPress={() => {
              if (!normalizedDraft) {
                return;
              }
              spectre.uploadBadUsbScript(draft).catch(() => {});
            }}>
            <Text style={styles.primaryText}>Upload to Vault</Text>
          </Pressable>

          <Pressable
            style={[
              styles.secondaryButton,
              !uploadActive ? styles.buttonDisabled : null,
            ]}
            disabled={!uploadActive}
            onPress={() => {
              spectre.cancelBadUsbUpload().catch(() => {});
            }}>
            <Text style={styles.secondaryText}>Cancel Transfer</Text>
          </Pressable>

          <Pressable
            style={styles.secondaryButton}
            onPress={() => {
              setDraft({...starterDraft});
              setImportSource(null);
              setImportError(null);
            }}>
            <Text style={styles.secondaryText}>Load Sample</Text>
          </Pressable>
        </View>
      </FieldPanel>

      <FieldPanel title="Field Controls" eyebrow="Control frame // slim surface" tone="accent">
        <View style={styles.compactRow}>
          <View style={styles.switchCopy}>
            <Text style={styles.switchTitle}>WireGuard active on phone</Text>
            <Text style={styles.switchBody}>
              Sets bit 0 in the phone control frame so Spectre can judge uplink
              readiness.
            </Text>
          </View>
          <Switch
            value={spectre.wireGuardActive}
            onValueChange={spectre.setWireGuardActive}
            trackColor={{
              false: theme.colors.panelEdge,
              true: theme.colors.cyanDim,
            }}
            thumbColor={theme.colors.white}
          />
        </View>

        <View style={styles.buttonRow}>
          <Pressable style={styles.primaryButton} onPress={spectre.queueDumpRequest}>
            <Text style={styles.primaryText}>Request Dump</Text>
          </Pressable>
          <Pressable
            style={styles.secondaryButton}
            onPress={spectre.queueCancelRequest}>
            <Text style={styles.secondaryText}>Send Cancel</Text>
          </Pressable>
        </View>
      </FieldPanel>

      <FieldPanel title="Location Source" eyebrow="GPS + enrichment">
        <View style={styles.modeRow}>
          {(['device', 'manual', 'off'] as const).map(mode => (
            <Pressable
              key={mode}
              style={[
                styles.modeButton,
                spectre.locationMode === mode ? styles.modeActive : null,
              ]}
              onPress={() => spectre.setLocationMode(mode)}>
              <Text
                style={[
                  styles.modeText,
                  spectre.locationMode === mode ? styles.modeTextActive : null,
                ]}>
                {mode}
              </Text>
            </Pressable>
          ))}
        </View>

        <Text style={styles.bodyText}>
          Current source:{' '}
          {spectre.activeLocation
            ? `${spectre.activeLocation.source} · ${spectre.activeLocation.lat.toFixed(5)}, ${spectre.activeLocation.lon.toFixed(5)}`
            : 'off'}
        </Text>

        <View style={styles.buttonRow}>
          <Pressable
            style={styles.secondaryButton}
            onPress={spectre.refreshDeviceLocation}>
            <Text style={styles.secondaryText}>Refresh Phone Fix</Text>
          </Pressable>
          <Pressable
            style={styles.secondaryButton}
            onPress={spectre.clearManualLocation}>
            <Text style={styles.secondaryText}>Clear Manual Fix</Text>
          </Pressable>
        </View>

        <View style={styles.manualGrid}>
          <TextInput
            value={spectre.manualLocationDraft.lat}
            onChangeText={lat => spectre.updateManualLocationDraft({lat})}
            style={styles.input}
            keyboardType="numeric"
            placeholder="Latitude"
            placeholderTextColor={theme.colors.textDim}
          />
          <TextInput
            value={spectre.manualLocationDraft.lon}
            onChangeText={lon => spectre.updateManualLocationDraft({lon})}
            style={styles.input}
            keyboardType="numeric"
            placeholder="Longitude"
            placeholderTextColor={theme.colors.textDim}
          />
          <TextInput
            value={spectre.manualLocationDraft.alt}
            onChangeText={alt => spectre.updateManualLocationDraft({alt})}
            style={styles.input}
            keyboardType="numeric"
            placeholder="Altitude m"
            placeholderTextColor={theme.colors.textDim}
          />
          <TextInput
            value={spectre.manualLocationDraft.accuracy}
            onChangeText={accuracy =>
              spectre.updateManualLocationDraft({accuracy})
            }
            style={styles.input}
            keyboardType="numeric"
            placeholder="Accuracy m"
            placeholderTextColor={theme.colors.textDim}
          />
        </View>

        <Pressable style={styles.primaryButton} onPress={spectre.applyManualLocation}>
          <Text style={styles.primaryText}>Arm Manual Field Fix</Text>
        </Pressable>
      </FieldPanel>

      <FieldPanel
        title="Companion Peripheral"
        eyebrow="Android bridge // foreground service"
        tone={companionTone}
        action={
          <View
            style={[
              styles.phasePill,
              peripheral.error
                ? styles.phaseDanger
                : peripheral.advertising
                  ? styles.phaseSuccess
                  : peripheral.running
                    ? styles.phaseWarn
                    : styles.phaseIdle,
            ]}>
            <Text style={styles.phasePillText}>
              {peripheral.error
                ? 'fault'
                : peripheral.advertising
                  ? 'advertising'
                  : peripheral.running
                    ? 'warming'
                    : 'offline'}
            </Text>
          </View>
        }>
        <View style={styles.peripheralHero}>
          <Text style={styles.peripheralHeadline}>
            {peripheral.advertising
              ? 'Foreground advertising active'
              : peripheral.running
                ? 'Foreground service active'
                : 'Peripheral bridge offline'}
          </Text>
          <Text style={styles.bodyText}>
            {peripheral.error ||
              `Mode ${peripheral.advertiseMode || '--'} - ${peripheral.connectedDevices} attached - secure ${
                peripheral.secureSessionReady ? 'ready' : 'waiting'
              }`}
          </Text>
        </View>

        <View style={styles.buttonRow}>
          <Pressable
            style={[
              styles.primaryButton,
              !spectre.permissions.allGranted ? styles.buttonDisabled : null,
            ]}
            disabled={!spectre.permissions.allGranted}
            onPress={() => {
              spectre.startFieldMode().catch(() => {});
            }}>
            <Text style={styles.primaryText}>Start Field Mode</Text>
          </Pressable>
          <Pressable
            style={[
              styles.secondaryButton,
              !peripheral.running ? styles.buttonDisabled : null,
            ]}
            disabled={!peripheral.running}
            onPress={() => {
              spectre.stopFieldMode().catch(() => {});
            }}>
            <Text style={styles.secondaryText}>Stop Field Mode</Text>
          </Pressable>
        </View>

        <View style={styles.peripheralGrid}>
          <PeripheralMetric
            label="Last connect"
            value={formatRelativeTime(peripheral.lastConnectedAt)}
            detail={peripheral.lastConnectedPeer || '--'}
          />
          <PeripheralMetric
            label="Last disconnect"
            value={formatRelativeTime(peripheral.lastDisconnectedAt)}
            detail={peripheral.lastDisconnectedPeer || '--'}
          />
          <PeripheralMetric
            label="Native batch rx"
            value={
              peripheral.lastBatchRecords
                ? `${peripheral.lastBatchRecords} records`
                : 'none'
            }
            detail={`${formatBytes(peripheral.lastBatchBytes)} - ${formatRelativeTime(
              peripheral.lastBatchReceivedAt,
            )}`}
          />
          <PeripheralMetric
            label="Last phone push"
            value={lastTransferRecords ? `${lastTransferRecords} records` : 'none'}
            detail={`${formatBytes(lastTransferBytes)} - ${formatRelativeTime(lastTransferAt)}`}
          />
          <PeripheralMetric
            label="Watchdog"
            value={peripheral.watchdogActive ? 'armed' : 'idle'}
            detail={`${peripheral.totalAdvertiseRestarts ?? 0} restarts`}
          />
          <PeripheralMetric
            label="Totals"
            value={`${peripheral.totalBatchesReceived ?? 0} batches`}
            detail={`${peripheral.totalBatchRecords ?? 0} records / ${formatBytes(
              peripheral.totalBatchBytes,
            )}`}
          />
          <PeripheralMetric
            label="Storage used"
            value={storage ? `${storage.usedPct}%` : '--'}
            detail={storage ? `${formatBytes(storage.freeBytes)} free` : 'no snapshot'}
          />
          <PeripheralMetric
            label="Pending upload"
            value={formatCount(storage ? pendingUpload : null)}
            detail={
              storage
                ? `mission ${formatCount(storage.pendingUploadMission)} / noise ${formatCount(storage.pendingUploadNoise)}`
                : 'waiting'
            }
          />
          <PeripheralMetric
            label="Pending enrich"
            value={formatCount(storage ? pendingEnrich : null)}
            detail={
              storage
                ? `mission ${formatCount(storage.pendingEnrichMission)} / noise ${formatCount(storage.pendingEnrichNoise)}`
                : 'waiting'
            }
          />
          <PeripheralMetric
            label="Event totals"
            value={formatCount(
              storage ? storage.missionTotal + storage.noiseTotal : null,
            )}
            detail={
              storage
                ? `P0 ${formatCount(storage.p0Total)} / P1 ${formatCount(storage.p1Total)} / P2 ${formatCount(storage.p2Total)} / P3 ${formatCount(storage.p3Total)}`
                : 'waiting'
            }
          />
        </View>

        <Pressable style={styles.secondaryButton} onPress={spectre.injectMockBatch}>
          <Text style={styles.secondaryText}>Inject Mock Batch</Text>
        </Pressable>
      </FieldPanel>

      <FieldPanel title="Field Log" eyebrow="Recent events">
        {spectre.logs.length === 0 ? (
          <Text style={styles.bodyText}>No events recorded yet.</Text>
        ) : (
          spectre.logs.slice(0, 8).map(entry => (
            <View key={entry.id} style={styles.logRow}>
              <Text style={styles.logTime}>
                {new Date(entry.timestamp).toLocaleTimeString()}
              </Text>
              <Text style={styles.logBody}>{entry.message}</Text>
            </View>
          ))
        )}
      </FieldPanel>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  scroll: {
    flex: 1,
  },
  content: {
    gap: theme.spacing.md,
    paddingBottom: theme.spacing.xxl,
  },
  bodyText: {
    color: theme.colors.textSoft,
    fontSize: 13,
    lineHeight: 19,
  },
  peripheralHero: {
    gap: theme.spacing.xs,
  },
  peripheralHeadline: {
    color: theme.colors.textStrong,
    fontSize: 18,
    lineHeight: 23,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.4,
    fontFamily: theme.fonts.title,
  },
  peripheralGrid: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: theme.spacing.xs,
  },
  peripheralMetric: {
    flexGrow: 1,
    flexBasis: '48%',
    minWidth: 132,
    padding: theme.spacing.sm,
    borderRadius: theme.radius.md,
    backgroundColor: theme.colors.bgAlt,
    borderWidth: 1,
    borderColor: theme.colors.panelEdge,
    gap: 3,
  },
  peripheralLabel: {
    color: theme.colors.textDim,
    fontSize: 10,
    textTransform: 'uppercase',
    letterSpacing: 0.8,
    fontFamily: theme.fonts.label,
  },
  peripheralValue: {
    color: theme.colors.textStrong,
    fontSize: 14,
    fontWeight: '700',
    textTransform: 'uppercase',
    fontFamily: theme.fonts.label,
  },
  peripheralDetail: {
    color: theme.colors.textSoft,
    fontSize: 11,
    lineHeight: 14,
    fontFamily: theme.fonts.label,
  },
  phasePill: {
    borderRadius: theme.radius.pill,
    paddingHorizontal: 10,
    paddingVertical: 6,
    borderWidth: 1,
  },
  phaseIdle: {
    backgroundColor: theme.colors.bgAlt,
    borderColor: theme.colors.panelEdge,
  },
  phaseAccent: {
    backgroundColor: theme.colors.cyanDim,
    borderColor: theme.colors.cyan,
  },
  phaseWarn: {
    backgroundColor: theme.colors.amberDim,
    borderColor: theme.colors.amber,
  },
  phaseSuccess: {
    backgroundColor: theme.colors.limeDim,
    borderColor: theme.colors.lime,
  },
  phaseDanger: {
    backgroundColor: theme.colors.redDim,
    borderColor: theme.colors.red,
  },
  phasePillText: {
    color: theme.colors.textStrong,
    fontSize: 10,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 1,
    fontFamily: theme.fonts.label,
  },
  summaryRow: {
    flexDirection: 'row',
    gap: theme.spacing.sm,
  },
  summaryChip: {
    flex: 1,
    minWidth: 0,
    padding: theme.spacing.sm,
    borderRadius: theme.radius.md,
    backgroundColor: theme.colors.bgAlt,
    borderWidth: 1,
    borderColor: theme.colors.panelEdge,
    gap: 4,
  },
  summaryChipWide: {
    flex: 1.5,
    minWidth: 0,
    padding: theme.spacing.sm,
    borderRadius: theme.radius.md,
    backgroundColor: theme.colors.bgAlt,
    borderWidth: 1,
    borderColor: theme.colors.panelEdge,
    gap: 4,
  },
  summaryLabel: {
    color: theme.colors.textDim,
    fontSize: 10,
    textTransform: 'uppercase',
    letterSpacing: 0.8,
    fontFamily: theme.fonts.label,
  },
  summaryValue: {
    color: theme.colors.textStrong,
    fontSize: 12,
    fontWeight: '700',
    fontFamily: theme.fonts.label,
  },
  progressTrack: {
    height: 8,
    overflow: 'hidden',
    borderRadius: theme.radius.pill,
    backgroundColor: theme.colors.chrome,
    borderWidth: 1,
    borderColor: theme.colors.panelEdge,
  },
  progressFill: {
    height: '100%',
    minWidth: 6,
    borderRadius: theme.radius.pill,
    backgroundColor: theme.colors.amber,
  },
  uploadMessage: {
    color: theme.colors.amber,
    fontSize: 12,
    lineHeight: 18,
    fontFamily: theme.fonts.label,
  },
  importMeta: {
    color: theme.colors.cyan,
    fontSize: 12,
    lineHeight: 17,
    fontFamily: theme.fonts.label,
  },
  warningBox: {
    gap: 6,
  },
  warnText: {
    color: theme.colors.amber,
    fontSize: 12,
    lineHeight: 17,
    fontFamily: theme.fonts.label,
  },
  metaGrid: {
    flexDirection: 'row',
    gap: theme.spacing.sm,
  },
  metaField: {
    flex: 1,
    gap: theme.spacing.xs,
  },
  inputLabel: {
    color: theme.colors.textDim,
    fontSize: 10,
    textTransform: 'uppercase',
    letterSpacing: 1,
    fontFamily: theme.fonts.label,
  },
  input: {
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    backgroundColor: theme.colors.bgAlt,
    color: theme.colors.textStrong,
    paddingHorizontal: theme.spacing.md,
    paddingVertical: 12,
    fontFamily: theme.fonts.body,
  },
  editorHeader: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    gap: theme.spacing.sm,
  },
  editorMeta: {
    color: theme.colors.textSoft,
    fontSize: 12,
    fontFamily: theme.fonts.label,
  },
  editor: {
    minHeight: 236,
    borderColor: theme.colors.cyanDim,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    backgroundColor: theme.colors.bgAlt,
    color: theme.colors.textStrong,
    paddingHorizontal: theme.spacing.md,
    paddingVertical: theme.spacing.md,
    fontFamily: theme.fonts.body,
    fontSize: 14,
    lineHeight: 20,
  },
  errorText: {
    color: theme.colors.red,
    fontSize: 12,
    lineHeight: 17,
    fontFamily: theme.fonts.label,
  },
  metricsRow: {
    flexDirection: 'row',
    gap: theme.spacing.sm,
  },
  metricBox: {
    flex: 1,
    minWidth: 0,
    backgroundColor: theme.colors.bgAlt,
    borderWidth: 1,
    borderColor: theme.colors.panelEdge,
    borderRadius: theme.radius.md,
    padding: theme.spacing.sm,
    gap: 4,
  },
  metricLabel: {
    color: theme.colors.textDim,
    fontSize: 10,
    textTransform: 'uppercase',
    letterSpacing: 0.8,
    fontFamily: theme.fonts.label,
  },
  metricValue: {
    color: theme.colors.textStrong,
    fontSize: 12,
    fontWeight: '700',
    fontFamily: theme.fonts.label,
  },
  buttonRow: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: theme.spacing.sm,
  },
  primaryButton: {
    backgroundColor: theme.colors.amber,
    borderRadius: theme.radius.md,
    paddingHorizontal: theme.spacing.md,
    paddingVertical: 12,
  },
  primaryText: {
    color: theme.colors.textOnAccent,
    fontSize: 13,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.8,
    fontFamily: theme.fonts.label,
  },
  secondaryButton: {
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    backgroundColor: theme.colors.bg,
    paddingHorizontal: theme.spacing.md,
    paddingVertical: 12,
  },
  secondaryText: {
    color: theme.colors.textSoft,
    fontSize: 13,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.8,
    fontFamily: theme.fonts.label,
  },
  buttonDisabled: {
    opacity: 0.38,
  },
  compactRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: theme.spacing.md,
  },
  switchCopy: {
    flex: 1,
    gap: 4,
  },
  switchTitle: {
    color: theme.colors.textStrong,
    fontSize: 14,
    fontWeight: '700',
  },
  switchBody: {
    color: theme.colors.textSoft,
    fontSize: 12,
    lineHeight: 18,
  },
  modeRow: {
    flexDirection: 'row',
    gap: theme.spacing.sm,
  },
  modeButton: {
    flex: 1,
    alignItems: 'center',
    borderWidth: 1,
    borderColor: theme.colors.panelEdge,
    borderRadius: theme.radius.md,
    backgroundColor: theme.colors.bgAlt,
    paddingVertical: 10,
    paddingHorizontal: theme.spacing.sm,
  },
  modeActive: {
    borderColor: theme.colors.amber,
    backgroundColor: theme.colors.amber,
  },
  modeText: {
    color: theme.colors.textSoft,
    fontSize: 12,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.8,
    fontFamily: theme.fonts.label,
  },
  modeTextActive: {
    color: theme.colors.textOnAccent,
  },
  manualGrid: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: theme.spacing.sm,
  },
  logRow: {
    flexDirection: 'row',
    gap: theme.spacing.sm,
    paddingVertical: 6,
    borderBottomWidth: 1,
    borderBottomColor: theme.colors.panelEdge,
  },
  logTime: {
    width: 84,
    color: theme.colors.textDim,
    fontSize: 11,
    fontFamily: theme.fonts.label,
  },
  logBody: {
    flex: 1,
    color: theme.colors.textSoft,
    fontSize: 12,
    lineHeight: 18,
  },
});
