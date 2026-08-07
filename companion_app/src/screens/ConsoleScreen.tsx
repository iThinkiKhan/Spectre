import React from 'react';
import {ScrollView, StyleSheet, Text, View} from 'react-native';

import {DeviceCommandsPanel} from '../components/DeviceCommandsPanel';
import {DeviceConsolePanel} from '../components/DeviceConsolePanel';
import {FieldPanel} from '../components/FieldPanel';
import {useSpectre} from '../state/SpectreContext';
import {theme} from '../theme/theme';

// Device-interaction tab: the interactive console up top, the one-tap command
// grid below it (kept by request), and the app-side event log for diagnostics.
export function ConsoleScreen() {
  const spectre = useSpectre();

  return (
    <ScrollView
      style={styles.scroll}
      contentContainerStyle={styles.content}
      showsVerticalScrollIndicator={false}>
      <DeviceConsolePanel />

      <DeviceCommandsPanel />

      <FieldPanel title="Field Log" eyebrow="Recent app events">
        {spectre.logs.length === 0 ? (
          <Text style={styles.bodyText}>No events recorded yet.</Text>
        ) : (
          spectre.logs.slice(0, 12).map(entry => (
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
