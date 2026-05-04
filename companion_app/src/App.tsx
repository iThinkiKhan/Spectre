import React, {useEffect, useState} from 'react';
import {StatusBar, StyleSheet, Text, View} from 'react-native';
import {SafeAreaProvider, SafeAreaView} from 'react-native-safe-area-context';

import {BottomTabs} from './components/BottomTabs';
import {PromptOverlay} from './components/PromptOverlay';
import {EnrichScreen} from './screens/EnrichScreen';
import {LinkScreen} from './screens/LinkScreen';
import {OpsScreen} from './screens/OpsScreen';
import {SpectreProvider, useSpectre} from './state/SpectreContext';
import {theme} from './theme/theme';

type RailTone = 'default' | 'accent' | 'success' | 'warn';

function humanizeState(value: string) {
  return value.replace(/_/g, ' ');
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

function railToneForLink(state: string): RailTone {
  if (state === 'connected' || state === 'reconnecting') {
    return 'success';
  }
  if (state === 'connecting' || state === 'scanning') {
    return 'accent';
  }
  return 'default';
}

function railToneForBackhaul(connectedDevices: number): RailTone {
  return connectedDevices > 0 ? 'success' : 'default';
}

function railToneForBatch(records?: number | null): RailTone {
  return records && records > 0 ? 'accent' : 'default';
}

function RailStat({
  label,
  value,
  detail,
  tone = 'default',
}: {
  label: string;
  value: string;
  detail: string;
  tone?: RailTone;
}) {
  return (
    <View
      style={[
        styles.railStat,
        tone === 'accent' ? styles.railStatAccent : null,
        tone === 'success' ? styles.railStatSuccess : null,
        tone === 'warn' ? styles.railStatWarn : null,
      ]}>
      <Text style={styles.railLabel}>{label}</Text>
      <Text numberOfLines={1} style={styles.railValue}>
        {value}
      </Text>
      <Text numberOfLines={1} style={styles.railDetail}>
        {detail}
      </Text>
    </View>
  );
}

function AppShell() {
  const spectre = useSpectre();
  const [dismissedPromptToken, setDismissedPromptToken] = useState<number | null>(
    null,
  );

  useEffect(() => {
    if (spectre.promptState.token !== dismissedPromptToken) {
      setDismissedPromptToken(null);
    }
  }, [dismissedPromptToken, spectre.promptState.token]);

  const promptVisible =
    spectre.promptState.awaitingReply &&
    spectre.promptState.token !== dismissedPromptToken;
  const pocketReady =
    spectre.peripheralState.running && spectre.peripheralState.advertising;
  const lastBatchRecords =
    spectre.lastPublishedBatch?.records ??
    spectre.peripheralState.lastBatchRecords ??
    0;

  let screen = <LinkScreen />;
  if (spectre.activeTab === 'enrich') {
    screen = <EnrichScreen />;
  } else if (spectre.activeTab === 'ops') {
    screen = <OpsScreen />;
  }

  return (
    <SafeAreaView style={styles.safe}>
      <StatusBar
        barStyle="light-content"
        backgroundColor={theme.colors.bg}
      />
      <View style={styles.root}>
        <View style={styles.chrome}>
          <View style={styles.chromeRule} />
          <View style={styles.chromeGlow} />
          <View style={styles.chromeHeader}>
            <View style={styles.brandBlock}>
              <Text style={styles.brandEyebrow}>Spectre // Field Companion</Text>
              <Text style={styles.brandTitle}>Phone Link</Text>
            </View>

            <View
              style={[
                styles.livePill,
                promptVisible ? styles.livePillWarn : null,
                !promptVisible && (spectre.connectedDevice || pocketReady)
                  ? styles.livePillSuccess
                  : null,
              ]}>
              <Text
                style={[
                  styles.livePillText,
                  promptVisible || spectre.connectedDevice
                    ? styles.livePillTextActive
                    : null,
                ]}>
                {promptVisible
                  ? 'Prompt waiting'
                  : spectre.connectedDevice
                    ? 'Mission link live'
                    : pocketReady
                      ? 'Pocket mode ready'
                      : 'Standby'}
              </Text>
            </View>
          </View>

          <Text style={styles.chromeNote}>
            Text first. Foreground BLE alive. Control stays slim.
          </Text>

          <View style={styles.railRow}>
            <RailStat
              label="Text link"
              value={humanizeState(spectre.connectionState)}
              detail={spectre.connectedDevice?.name || 'Scan ready'}
              tone={railToneForLink(spectre.connectionState)}
            />
            <RailStat
              label="Backhaul"
              value={
                spectre.peripheralState.connectedDevices > 0
                  ? 'linked'
                  : spectre.peripheralState.advertising
                    ? 'advertising'
                    : spectre.peripheralState.running
                      ? 'starting'
                      : 'offline'
              }
              detail={
                spectre.peripheralState.connectedDevices > 0
                  ? `${spectre.peripheralState.connectedDevices} attached`
                  : `mode ${spectre.peripheralState.advertiseMode || '--'}`
              }
              tone={railToneForBackhaul(spectre.peripheralState.connectedDevices)}
            />
            <RailStat
              label="Last batch"
              value={lastBatchRecords > 0 ? `${lastBatchRecords} records` : 'none'}
              detail={
                spectre.lastPublishedBatch
                  ? `sent ${formatRelativeTime(spectre.lastPublishedBatch.sentAt)}`
                  : formatRelativeTime(spectre.peripheralState.lastBatchReceivedAt)
              }
              tone={railToneForBatch(lastBatchRecords)}
            />
          </View>
        </View>

        <View style={styles.screen}>{screen}</View>

        <BottomTabs
          activeTab={spectre.activeTab}
          onSelect={spectre.setActiveTab}
        />
      </View>

      <PromptOverlay
        visible={promptVisible}
        promptText={spectre.promptState.promptText}
        promptKind={spectre.promptState.promptKind}
        receipt={spectre.promptState.receipt}
        replyError={spectre.promptState.replyError}
        status={spectre.promptState.rawStatus}
        onSubmit={async text => {
          setDismissedPromptToken(null);
          await spectre.submitPromptReply(text);
        }}
        onDismiss={() => {
          setDismissedPromptToken(spectre.promptState.token ?? Date.now());
        }}
      />
    </SafeAreaView>
  );
}

export default function App() {
  return (
    <SafeAreaProvider>
      <SpectreProvider>
        <AppShell />
      </SpectreProvider>
    </SafeAreaProvider>
  );
}

const styles = StyleSheet.create({
  safe: {
    flex: 1,
    backgroundColor: theme.colors.bg,
  },
  root: {
    flex: 1,
    paddingHorizontal: theme.spacing.md,
    paddingTop: theme.spacing.xs,
    paddingBottom: theme.spacing.sm,
    gap: theme.spacing.sm,
    backgroundColor: theme.colors.bg,
  },
  chrome: {
    overflow: 'hidden',
    backgroundColor: theme.colors.panelAlt,
    borderColor: theme.colors.panelEdgeBright,
    borderWidth: 1,
    borderRadius: theme.radius.lg,
    paddingHorizontal: theme.spacing.md,
    paddingTop: 12,
    paddingBottom: 10,
    gap: theme.spacing.xs,
  },
  chromeRule: {
    position: 'absolute',
    top: 0,
    left: 0,
    right: 0,
    height: 2,
    backgroundColor: theme.colors.amber,
  },
  chromeGlow: {
    position: 'absolute',
    top: -22,
    right: -14,
    width: 132,
    height: 92,
    borderRadius: 80,
    backgroundColor: theme.colors.gridAlt,
  },
  chromeHeader: {
    flexDirection: 'row',
    alignItems: 'flex-start',
    justifyContent: 'space-between',
    gap: theme.spacing.sm,
  },
  brandBlock: {
    flex: 1,
    gap: 1,
  },
  brandEyebrow: {
    color: theme.colors.textSoft,
    fontSize: 10,
    fontWeight: '700',
    letterSpacing: 1.2,
    textTransform: 'uppercase',
    fontFamily: theme.fonts.label,
  },
  brandTitle: {
    color: theme.colors.amber,
    fontSize: 20,
    lineHeight: 22,
    fontWeight: '700',
    fontFamily: theme.fonts.title,
    textTransform: 'uppercase',
    letterSpacing: 0.8,
  },
  livePill: {
    borderRadius: theme.radius.pill,
    paddingHorizontal: 11,
    paddingVertical: 7,
    backgroundColor: theme.colors.chrome,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
  },
  livePillWarn: {
    backgroundColor: theme.colors.amber,
    borderColor: theme.colors.amber,
  },
  livePillSuccess: {
    backgroundColor: theme.colors.lime,
    borderColor: theme.colors.lime,
  },
  livePillText: {
    color: theme.colors.textStrong,
    fontSize: 11,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.8,
    fontFamily: theme.fonts.label,
  },
  livePillTextActive: {
    color: theme.colors.textOnAccent,
  },
  chromeNote: {
    color: theme.colors.textSoft,
    fontSize: 11,
    lineHeight: 14,
    textTransform: 'uppercase',
    letterSpacing: 0.4,
    fontFamily: theme.fonts.label,
  },
  railRow: {
    flexDirection: 'row',
    gap: theme.spacing.xs,
  },
  railStat: {
    flex: 1,
    minWidth: 0,
    backgroundColor: theme.colors.bgAlt,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    paddingHorizontal: theme.spacing.sm,
    paddingVertical: 8,
    gap: 2,
  },
  railStatAccent: {
    borderColor: theme.colors.cyan,
  },
  railStatSuccess: {
    borderColor: theme.colors.lime,
  },
  railStatWarn: {
    borderColor: theme.colors.amber,
  },
  railLabel: {
    color: theme.colors.textDim,
    fontSize: 9,
    letterSpacing: 0.8,
    textTransform: 'uppercase',
    fontFamily: theme.fonts.label,
  },
  railValue: {
    color: theme.colors.textStrong,
    fontSize: 12,
    fontWeight: '700',
    textTransform: 'uppercase',
    fontFamily: theme.fonts.label,
  },
  railDetail: {
    color: theme.colors.textSoft,
    fontSize: 10,
    lineHeight: 12,
    fontFamily: theme.fonts.label,
  },
  screen: {
    flex: 1,
    minHeight: 0,
  },
});
