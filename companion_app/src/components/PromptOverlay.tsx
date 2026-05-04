import React, {useEffect, useState} from 'react';
import {
  Modal,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native';
import {theme} from '../theme/theme';

type Props = {
  visible: boolean;
  promptText: string | null;
  promptKind?: 'sessionTag' | 'saveLocation' | 'generic' | 'none';
  receipt: string | null;
  replyError?: string | null;
  status: string | null;
  onSubmit: (text: string) => Promise<void>;
  onDismiss: () => void;
};

function placeholderForPromptKind(
  promptKind: Props['promptKind'],
) {
  switch (promptKind) {
    case 'sessionTag':
      return 'Enter session tag';
    case 'saveLocation':
      return 'Name this location';
    default:
      return 'Enter response';
  }
}

export function PromptOverlay({
  visible,
  promptText,
  promptKind,
  receipt,
  replyError,
  status,
  onSubmit,
  onDismiss,
}: Props) {
  const [value, setValue] = useState('');
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    if (!visible) {
      setValue('');
    }
  }, [visible, promptText]);

  const handleSubmit = async () => {
    if (!value.trim() || busy) {
      return;
    }

    try {
      setBusy(true);
      await onSubmit(value.trim());
      setValue('');
    } finally {
      setBusy(false);
    }
  };

  return (
    <Modal visible={visible} transparent animationType="fade">
      <View style={styles.scrim}>
        <View style={styles.panel}>
          <Text style={styles.title}>SPECTRE INPUT REQUEST</Text>
          <Text style={styles.prompt}>{promptText || 'Awaiting prompt...'}</Text>

          <TextInput
            style={styles.input}
            value={value}
            onChangeText={setValue}
            placeholder={placeholderForPromptKind(promptKind)}
            placeholderTextColor={theme.colors.textDim}
            editable={!busy}
            multiline
            autoFocus
          />

          {!!replyError && <Text style={styles.error}>{replyError}</Text>}
          {!!status && <Text style={styles.meta}>Status: {status}</Text>}
          {!!receipt && <Text style={styles.meta}>Receipt: {receipt}</Text>}

          <View style={styles.row}>
            <TouchableOpacity style={styles.secondary} onPress={onDismiss}>
              <Text style={styles.secondaryText}>HIDE</Text>
            </TouchableOpacity>

            <TouchableOpacity style={styles.primary} onPress={handleSubmit}>
              <Text style={styles.primaryText}>
                {busy ? 'SENDING...' : 'SEND'}
              </Text>
            </TouchableOpacity>
          </View>
        </View>
      </View>
    </Modal>
  );
}

const styles = StyleSheet.create({
  scrim: {
    flex: 1,
    backgroundColor: theme.colors.scrim,
    alignItems: 'center',
    justifyContent: 'center',
    padding: theme.spacing.lg,
  },
  panel: {
    width: '100%',
    backgroundColor: theme.colors.panel,
    borderColor: theme.colors.amber,
    borderWidth: 1,
    borderRadius: theme.radius.lg,
    padding: theme.spacing.lg,
    gap: theme.spacing.md,
  },
  title: {
    color: theme.colors.amber,
    fontSize: 19,
    fontWeight: '700',
    textTransform: 'uppercase',
    letterSpacing: 1,
    fontFamily: theme.fonts.title,
  },
  prompt: {
    color: theme.colors.textStrong,
    fontSize: 15,
    lineHeight: 21,
  },
  input: {
    borderWidth: 1,
    borderColor: theme.colors.cyanDim,
    borderRadius: theme.radius.sm,
    padding: theme.spacing.md,
    color: theme.colors.textStrong,
    backgroundColor: theme.colors.bg,
    minHeight: 120,
    textAlignVertical: 'top',
    fontFamily: theme.fonts.body,
  },
  meta: {
    color: theme.colors.textDim,
    fontSize: 12,
    fontFamily: theme.fonts.label,
  },
  error: {
    color: theme.colors.red,
    fontSize: 12,
    lineHeight: 16,
    fontFamily: theme.fonts.label,
  },
  row: {
    flexDirection: 'row',
    gap: theme.spacing.sm,
    justifyContent: 'flex-end',
  },
  secondary: {
    borderWidth: 1,
    borderColor: theme.colors.panelEdge,
    borderRadius: theme.radius.sm,
    paddingVertical: 10,
    paddingHorizontal: 14,
    backgroundColor: theme.colors.bg,
  },
  secondaryText: {
    color: theme.colors.textSoft,
    fontWeight: '700',
    fontFamily: theme.fonts.label,
  },
  primary: {
    backgroundColor: theme.colors.amber,
    borderRadius: theme.radius.sm,
    paddingVertical: 10,
    paddingHorizontal: 16,
  },
  primaryText: {
    color: theme.colors.textOnAccent,
    fontWeight: '700',
    fontFamily: theme.fonts.label,
  },
});
