import React from 'react';
import {Pressable, ScrollView, StyleSheet, Text, View} from 'react-native';

import {FieldPanel} from '../components/FieldPanel';
import {GeoMap} from '../components/GeoMap';
import {useSpectre} from '../state/SpectreContext';
import {theme} from '../theme/theme';

export function MapScreen() {
  const spectre = useSpectre();
  const target =
    spectre.localization.targets.find(item => item.id === spectre.selectedTargetId) ??
    spectre.localization.targets[0] ??
    null;
  const estimate = target?.estimate ?? null;

  return (
    <ScrollView style={styles.scroll} contentContainerStyle={styles.content} showsVerticalScrollIndicator={false}>
      {target ? (
        <>
          <FieldPanel
            title={target.label}
            eyebrow={`${target.kind.replace('-', ' ')} · ${target.lastRssi} dBm`}
            tone={estimate?.confidence === 'high' ? 'success' : estimate?.confidence === 'medium' ? 'accent' : 'warn'}>
            <Text numberOfLines={1} style={styles.identity}>{target.identity}</Text>
            <View style={styles.metricRow}>
              <View style={styles.metric}><Text style={styles.metricValue}>{estimate?.sampleCount ?? 0}</Text><Text style={styles.metricLabel}>Distinct fixes</Text></View>
              <View style={styles.metric}><Text style={styles.metricValue}>{estimate ? `±${Math.round(estimate.uncertaintyM)}m` : '—'}</Text><Text style={styles.metricLabel}>Uncertainty</Text></View>
              <View style={styles.metric}><Text style={styles.metricValue}>{estimate?.confidence ?? 'none'}</Text><Text style={styles.metricLabel}>Confidence</Text></View>
            </View>
            <Text style={styles.body}>
              {!estimate
                ? 'No GPS-correlated observation is available.'
                : estimate.confidence === 'insufficient'
                  ? 'This marker is the receiver position, not a solved transmitter position. Collect from at least two more separated locations.'
                  : 'The amber marker is the signal-weighted estimate; the amber circle is the current uncertainty region. Cyan dots are receiver observations and green is the phone.'}
            </Text>
          </FieldPanel>
          <GeoMap estimate={estimate} observations={target.observations} phoneLocation={spectre.activeLocation} />
          <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={styles.targetStrip}>
            {spectre.localization.targets.slice(0, 30).map(item => (
              <Pressable key={item.id} style={[styles.targetChip, item.id === target.id ? styles.targetChipActive : null]} onPress={() => spectre.setSelectedTargetId(item.id)}>
                <Text numberOfLines={1} style={[styles.targetChipText, item.id === target.id ? styles.targetChipTextActive : null]}>{item.label}</Text>
                <Text style={styles.targetChipMeta}>{item.observations.length} fixes</Text>
              </Pressable>
            ))}
          </ScrollView>
        </>
      ) : (
        <>
          <GeoMap estimate={null} observations={[]} phoneLocation={spectre.activeLocation} />
          <FieldPanel title="Build a location track" eyebrow="No target selected" tone="warn">
            <Text style={styles.body}>Start a mission, move through the area, and save Spectre data to the phone. Targets will appear here automatically.</Text>
          </FieldPanel>
        </>
      )}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  scroll: {flex: 1}, content: {gap: theme.spacing.sm, paddingTop: 2, paddingBottom: theme.spacing.xxl},
  identity: {color: theme.colors.textDim, fontSize: 10, fontFamily: theme.fonts.label}, body: {color: theme.colors.textSoft, fontSize: 13, lineHeight: 19},
  metricRow: {flexDirection: 'row', gap: theme.spacing.xs}, metric: {flex: 1, minWidth: 0, backgroundColor: theme.colors.bgAlt, borderColor: theme.colors.panelEdge, borderWidth: 1, borderRadius: theme.radius.md, paddingVertical: 11, alignItems: 'center', gap: 2},
  metricValue: {color: theme.colors.textStrong, fontSize: 15, fontWeight: '800', textTransform: 'uppercase'}, metricLabel: {color: theme.colors.textDim, fontSize: 8, textTransform: 'uppercase', letterSpacing: 0.5},
  targetStrip: {gap: 8, paddingVertical: 2}, targetChip: {width: 145, backgroundColor: theme.colors.panel, borderColor: theme.colors.panelEdge, borderWidth: 1, borderRadius: theme.radius.md, paddingHorizontal: 11, paddingVertical: 9, gap: 2}, targetChipActive: {borderColor: theme.colors.amber, backgroundColor: theme.colors.panelAlt},
  targetChipText: {color: theme.colors.textSoft, fontSize: 12, fontWeight: '700'}, targetChipTextActive: {color: theme.colors.amber}, targetChipMeta: {color: theme.colors.textDim, fontSize: 9},
});
