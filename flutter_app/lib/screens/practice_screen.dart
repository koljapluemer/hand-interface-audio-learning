import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../services/ble_button_service.dart';
import '../services/practice_controller.dart';
import '../services/review_store.dart';
import '../services/sentence_library.dart';
import 'setup_screen.dart';

class PracticeScreen extends StatefulWidget {
  const PracticeScreen({
    super.key,
    required this.controller,
    required this.library,
    required this.reviews,
    required this.ble,
  });

  final PracticeController controller;
  final SentenceLibrary library;
  final ReviewStore reviews;
  final BleButtonService ble;

  @override
  State<PracticeScreen> createState() => _PracticeScreenState();
}

class _PracticeScreenState extends State<PracticeScreen> {
  final _focus = FocusNode();

  @override
  void dispose() {
    _focus.dispose();
    super.dispose();
  }

  // Physical number keys 3/4/5 mirror the ESP remote for on-desk testing.
  void _onKey(KeyEvent e) {
    if (e is! KeyDownEvent) return;
    final k = e.logicalKey;
    final b = (k == LogicalKeyboardKey.digit3 || k == LogicalKeyboardKey.numpad3)
        ? 3
        : (k == LogicalKeyboardKey.digit4 || k == LogicalKeyboardKey.numpad4)
            ? 4
            : (k == LogicalKeyboardKey.digit5 || k == LogicalKeyboardKey.numpad5)
                ? 5
                : 0;
    if (b != 0) widget.controller.injectButton(b);
  }

  @override
  Widget build(BuildContext context) {
    return KeyboardListener(
      focusNode: _focus,
      autofocus: true,
      onKeyEvent: _onKey,
      child: Scaffold(
        appBar: AppBar(
          title: const Text('Sentences'),
          actions: [
            IconButton(
              icon: const Icon(Icons.settings),
              onPressed: () => Navigator.of(context).push(
                MaterialPageRoute(
                  builder: (_) =>
                      SetupScreen(library: widget.library, ble: widget.ble),
                ),
              ),
            ),
          ],
        ),
        body: AnimatedBuilder(
          animation: Listenable.merge(
            [widget.controller, widget.reviews, widget.ble],
          ),
          builder: (context, _) => _body(context),
        ),
      ),
    );
  }

  Widget _body(BuildContext context) {
    final theme = Theme.of(context);
    final c = widget.controller;
    final cards = widget.library.cards;

    final phaseText = switch (c.phase) {
      Phase.idle => 'Nothing new or due right now',
      Phase.hidden => 'Listen — recall the meaning',
      Phase.revealed => 'Revealed — score yourself',
    };

    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          _BleChip(ble: widget.ble),
          const Spacer(),
          Center(
            child: Text(
              c.current?.id ?? '—',
              style: theme.textTheme.displayLarge,
            ),
          ),
          const SizedBox(height: 12),
          Center(
            child: Text(
              c.phase == Phase.revealed ? 'IT + EN' : 'IT',
              style: theme.textTheme.titleMedium
                  ?.copyWith(color: theme.colorScheme.primary),
            ),
          ),
          const SizedBox(height: 8),
          Center(child: Text(phaseText, style: theme.textTheme.bodyMedium)),
          const SizedBox(height: 12),
          Center(
            child: Opacity(
              opacity: c.audioBusy ? 1 : 0,
              child: const SizedBox(
                width: 120,
                child: LinearProgressIndicator(),
              ),
            ),
          ),
          const Spacer(),
          Text(
            'new ${widget.reviews.newCount(cards)}  ·  '
            'due ${widget.reviews.dueCount(cards)}  ·  '
            'total ${cards.length}',
            textAlign: TextAlign.center,
            style: theme.textTheme.bodySmall,
          ),
          const SizedBox(height: 16),
          _Legend(phase: c.phase, lastButton: c.lastButton),
        ],
      ),
    );
  }
}

class _BleChip extends StatelessWidget {
  const _BleChip({required this.ble});

  final BleButtonService ble;

  @override
  Widget build(BuildContext context) {
    final connected = ble.status == BleStatus.connected;
    final label = switch (ble.status) {
      BleStatus.connected => 'remote connected',
      BleStatus.connecting => 'remote connecting…',
      BleStatus.scanning => 'scanning…',
      BleStatus.off => 'bluetooth off',
      BleStatus.unsupported => 'no bluetooth',
      BleStatus.disconnected => 'remote not connected',
    };
    return Align(
      alignment: Alignment.centerLeft,
      child: Chip(
        avatar: Icon(
          connected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
          size: 18,
        ),
        label: Text(label),
        visualDensity: VisualDensity.compact,
      ),
    );
  }
}

class _Legend extends StatelessWidget {
  const _Legend({required this.phase, required this.lastButton});

  final Phase phase;
  final int lastButton;

  @override
  Widget build(BuildContext context) {
    final rows = switch (phase) {
      Phase.revealed => const [
          ('3', 'replay IT then EN'),
          ('5', 'Wrong'),
          ('4', 'Correct'),
        ],
      _ => const [
          ('5', 'replay IT'),
          ('3', 'reveal (play EN)'),
        ],
    };
    final theme = Theme.of(context);
    return Column(
      children: [
        for (final (btn, desc) in rows)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 2),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                CircleAvatar(
                  radius: 12,
                  backgroundColor: lastButton.toString() == btn
                      ? theme.colorScheme.primary
                      : theme.colorScheme.surfaceContainerHighest,
                  foregroundColor: lastButton.toString() == btn
                      ? theme.colorScheme.onPrimary
                      : theme.colorScheme.onSurface,
                  child: Text(btn, style: const TextStyle(fontSize: 12)),
                ),
                const SizedBox(width: 8),
                Text(desc, style: theme.textTheme.bodyMedium),
              ],
            ),
          ),
      ],
    );
  }
}
