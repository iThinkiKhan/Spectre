import React from 'react';
import {
  Pressable,
  ScrollView,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  View,
} from 'react-native';

import {FieldPanel} from '../components/FieldPanel';
import {eventTimestampToUnixMs} from '../protocol/binary';
import {useSpectre} from '../state/SpectreContext';
import {theme} from '../theme/theme';

function eventTypeLabel(type: number) {
  switch (type) {
    case 1:
      return 'Probe';
    case 2:
      return 'Device';
    case 3:
      return 'Drone';
    case 4:
      return 'PMKID';
    case 5:
      return 'Event';
    default:
      return 'Custom';
  }
}

function eventLaneLabel(lane: number) {
  return lane === 0 ? 'MISSION' : 'NOISE';
}

function eventPriorityLabel(priority: number) {
  return `P${priority}`;
}

function eventTimeLabel(timestamp: number) {
  const unixMs = eventTimestampToUnixMs(timestamp);
  return unixMs ? new Date(unixMs).toLocaleTimeString() : 'pending time';
}

export function EnrichScreen() {
  const spectre = useSpectre();

  return (
    <ScrollView
      style={styles.scroll}
      contentContainerStyle={styles.content}
      showsVerticalScrollIndicator={false}>
      <FieldPanel
        title="Prompt Workflow"
        eyebrow="Priority"
        tone={
          spectre.promptState.awaitingReply
            ? 'accent'
            : spectre.promptState.submittedReply
              ? 'success'
              : 'default'
        }>
        <Text style={styles.promptLabel}>Prompt</Text>
        <Text style={styles.promptText}>
          {spectre.promptState.promptText ||
            'No active text prompt. Spectre will raise the overlay when it needs input.'}
        </Text>
        <View style={styles.promptMetaRow}>
          <View style={styles.metaChip}>
            <Text style={styles.metaChipLabel}>Receipt</Text>
            <Text style={styles.metaChipValue}>
              {spectre.promptState.receipt || 'IDLE'}
            </Text>
          </View>
          <View style={styles.metaChip}>
            <Text style={styles.metaChipLabel}>Token</Text>
            <Text style={styles.metaChipValue}>
              {spectre.statusSummary.token ?? '--'}
            </Text>
          </View>
          <View style={styles.metaChip}>
            <Text style={styles.metaChipLabel}>Flow</Text>
            <Text style={styles.metaChipValue}>
              {spectre.promptState.awaitingReply
                ? 'AWAITING'
                : spectre.promptState.submittedReply
                  ? 'SENT'
                  : 'IDLE'}
            </Text>
          </View>
          <View style={styles.metaChip}>
            <Text style={styles.metaChipLabel}>Input state</Text>
            <Text style={styles.metaChipValue}>
              {spectre.statusSummary.inputState || 'IDLE'}
            </Text>
          </View>
        </View>
        {!!spectre.promptState.replyError && (
          <Text style={styles.errorText}>{spectre.promptState.replyError}</Text>
        )}
      </FieldPanel>

      <FieldPanel title="Enrichment Defaults" eyebrow="Fast path">
        <Text style={styles.bodyText}>
          The phone is a field-side enrichment relay. Keep the tag tight and the active location trustworthy, then let new batches auto-apply whenever possible.
        </Text>

        <Text style={styles.inputLabel}>Active tag</Text>
        <TextInput
          value={spectre.activeTag}
          onChangeText={spectre.setActiveTag}
          style={styles.input}
          placeholder="FIELD"
          placeholderTextColor={theme.colors.textDim}
        />

        <View style={styles.switchRow}>
          <View style={styles.switchText}>
            <Text style={styles.switchTitle}>Auto-apply incoming batches</Text>
            <Text style={styles.switchBody}>
              New event batches are pushed back to Spectre as soon as a usable location exists.
            </Text>
          </View>
          <Switch
            value={spectre.autoApplyEnrichment}
            onValueChange={spectre.setAutoApplyEnrichment}
            trackColor={{
              false: theme.colors.panelEdge,
              true: theme.colors.cyanDim,
            }}
            thumbColor={theme.colors.text}
          />
        </View>

        <View style={styles.locationBox}>
          <Text style={styles.locationTitle}>Active location source</Text>
          <Text style={styles.locationBody}>
            {spectre.activeLocation
              ? `${spectre.activeLocation.source.toUpperCase()} · ${spectre.activeLocation.lat.toFixed(5)}, ${spectre.activeLocation.lon.toFixed(5)}`
              : 'No active fix. Batches will wait until you refresh device location or arm a manual field fix.'}
          </Text>
        </View>
      </FieldPanel>

      <FieldPanel title="Batch Queue" eyebrow="Event enrichment">
        {spectre.eventBatches.length === 0 ? (
          <Text style={styles.bodyText}>
            No event batches yet. When Spectre writes a pending event batch to the phone companion service, it will appear here.
          </Text>
        ) : (
          spectre.eventBatches.map(batch => (
            <View key={batch.id} style={styles.batchCard}>
              <View style={styles.batchHeader}>
                <View style={styles.batchTitleWrap}>
                  <Text style={styles.batchTitle}>
                    {batch.source === 'mock' ? 'Mock batch' : 'Spectre batch'}
                  </Text>
                  <Text style={styles.batchMeta}>
                    {batch.events.length} events · {new Date(batch.receivedAt).toLocaleTimeString()}
                  </Text>
                </View>
                <View
                  style={[
                    styles.batchStatus,
                    batch.status === 'sent'
                      ? styles.batchStatusSent
                      : batch.status === 'ready'
                        ? styles.batchStatusReady
                        : styles.batchStatusWaiting,
                  ]}>
                  <Text style={styles.batchStatusText}>{batch.status}</Text>
                </View>
              </View>

              <Text style={styles.batchNote}>{batch.note}</Text>

              {batch.events.map(event => (
                <View key={`${batch.id}-${event.eventId}`} style={styles.eventRow}>
                  <Text style={styles.eventType}>{eventTypeLabel(event.type)}</Text>
                  <Text style={styles.eventMeta}>#{event.eventId}</Text>
                  <Text style={styles.eventMeta}>{eventLaneLabel(event.lane)}</Text>
                  <Text style={styles.eventMeta}>{eventPriorityLabel(event.priority)}</Text>
                  <Text style={styles.eventMeta}>{eventTimeLabel(event.timestampMs)}</Text>
                </View>
              ))}

              <View style={styles.batchFooter}>
                <Text style={styles.batchFooterText}>
                  Tag: {batch.tag || 'none'} · Location:{' '}
                  {batch.location
                    ? `${batch.location.lat.toFixed(4)}, ${batch.location.lon.toFixed(4)}`
                    : 'waiting'}
                </Text>
                <Pressable
                  style={styles.primaryButton}
                  onPress={() => spectre.sendBatchNow(batch.id)}>
                  <Text style={styles.primaryText}>Push now</Text>
                </Pressable>
              </View>
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
  promptLabel: {
    color: theme.colors.textDim,
    fontSize: 11,
    textTransform: 'uppercase',
    letterSpacing: 1,
    fontFamily: theme.fonts.label,
  },
  promptText: {
    color: theme.colors.text,
    fontSize: 18,
    lineHeight: 24,
    fontWeight: '700',
  },
  promptMetaRow: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: theme.spacing.sm,
  },
  metaChip: {
    minWidth: 90,
    backgroundColor: theme.colors.bgAlt,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    padding: theme.spacing.sm,
    gap: 2,
  },
  metaChipLabel: {
    color: theme.colors.textDim,
    fontSize: 10,
    textTransform: 'uppercase',
    letterSpacing: 0.8,
    fontFamily: theme.fonts.label,
  },
  metaChipValue: {
    color: theme.colors.text,
    fontSize: 13,
    fontWeight: '700',
  },
  errorText: {
    color: theme.colors.red,
    fontSize: 12,
    lineHeight: 17,
  },
  bodyText: {
    color: theme.colors.textSoft,
    fontSize: 14,
    lineHeight: 20,
  },
  inputLabel: {
    color: theme.colors.textDim,
    fontSize: 11,
    textTransform: 'uppercase',
    letterSpacing: 1,
    fontFamily: theme.fonts.label,
  },
  input: {
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    backgroundColor: theme.colors.bgAlt,
    color: theme.colors.text,
    paddingHorizontal: theme.spacing.md,
    paddingVertical: 12,
    fontSize: 16,
    fontFamily: theme.fonts.body,
  },
  switchRow: {
    flexDirection: 'row',
    gap: theme.spacing.md,
    alignItems: 'center',
  },
  switchText: {
    flex: 1,
    gap: 4,
  },
  switchTitle: {
    color: theme.colors.text,
    fontSize: 15,
    fontWeight: '700',
  },
  switchBody: {
    color: theme.colors.textSoft,
    fontSize: 13,
    lineHeight: 18,
  },
  locationBox: {
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    backgroundColor: theme.colors.bgAlt,
    padding: theme.spacing.md,
    gap: theme.spacing.xs,
  },
  locationTitle: {
    color: theme.colors.textDim,
    fontSize: 11,
    textTransform: 'uppercase',
    letterSpacing: 1,
    fontFamily: theme.fonts.label,
  },
  locationBody: {
    color: theme.colors.text,
    fontSize: 14,
    lineHeight: 20,
  },
  batchCard: {
    backgroundColor: theme.colors.bgAlt,
    borderColor: theme.colors.panelEdge,
    borderWidth: 1,
    borderRadius: theme.radius.md,
    padding: theme.spacing.md,
    gap: theme.spacing.sm,
  },
  batchHeader: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    gap: theme.spacing.sm,
  },
  batchTitleWrap: {
    flex: 1,
    gap: 2,
  },
  batchTitle: {
    color: theme.colors.text,
    fontSize: 16,
    fontWeight: '700',
  },
  batchMeta: {
    color: theme.colors.textDim,
    fontSize: 12,
  },
  batchStatus: {
    alignSelf: 'flex-start',
    borderRadius: theme.radius.pill,
    paddingHorizontal: 10,
    paddingVertical: 5,
  },
  batchStatusWaiting: {
    backgroundColor: theme.colors.panelEdge,
  },
  batchStatusReady: {
    backgroundColor: theme.colors.amberDim,
  },
  batchStatusSent: {
    backgroundColor: theme.colors.limeDim,
  },
  batchStatusText: {
    color: theme.colors.textStrong,
    fontSize: 11,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.8,
    fontFamily: theme.fonts.label,
  },
  batchNote: {
    color: theme.colors.textSoft,
    fontSize: 13,
    lineHeight: 18,
  },
  eventRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: theme.spacing.sm,
  },
  eventType: {
    color: theme.colors.amber,
    fontSize: 12,
    fontWeight: '700',
    minWidth: 56,
  },
  eventMeta: {
    color: theme.colors.textSoft,
    fontSize: 12,
  },
  batchFooter: {
    gap: theme.spacing.sm,
  },
  batchFooterText: {
    color: theme.colors.textSoft,
    fontSize: 12,
    lineHeight: 18,
  },
  primaryButton: {
    alignSelf: 'flex-start',
    backgroundColor: theme.colors.amber,
    borderRadius: theme.radius.md,
    paddingHorizontal: theme.spacing.md,
    paddingVertical: 10,
  },
  primaryText: {
    color: theme.colors.textOnAccent,
    fontSize: 13,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 0.8,
    fontFamily: theme.fonts.label,
  },
});
