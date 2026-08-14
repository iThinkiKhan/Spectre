import React, {useMemo, useState} from 'react';
import {Pressable, ScrollView, StyleSheet, Text, TextInput, View} from 'react-native';

import {FieldPanel} from '../components/FieldPanel';
import type {LocalizationTarget} from '../services/localization/LocalizationService';
import {useSpectre} from '../state/SpectreContext';
import {theme} from '../theme/theme';

function age(timestamp: number) {
  const seconds = Math.max(0, Math.floor((Date.now() - timestamp) / 1000));
  if (seconds < 60) return 'now';
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)}h`;
  return `${Math.floor(seconds / 86400)}d`;
}

function kindName(target: LocalizationTarget) {
  if (target.kind === 'access-point') return 'Wi-Fi AP';
  if (target.kind === 'wifi-device') return 'Wi-Fi device';
  if (target.kind === 'subghz') return 'Sub-GHz';
  return target.kind.toUpperCase();
}

export function TargetsScreen() {
  const spectre = useSpectre();
  const [query, setQuery] = useState('');
  const targets = useMemo(() => {
    const normalized = query.trim().toLowerCase();
    if (!normalized) return spectre.localization.targets;
    return spectre.localization.targets.filter(target =>
      `${target.label} ${target.identity} ${target.kind}`.toLowerCase().includes(normalized),
    );
  }, [query, spectre.localization.targets]);

  return (
    <ScrollView style={styles.scroll} contentContainerStyle={styles.content} showsVerticalScrollIndicator={false} keyboardShouldPersistTaps="handled">
      <FieldPanel title="Detected transmitters" eyebrow={`${spectre.localization.targets.length} targets · ${spectre.localization.locatedRecordCount} located observations`}>
        <Text style={styles.body}>Choose a transmitter to see its observation geometry and best current estimate. More separated observations improve confidence.</Text>
        <TextInput
          value={query}
          onChangeText={setQuery}
          placeholder="Search SSID, MAC, fingerprint…"
          placeholderTextColor={theme.colors.textDim}
          autoCapitalize="none"
          autoCorrect={false}
          style={styles.search}
        />
        <Pressable style={styles.refresh} onPress={() => spectre.refreshLocalization()}>
          <Text style={styles.refreshText}>Refresh phone archive</Text>
        </Pressable>
      </FieldPanel>

      {targets.length === 0 ? (
        <FieldPanel title="No targets yet" eyebrow="Field collection" tone="warn">
          <Text style={styles.body}>Run a mission, let Spectre observe transmitters from multiple positions, then save its data to the phone.</Text>
        </FieldPanel>
      ) : targets.map(target => {
        const estimate = target.estimate;
        const selected = spectre.selectedTargetId === target.id;
        return (
          <Pressable
            key={target.id}
            onPress={() => {
              spectre.setSelectedTargetId(target.id);
              spectre.setActiveTab('map');
            }}
            style={[styles.target, selected ? styles.targetSelected : null]}>
            <View style={styles.targetHeader}>
              <View style={styles.targetCopy}>
                <Text numberOfLines={1} style={styles.targetName}>{target.label}</Text>
                <Text numberOfLines={1} style={styles.identity}>{target.identity}</Text>
              </View>
              <View style={styles.signalBlock}>
                <Text style={styles.rssi}>{target.lastRssi} dBm</Text>
                <Text style={styles.age}>{age(target.lastSeen)}</Text>
              </View>
            </View>
            <View style={styles.badges}>
              <Text style={styles.badge}>{kindName(target)}</Text>
              <Text style={styles.badge}>{target.observations.length} GPS fixes</Text>
              <Text style={[styles.badge, estimate?.confidence === 'high' ? styles.goodBadge : estimate?.confidence === 'medium' ? styles.midBadge : null]}>
                {estimate?.confidence ?? 'unmapped'}
              </Text>
            </View>
            <Text style={styles.detail}>
              {estimate
                ? estimate.confidence === 'insufficient'
                  ? 'One location only — keep moving to establish geometry.'
                  : `Estimated uncertainty ±${Math.round(estimate.uncertaintyM)} m · geometry ${Math.round(estimate.geometrySpreadM)} m`
                : 'No GPS-correlated observation has reached the phone yet.'}
            </Text>
          </Pressable>
        );
      })}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  scroll: {flex: 1}, content: {gap: theme.spacing.sm, paddingTop: 2, paddingBottom: theme.spacing.xxl},
  body: {color: theme.colors.textSoft, fontSize: 13, lineHeight: 19},
  search: {height: 48, borderRadius: theme.radius.md, borderColor: theme.colors.panelEdgeBright, borderWidth: 1, backgroundColor: theme.colors.bg, color: theme.colors.text, paddingHorizontal: 14, fontSize: 14},
  refresh: {alignSelf: 'flex-start', paddingVertical: 7}, refreshText: {color: theme.colors.amber, fontSize: 12, fontWeight: '700', textTransform: 'uppercase', letterSpacing: 0.6},
  target: {backgroundColor: theme.colors.panel, borderColor: theme.colors.panelEdge, borderWidth: 1, borderRadius: theme.radius.lg, padding: theme.spacing.md, gap: 11},
  targetSelected: {borderColor: theme.colors.amber}, targetHeader: {flexDirection: 'row', gap: 10, alignItems: 'flex-start'}, targetCopy: {flex: 1, gap: 3},
  targetName: {color: theme.colors.textStrong, fontSize: 17, fontWeight: '800'}, identity: {color: theme.colors.textDim, fontSize: 10, fontFamily: theme.fonts.label},
  signalBlock: {alignItems: 'flex-end', gap: 2}, rssi: {color: theme.colors.cyan, fontSize: 13, fontWeight: '800'}, age: {color: theme.colors.textDim, fontSize: 10},
  badges: {flexDirection: 'row', flexWrap: 'wrap', gap: 6}, badge: {color: theme.colors.textSoft, backgroundColor: theme.colors.bgAlt, borderColor: theme.colors.panelEdge, borderWidth: 1, borderRadius: 999, paddingHorizontal: 8, paddingVertical: 4, fontSize: 9, textTransform: 'uppercase'},
  goodBadge: {color: theme.colors.lime, borderColor: theme.colors.limeDim}, midBadge: {color: theme.colors.amber, borderColor: theme.colors.amberDim},
  detail: {color: theme.colors.textSoft, fontSize: 12, lineHeight: 17},
});
