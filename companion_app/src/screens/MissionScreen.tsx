import React from 'react';
import {Pressable, ScrollView, StyleSheet, Text, View} from 'react-native';

import {FieldPanel} from '../components/FieldPanel';
import {useSpectre} from '../state/SpectreContext';
import {theme} from '../theme/theme';

function relativeTime(timestamp?: number | null) {
  if (!timestamp) return 'never';
  const seconds = Math.max(0, Math.floor((Date.now() - timestamp) / 1000));
  if (seconds < 60) return `${seconds}s ago`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)}h ago`;
  return `${Math.floor(seconds / 86400)}d ago`;
}

function ReadinessRow({label, value, detail, good}: {label: string; value: string; detail: string; good: boolean}) {
  return (
    <View style={styles.readinessRow}>
      <View style={[styles.dot, good ? styles.dotGood : styles.dotWarn]} />
      <View style={styles.readinessCopy}>
        <Text style={styles.readinessLabel}>{label}</Text>
        <Text style={styles.readinessDetail}>{detail}</Text>
      </View>
      <Text style={[styles.readinessValue, good ? styles.valueGood : styles.valueWarn]}>{value}</Text>
    </View>
  );
}

export function MissionScreen() {
  const spectre = useSpectre();
  const missionRunning = spectre.gpsRecording && spectre.peripheralState.running;
  const gpsFresh = !!spectre.activeLocation && Date.now() - spectre.activeLocation.timestamp < 90_000;
  const phoneReady = spectre.peripheralState.running && spectre.peripheralState.advertising;
  const linked = !!spectre.peripheralState.secureSessionReady;
  const homeReady = spectre.networkStatus.vpnValidated;
  const storage = spectre.storageSnapshot;
  const pendingSpectre = storage
    ? storage.pendingUploadMission + storage.pendingUploadNoise
    : 0;
  const pendingEnrichment = storage
    ? storage.pendingEnrichMission + storage.pendingEnrichNoise
    : 0;
  const pendingPhone = spectre.relayStatus.pending;
  const locatedTargets = spectre.localization.targets.filter(target => target.estimate).length;
  const heading = !missionRunning
    ? 'Ready when you are'
    : !gpsFresh
      ? 'Waiting for an accurate phone fix'
      : linked
        ? pendingEnrichment > 0
          ? `Locating ${pendingEnrichment} observations`
          : 'Spectre is transferring field data'
        : 'Collecting location and waiting for Spectre';

  return (
    <ScrollView style={styles.scroll} contentContainerStyle={styles.content} showsVerticalScrollIndicator={false}>
      {!spectre.permissions.allGranted && (
        <FieldPanel title="One-time setup" eyebrow="Permissions" tone="warn">
          <Text style={styles.body}>Spectre needs Bluetooth, nearby-device, notification, and precise-location access to run in your pocket.</Text>
          <Pressable style={styles.primaryButton} onPress={spectre.requestPermissions}>
            <Text style={styles.primaryText}>Allow required access</Text>
          </Pressable>
        </FieldPanel>
      )}

      <FieldPanel
        title={missionRunning ? 'Mission active' : 'Mission stopped'}
        eyebrow="Field collection"
        tone={missionRunning && gpsFresh ? 'success' : missionRunning ? 'warn' : 'default'}>
        <Text style={styles.hero}>{heading}</Text>
        <Text style={styles.body}>
          {missionRunning
            ? 'Keep the phone with Spectre. GPS history is recorded continuously; Spectre connects only when it needs enrichment or safe offload.'
            : 'Start once, then carry the phone and Spectre normally. Collection, GPS matching, and safe phone storage run in the background.'}
        </Text>
        <Pressable
          disabled={!spectre.permissions.allGranted}
          style={[styles.missionButton, missionRunning ? styles.stopButton : null, !spectre.permissions.allGranted ? styles.disabled : null]}
          onPress={() => {
            (missionRunning ? spectre.stopFieldMode() : spectre.startFieldMode()).catch(() => {});
          }}>
          <Text style={[styles.missionButtonText, missionRunning ? styles.stopButtonText : null]}>
            {missionRunning ? 'Stop mission' : 'Start mission'}
          </Text>
        </Pressable>
      </FieldPanel>

      <FieldPanel title="Mission health" eyebrow="What matters">
        <ReadinessRow
          label="Phone location"
          value={gpsFresh ? 'Ready' : missionRunning ? 'Acquiring' : 'Off'}
          detail={spectre.activeLocation
            ? `±${Math.round(spectre.activeLocation.accuracy)} m · ${relativeTime(spectre.activeLocation.timestamp)}`
            : 'No recent fix'}
          good={gpsFresh}
        />
        <ReadinessRow
          label="Spectre receiver"
          value={linked ? 'Linked' : phoneReady ? 'Listening' : 'Off'}
          detail={linked
            ? 'Authenticated transfer active'
            : phoneReady
              ? `Advertising · last contact ${relativeTime(spectre.peripheralState.lastConnectedAt)}`
              : 'Start the mission to advertise'}
          good={linked || phoneReady}
        />
        <ReadinessRow
          label="Phone vault"
          value={pendingPhone ? `${pendingPhone} waiting` : 'Safe'}
          detail={`${spectre.relayStatus.published} acknowledged by home broker`}
          good={pendingPhone === 0}
        />
        <ReadinessRow
          label="Home anchor"
          value={homeReady ? 'Reachable' : spectre.networkStatus.vpnActive ? 'Tunnel stalled' : 'Offline'}
          detail={homeReady ? 'Validated WireGuard route' : 'Field data remains safe on the phone'}
          good={homeReady}
        />
      </FieldPanel>

      <FieldPanel title="Data safety" eyebrow="Automatic handoff" tone={pendingPhone > 0 || pendingSpectre > 0 ? 'warn' : 'success'}>
        <View style={styles.metricRow}>
          <View style={styles.metric}><Text style={styles.metricValue}>{pendingSpectre}</Text><Text style={styles.metricLabel}>On Spectre</Text></View>
          <View style={styles.metric}><Text style={styles.metricValue}>{pendingPhone}</Text><Text style={styles.metricLabel}>On phone</Text></View>
          <View style={styles.metric}><Text style={styles.metricValue}>{locatedTargets}</Text><Text style={styles.metricLabel}>Mapped targets</Text></View>
        </View>
        <Text style={styles.body}>{spectre.fieldTransfer.message}</Text>
        <View style={styles.buttonRow}>
          {linked && pendingSpectre > 0 && pendingEnrichment === 0 && (
            <Pressable
              disabled={spectre.fieldTransfer.phase === 'copying'}
              style={styles.primaryButton}
              onPress={() => spectre.offloadToPhone().catch(() => {})}>
              <Text style={styles.primaryText}>Save Spectre data</Text>
            </Pressable>
          )}
          {pendingPhone > 0 && homeReady && (
            <Pressable style={styles.secondaryButton} onPress={() => spectre.relayHome().catch(() => {})}>
              <Text style={styles.secondaryText}>Sync home now</Text>
            </Pressable>
          )}
          <Pressable style={styles.secondaryButton} onPress={() => spectre.refreshLocalization()}>
            <Text style={styles.secondaryText}>Refresh targets</Text>
          </Pressable>
        </View>
      </FieldPanel>

      {spectre.notifications.length > 0 && (
        <FieldPanel title="Recent activity" eyebrow="Spectre alerts">
          {spectre.notifications.slice(0, 4).map(notification => (
            <View key={`${notification.type}-${notification.seq}`} style={styles.activityRow}>
              <Text style={styles.activityText}>{notification.text}</Text>
              <Text style={styles.activityTime}>device +{Math.floor(notification.deviceUptimeMs / 60000)}m</Text>
            </View>
          ))}
        </FieldPanel>
      )}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  scroll: {flex: 1},
  content: {gap: theme.spacing.sm, paddingTop: 2, paddingBottom: theme.spacing.xxl},
  hero: {color: theme.colors.textStrong, fontSize: 21, lineHeight: 27, fontWeight: '700'},
  body: {color: theme.colors.textSoft, fontSize: 14, lineHeight: 20},
  missionButton: {backgroundColor: theme.colors.lime, borderRadius: theme.radius.md, minHeight: 54, alignItems: 'center', justifyContent: 'center', paddingHorizontal: theme.spacing.md},
  missionButtonText: {color: theme.colors.textOnAccent, fontSize: 16, fontWeight: '800', textTransform: 'uppercase', letterSpacing: 0.8},
  stopButton: {backgroundColor: theme.colors.bg, borderColor: theme.colors.panelEdgeBright, borderWidth: 1},
  stopButtonText: {color: theme.colors.textSoft},
  disabled: {opacity: 0.35},
  readinessRow: {flexDirection: 'row', alignItems: 'center', gap: 11, paddingVertical: 5},
  dot: {width: 10, height: 10, borderRadius: 5},
  dotGood: {backgroundColor: theme.colors.lime},
  dotWarn: {backgroundColor: theme.colors.amber},
  readinessCopy: {flex: 1, gap: 2},
  readinessLabel: {color: theme.colors.textStrong, fontSize: 14, fontWeight: '700'},
  readinessDetail: {color: theme.colors.textDim, fontSize: 11, lineHeight: 15},
  readinessValue: {fontSize: 11, fontWeight: '800', textTransform: 'uppercase', letterSpacing: 0.5},
  valueGood: {color: theme.colors.lime},
  valueWarn: {color: theme.colors.amber},
  metricRow: {flexDirection: 'row', gap: theme.spacing.xs},
  metric: {flex: 1, backgroundColor: theme.colors.bgAlt, borderColor: theme.colors.panelEdge, borderWidth: 1, borderRadius: theme.radius.md, alignItems: 'center', paddingVertical: 12, gap: 2},
  metricValue: {color: theme.colors.textStrong, fontSize: 20, fontWeight: '800'},
  metricLabel: {color: theme.colors.textDim, fontSize: 9, textTransform: 'uppercase', letterSpacing: 0.7},
  buttonRow: {flexDirection: 'row', flexWrap: 'wrap', gap: theme.spacing.sm},
  primaryButton: {backgroundColor: theme.colors.amber, borderRadius: theme.radius.md, paddingHorizontal: theme.spacing.md, paddingVertical: 12},
  primaryText: {color: theme.colors.textOnAccent, fontSize: 12, fontWeight: '800', textTransform: 'uppercase', letterSpacing: 0.7},
  secondaryButton: {backgroundColor: theme.colors.bg, borderColor: theme.colors.panelEdgeBright, borderWidth: 1, borderRadius: theme.radius.md, paddingHorizontal: theme.spacing.md, paddingVertical: 12},
  secondaryText: {color: theme.colors.textSoft, fontSize: 12, fontWeight: '800', textTransform: 'uppercase', letterSpacing: 0.7},
  activityRow: {borderBottomColor: theme.colors.panelEdge, borderBottomWidth: 1, paddingVertical: 8, gap: 3},
  activityText: {color: theme.colors.text, fontSize: 13},
  activityTime: {color: theme.colors.textDim, fontSize: 10},
});
