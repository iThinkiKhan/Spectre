import React from 'react';
import {Pressable, StyleSheet, Text, TextInput, View} from 'react-native';

import {FieldPanel} from './FieldPanel';
import {useSpectre} from '../state/SpectreContext';
import {theme} from '../theme/theme';

// GPS source selector + manual fix entry. Lives on the Enrich tab because it
// feeds enrichment; previously it was buried in the long Vault page.
export function LocationSourcePanel() {
  const spectre = useSpectre();

  return (
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
          onChangeText={accuracy => spectre.updateManualLocationDraft({accuracy})}
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
  );
}

const styles = StyleSheet.create({
  bodyText: {
    color: theme.colors.textSoft,
    fontSize: 13,
    lineHeight: 19,
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
  input: {
    flexGrow: 1,
    flexBasis: '47%',
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    backgroundColor: theme.colors.bgAlt,
    color: theme.colors.textStrong,
    paddingHorizontal: theme.spacing.md,
    paddingVertical: 12,
    fontFamily: theme.fonts.body,
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
    alignItems: 'center',
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
});
