import React, {useEffect, useState} from 'react';
import {StatusBar, StyleSheet, Text, View} from 'react-native';

import {BottomTabs} from './components/BottomTabs';
import {PromptOverlay} from './components/PromptOverlay';
import {ConsoleScreen} from './screens/ConsoleScreen';
import {EnrichScreen} from './screens/EnrichScreen';
import {LinkScreen} from './screens/LinkScreen';
import {OpsScreen} from './screens/OpsScreen';
import {SpectreProvider, useSpectre} from './state/SpectreContext';
import {theme} from './theme/theme';

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
  const fieldModeReady = pocketReady && spectre.nativeRecorderActive;

  let screen = <LinkScreen />;
  if (spectre.activeTab === 'enrich') {
    screen = <EnrichScreen />;
  } else if (spectre.activeTab === 'console') {
    screen = <ConsoleScreen />;
  } else if (spectre.activeTab === 'ops') {
    screen = <OpsScreen />;
  }

  return (
    <View style={styles.safe}>
      <StatusBar barStyle="light-content" backgroundColor={theme.colors.bg} />
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
                !promptVisible && (spectre.connectedDevice || fieldModeReady)
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
                    : fieldModeReady
                      ? 'Field mode live'
                      : pocketReady
                        ? 'BLE live, GPS starting'
                        : 'Standby'}
              </Text>
            </View>
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
    </View>
  );
}

export default function App() {
  return (
    <SpectreProvider>
      <AppShell />
    </SpectreProvider>
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
    alignItems: 'center',
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
  screen: {
    flex: 1,
    minHeight: 0,
  },
});
