import React from 'react';
import {StyleSheet, Text, View} from 'react-native';
import {theme} from '../theme/theme';

type Props = {
  label: string;
  value: string;
  detail?: string;
};

export function StatusCard({label, value, detail}: Props) {
  return (
    <View style={styles.card}>
      <Text style={styles.label}>{label}</Text>
      <Text style={styles.value}>{value}</Text>
      {detail ? <Text style={styles.detail}>{detail}</Text> : null}
    </View>
  );
}

const styles = StyleSheet.create({
  card: {
    backgroundColor: theme.colors.panel,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    padding: theme.spacing.md,
    gap: theme.spacing.xs,
  },
  label: {
    color: theme.colors.amber,
    fontSize: 12,
    fontWeight: '700',
    letterSpacing: 1,
  },
  value: {
    color: theme.colors.text,
    fontSize: 20,
    fontWeight: '700',
  },
  detail: {
    color: theme.colors.textDim,
    fontSize: 13,
  },
});