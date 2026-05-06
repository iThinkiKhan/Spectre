import React from 'react';
import {
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  View,
} from 'react-native';

import {FieldPanel} from '../components/FieldPanel';
import {useSpectre} from '../state/SpectreContext';
import {theme} from '../theme/theme';

function statusTone(state: string) {
  if (state === 'connected' || state === 'reconnecting') {
    return styles.signalGood;
  }
  if (state === 'error') {
    return styles.signalBad;
  }
  return styles.signalIdle;
}

function connectionPanelTone(state: string) {
  if (state === 'connected' || state === 'reconnecting') {
    return 'accent' as const;
  }
  if (state === 'error') {
    return 'danger' as const;
  }
  return 'default' as const;
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

function TelemetryTile({
  label,
  value,
  detail,
}: {
  label: string;
  value: string;
  detail: string;
}) {
  return (
    <View style={styles.telemetryTile}>
      <Text style={styles.telemetryLabel}>{label}</Text>
      <Text numberOfLines={1} style={styles.telemetryValue}>
        {value}
      </Text>
      <Text numberOfLines={1} style={styles.telemetryDetail}>
        {detail}
      </Text>
    </View>
  );
}

export function LinkScreen() {
  const spectre = useSpectre();
  const pocketReady =
    spectre.peripheralState.running && spectre.peripheralState.advertising;
  const lastBackhaulAt =
    spectre.peripheralState.lastConnectedAt ??
    spectre.peripheralState.lastDisconnectedAt;
  const lastBackhaulPeer =
    spectre.peripheralState.lastConnectedPeer ??
    spectre.peripheralState.lastDisconnectedPeer ??
    '--';
  const lastBatchRecords =
    spectre.lastPublishedBatch?.records ??
    spectre.peripheralState.lastBatchRecords ??
    0;
  const lastBatchBytes =
    spectre.lastPublishedBatch?.bytes ??
    spectre.peripheralState.lastBatchBytes ??
    0;
  const lastBatchAt =
    spectre.lastPublishedBatch?.sentAt ??
    spectre.peripheralState.lastBatchReceivedAt;
  const storage = spectre.storageSnapshot;
  const pendingUpload =
    (storage?.pendingUploadMission ?? 0) + (storage?.pendingUploadNoise ?? 0);
  const pendingEnrich =
    (storage?.pendingEnrichMission ?? 0) + (storage?.pendingEnrichNoise ?? 0);

  const troubleshooting: string[] = [];
  if (!spectre.permissions.allGranted) {
    troubleshooting.push('Grant Bluetooth, advertise, and precise location permissions.');
  }
  if (spectre.adapterState !== 'PoweredOn') {
    troubleshooting.push('Turn Bluetooth on before scanning or advertising.');
  }
  if (!spectre.peripheralState.running) {
    troubleshooting.push('Start the phone companion foreground service before field carry.');
  }
  if (!spectre.connectedDevice) {
    troubleshooting.push('Scan for a Spectre unit that is advertising the text service.');
  }
  if (spectre.statusSummary.backhaulStateLabel === 'IDLE') {
    troubleshooting.push('If text works but enrichment is idle, let Spectre rediscover the phone companion service.');
  }

  return (
    <ScrollView
      style={styles.scroll}
      contentContainerStyle={styles.content}
      showsVerticalScrollIndicator={false}>
      {!spectre.permissions.allGranted && (
        <FieldPanel title="Permissions" eyebrow="Required" tone="warn">
          <Text style={styles.bodyText}>
            Android needs scan, connect, advertise, and precise location access before the dual BLE path can stay reliable.
          </Text>
          {spectre.missingPermissionLabels.map(label => (
            <Text key={label} style={styles.listItem}>
              {`\u2022 ${label}`}
            </Text>
          ))}
          <Pressable
            style={styles.primaryButton}
            onPress={spectre.requestPermissions}>
            <Text style={styles.primaryText}>Grant Permissions</Text>
          </Pressable>
        </FieldPanel>
      )}

      <FieldPanel
        title="Pocket Beacon"
        eyebrow="Foreground BLE"
        tone={pocketReady ? 'success' : spectre.peripheralState.error ? 'danger' : 'warn'}
        action={
          <View
            style={[
              styles.signalPill,
              pocketReady ? styles.signalGood : styles.signalIdle,
            ]}>
            <Text style={styles.signalText}>
              {pocketReady ? 'ready' : spectre.peripheralState.running ? 'warming' : 'offline'}
            </Text>
          </View>
        }>
        <Text style={styles.beaconHeadline}>
          {pocketReady
            ? 'Lock-screen advertising is active'
            : spectre.peripheralState.running
              ? 'Foreground service is running'
              : 'Phone peripheral is not running'}
        </Text>
        <Text style={styles.metricDetail}>
          {spectre.peripheralState.error ||
            `Mode ${spectre.peripheralState.advertiseMode || '--'} - watchdog ${
              spectre.peripheralState.watchdogActive ? 'armed' : 'idle'
            }`}
        </Text>

        <View style={styles.telemetryGrid}>
          <TelemetryTile
            label="Foreground"
            value={spectre.peripheralState.running ? 'active' : 'off'}
            detail={
              spectre.peripheralState.advertiseStartConfirmed
                ? `since ${formatRelativeTime(spectre.peripheralState.lastAdvertiseStartedAt)}`
                : 'waiting for callback'
            }
          />
          <TelemetryTile
            label="Advertising"
            value={spectre.peripheralState.advertising ? 'yes' : 'no'}
            detail={
              spectre.peripheralState.lastAdvertiseFailureCode
                ? `fault ${spectre.peripheralState.lastAdvertiseFailureCode}`
                : `${spectre.peripheralState.totalAdvertiseRestarts ?? 0} restarts`
            }
          />
          <TelemetryTile
            label="Last connect"
            value={formatRelativeTime(lastBackhaulAt)}
            detail={lastBackhaulPeer}
          />
          <TelemetryTile
            label="Last batch"
            value={lastBatchRecords > 0 ? `${lastBatchRecords} records` : 'none'}
            detail={`${formatBytes(lastBatchBytes)} - ${formatRelativeTime(lastBatchAt)}`}
          />
          <TelemetryTile
            label="Storage"
            value={storage ? `${storage.usedPct}% used` : 'waiting'}
            detail={
              storage
                ? `up ${formatCount(pendingUpload)} / enrich ${formatCount(pendingEnrich)}`
                : 'no snapshot yet'
            }
          />
          <TelemetryTile
            label="Events"
            value={storage ? formatCount(storage.missionTotal + storage.noiseTotal) : '--'}
            detail={
              storage
                ? `mission ${formatCount(storage.missionTotal)} / noise ${formatCount(storage.noiseTotal)}`
                : 'counts unavailable'
            }
          />
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
              !spectre.peripheralState.running ? styles.buttonDisabled : null,
            ]}
            disabled={!spectre.peripheralState.running}
            onPress={() => {
              spectre.stopFieldMode().catch(() => {});
            }}>
            <Text style={styles.secondaryText}>Stop Field Mode</Text>
          </Pressable>
        </View>
      </FieldPanel>

      <FieldPanel
        title="Text Link"
        eyebrow="Quick actions"
        tone={connectionPanelTone(spectre.connectionState)}
        action={
          <View style={[styles.signalPill, statusTone(spectre.connectionState)]}>
            <Text style={styles.signalText}>{spectre.connectionState}</Text>
          </View>
        }>
        <Text style={styles.linkHeadline}>
          {spectre.connectedDevice?.name || 'No Spectre unit selected'}
        </Text>
        <Text style={styles.metricDetail}>
          {spectre.connectionDetail || 'Scan and connect to the Spectre text service.'}
        </Text>

        <View style={styles.quickStats}>
          <View style={styles.quickStat}>
            <Text style={styles.quickStatLabel}>Adapter</Text>
            <Text numberOfLines={1} style={styles.quickStatValue}>
              {spectre.adapterState}
            </Text>
          </View>

          <View style={styles.quickStat}>
            <Text style={styles.quickStatLabel}>Prompt</Text>
            <Text numberOfLines={1} style={styles.quickStatValue}>
              {spectre.promptState.awaitingReply
                ? 'awaiting'
                : spectre.promptState.submittedReply
                  ? 'sent'
                  : 'idle'}
            </Text>
          </View>

          <View style={styles.quickStat}>
            <Text style={styles.quickStatLabel}>Nearby</Text>
            <Text numberOfLines={1} style={styles.quickStatValue}>
              {String(spectre.discoveredDevices.length)}
            </Text>
          </View>
        </View>

        <View style={styles.buttonRow}>
          <Pressable style={styles.primaryButton} onPress={spectre.scanForDevices}>
            <Text style={styles.primaryText}>
              {spectre.connectedDevice ? 'Refresh Scan' : 'Scan for Spectre'}
            </Text>
          </Pressable>
          <Pressable style={styles.secondaryButton} onPress={spectre.disconnect}>
            <Text style={styles.secondaryText}>Disconnect</Text>
          </Pressable>
        </View>
      </FieldPanel>

      <FieldPanel
        title="Nearby Spectre Units"
        eyebrow="Text service"
        action={
          <Text style={styles.sectionMeta}>
            {spectre.discoveredDevices.length} seen
          </Text>
        }>
        {spectre.discoveredDevices.length === 0 ? (
          <Text style={styles.bodyText}>
            No Spectre text service results yet. Run a scan with Bluetooth on and the device awake.
          </Text>
        ) : (
          spectre.discoveredDevices.map(device => (
            <Pressable
              key={device.id}
              style={[
                styles.deviceRow,
                spectre.connectedDevice?.id === device.id ? styles.deviceRowActive : null,
              ]}
              onPress={() => spectre.connectToDevice(device.id)}>
              <View style={styles.deviceInfo}>
                <Text style={styles.deviceName}>{device.name}</Text>
                <Text style={styles.deviceMeta}>{device.id}</Text>
              </View>
              <View style={styles.deviceRight}>
                <Text style={styles.deviceRssi}>
                  {typeof device.rssi === 'number' ? `${device.rssi} dBm` : 'RSSI --'}
                </Text>
                <Text style={styles.deviceAction}>
                  {spectre.connectedDevice?.id === device.id ? 'linked' : 'connect'}
                </Text>
              </View>
            </Pressable>
          ))
        )}
      </FieldPanel>

      {troubleshooting.length > 0 && (
        <FieldPanel title="Troubleshooting" eyebrow="Field notes" tone="default">
          <Text style={styles.bodyText}>
            Adapter state: {spectre.adapterState}. GPS on Spectre:{' '}
            {spectre.statusSummary.gpsValid ? 'fresh' : 'not confirmed'}.
          </Text>
          {troubleshooting.map(step => (
            <Text key={step} style={styles.listItem}>
              {`\u2022 ${step}`}
            </Text>
          ))}
        </FieldPanel>
      )}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  scroll: {
    flex: 1,
  },
  content: {
    gap: theme.spacing.sm,
    paddingTop: 2,
    paddingBottom: theme.spacing.xxl,
  },
  linkHeadline: {
    color: theme.colors.text,
    fontSize: 18,
    fontWeight: '700',
  },
  beaconHeadline: {
    color: theme.colors.textStrong,
    fontSize: 19,
    lineHeight: 24,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.4,
    fontFamily: theme.fonts.title,
  },
  metricDetail: {
    color: theme.colors.textSoft,
    fontSize: 13,
    lineHeight: 18,
  },
  telemetryGrid: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: theme.spacing.xs,
  },
  telemetryTile: {
    flexGrow: 1,
    flexBasis: '48%',
    minWidth: 132,
    backgroundColor: theme.colors.bgAlt,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    paddingHorizontal: theme.spacing.sm,
    paddingVertical: theme.spacing.sm,
    gap: 3,
  },
  telemetryLabel: {
    color: theme.colors.textDim,
    fontSize: 10,
    textTransform: 'uppercase',
    letterSpacing: 0.8,
    fontFamily: theme.fonts.label,
  },
  telemetryValue: {
    color: theme.colors.textStrong,
    fontSize: 15,
    fontWeight: '700',
    textTransform: 'uppercase',
    fontFamily: theme.fonts.label,
  },
  telemetryDetail: {
    color: theme.colors.textSoft,
    fontSize: 11,
    lineHeight: 14,
    fontFamily: theme.fonts.label,
  },
  quickStats: {
    flexDirection: 'row',
    gap: theme.spacing.xs,
  },
  quickStat: {
    flex: 1,
    minWidth: 0,
    backgroundColor: theme.colors.bgAlt,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    paddingHorizontal: theme.spacing.sm,
    paddingVertical: theme.spacing.sm,
    gap: 2,
  },
  quickStatLabel: {
    color: theme.colors.textDim,
    fontSize: 10,
    textTransform: 'uppercase',
    letterSpacing: 0.8,
    fontFamily: theme.fonts.label,
  },
  quickStatValue: {
    color: theme.colors.text,
    fontSize: 13,
    fontWeight: '700',
    textTransform: 'uppercase',
  },
  signalPill: {
    alignSelf: 'flex-start',
    borderRadius: theme.radius.pill,
    paddingHorizontal: 10,
    paddingVertical: 5,
  },
  signalIdle: {
    backgroundColor: theme.colors.panelEdge,
  },
  signalGood: {
    backgroundColor: theme.colors.limeDim,
  },
  signalBad: {
    backgroundColor: theme.colors.redDim,
  },
  signalText: {
    color: theme.colors.textStrong,
    fontSize: 10,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.8,
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
  sectionMeta: {
    color: theme.colors.amber,
    fontSize: 11,
    fontFamily: theme.fonts.label,
  },
  bodyText: {
    color: theme.colors.textSoft,
    fontSize: 14,
    lineHeight: 20,
  },
  listItem: {
    color: theme.colors.textSoft,
    fontSize: 13,
    lineHeight: 20,
  },
  deviceRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: theme.spacing.sm,
    backgroundColor: theme.colors.bgAlt,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    paddingHorizontal: theme.spacing.md,
    paddingVertical: 14,
  },
  deviceRowActive: {
    borderColor: theme.colors.amber,
  },
  deviceInfo: {
    flex: 1,
    gap: 2,
  },
  deviceName: {
    color: theme.colors.text,
    fontSize: 15,
    fontWeight: '700',
  },
  deviceMeta: {
    color: theme.colors.textDim,
    fontSize: 11,
  },
  deviceRight: {
    alignItems: 'flex-end',
    gap: 4,
  },
  deviceRssi: {
    color: theme.colors.cyan,
    fontSize: 12,
    fontWeight: '700',
    fontFamily: theme.fonts.label,
  },
  deviceAction: {
    color: theme.colors.amber,
    fontSize: 11,
    textTransform: 'uppercase',
    letterSpacing: 0.8,
    fontFamily: theme.fonts.label,
    fontWeight: '700',
  },
});
