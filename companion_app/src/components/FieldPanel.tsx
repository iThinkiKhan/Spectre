import React from 'react';
import {StyleSheet, Text, View, type ViewStyle} from 'react-native';

import {theme} from '../theme/theme';

type Tone = 'default' | 'accent' | 'warn' | 'success' | 'danger';

type Props = {
  title: string;
  eyebrow?: string;
  tone?: Tone;
  children: React.ReactNode;
  style?: ViewStyle;
  action?: React.ReactNode;
};

const toneColors: Record<Tone, string> = {
  default: theme.colors.panelEdge,
  accent: theme.colors.cyan,
  warn: theme.colors.amberDim,
  success: theme.colors.lime,
  danger: theme.colors.red,
};

export function FieldPanel({
  title,
  eyebrow,
  tone = 'default',
  children,
  style,
  action,
}: Props) {
  const accentColor = toneColors[tone];

  return (
    <View style={[styles.panel, {borderColor: accentColor}, style]}>
      <View style={[styles.glow, {backgroundColor: accentColor}]} />
      <View style={[styles.rule, {backgroundColor: accentColor}]} />
      <View style={styles.header}>
        <View style={styles.headerText}>
          {!!eyebrow && <Text style={styles.eyebrow}>{eyebrow}</Text>}
          <Text style={styles.title}>{title}</Text>
        </View>
        {action}
      </View>
      <View style={styles.body}>{children}</View>
    </View>
  );
}

const styles = StyleSheet.create({
  panel: {
    overflow: 'hidden',
    backgroundColor: theme.colors.panel,
    borderWidth: 1,
    borderRadius: theme.radius.lg,
    paddingHorizontal: theme.spacing.md,
    paddingTop: 14,
    paddingBottom: 14,
    gap: 12,
  },
  glow: {
    position: 'absolute',
    top: -24,
    right: -20,
    width: 120,
    height: 72,
    borderRadius: 999,
    opacity: 0.08,
  },
  rule: {
    position: 'absolute',
    top: 0,
    left: 0,
    right: 0,
    height: 2,
    opacity: 0.8,
  },
  header: {
    flexDirection: 'row',
    alignItems: 'flex-start',
    justifyContent: 'space-between',
    gap: theme.spacing.sm,
  },
  headerText: {
    flex: 1,
    gap: 2,
  },
  eyebrow: {
    color: theme.colors.textSoft,
    fontSize: 10,
    letterSpacing: 1.2,
    textTransform: 'uppercase',
    fontFamily: theme.fonts.label,
  },
  title: {
    color: theme.colors.textStrong,
    fontSize: 17,
    fontWeight: '700',
    letterSpacing: 0.6,
    textTransform: 'uppercase',
    fontFamily: theme.fonts.title,
  },
  body: {
    gap: 12,
  },
});
