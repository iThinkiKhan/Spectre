import React from 'react';
import {Pressable, StyleSheet, Text, View} from 'react-native';

import {theme} from '../theme/theme';

type TabKey = 'link' | 'enrich' | 'ops';

type Props = {
  activeTab: TabKey;
  onSelect: (tab: TabKey) => void;
};

const tabs: Array<{key: TabKey; label: string}> = [
  {key: 'link', label: 'Link'},
  {key: 'enrich', label: 'Enrich'},
  {key: 'ops', label: 'Vault'},
];

export function BottomTabs({activeTab, onSelect}: Props) {
  return (
    <View style={styles.bar}>
      {tabs.map(tab => {
        const active = tab.key === activeTab;
        return (
          <Pressable
            key={tab.key}
            onPress={() => onSelect(tab.key)}
            style={[styles.tab, active ? styles.tabActive : null]}>
            <Text style={[styles.label, active ? styles.labelActive : null]}>
              {tab.label}
            </Text>
          </Pressable>
        );
      })}
    </View>
  );
}

const styles = StyleSheet.create({
  bar: {
    flexDirection: 'row',
    backgroundColor: theme.colors.panelAlt,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.lg,
    padding: 5,
    gap: 6,
  },
  tab: {
    flex: 1,
    alignItems: 'center',
    justifyContent: 'center',
    borderRadius: theme.radius.md,
    borderWidth: 1,
    borderColor: theme.colors.panelEdge,
    backgroundColor: theme.colors.bg,
    paddingVertical: 10,
    paddingHorizontal: theme.spacing.sm,
  },
  tabActive: {
    backgroundColor: theme.colors.amber,
    borderColor: theme.colors.amber,
  },
  label: {
    color: theme.colors.textDim,
    fontSize: 12,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 1,
    fontFamily: theme.fonts.label,
  },
  labelActive: {
    color: theme.colors.textOnAccent,
  },
});
