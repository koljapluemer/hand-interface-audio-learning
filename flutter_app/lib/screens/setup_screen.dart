import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:permission_handler/permission_handler.dart';

import '../services/ble_button_service.dart';
import '../services/sentence_library.dart';

class SetupScreen extends StatefulWidget {
  const SetupScreen({super.key, required this.library, required this.ble});

  final SentenceLibrary library;
  final BleButtonService ble;

  @override
  State<SetupScreen> createState() => _SetupScreenState();
}

class _SetupScreenState extends State<SetupScreen> {
  bool _busy = false;

  Future<void> _pickFolder() async {
    setState(() => _busy = true);
    try {
      final path = await FilePicker.platform.getDirectoryPath();
      if (path != null) await widget.library.setFolder(path);
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final lib = widget.library;
    final theme = Theme.of(context);

    return Scaffold(
      appBar: AppBar(title: const Text('Setup')),
      body: AnimatedBuilder(
        animation: Listenable.merge([lib, widget.ble]),
        builder: (context, _) => ListView(
          padding: const EdgeInsets.all(16),
          children: [
            Text('Sentence folder', style: theme.textTheme.titleMedium),
            const SizedBox(height: 8),
            Text(lib.folderPath ?? 'Not set'),
            const SizedBox(height: 4),
            Text('${lib.count} audio pairs', style: theme.textTheme.bodySmall),
            if (lib.error != null) ...[
              const SizedBox(height: 8),
              Text(lib.error!, style: TextStyle(color: theme.colorScheme.error)),
            ],
            const SizedBox(height: 12),
            Wrap(
              spacing: 12,
              children: [
                FilledButton(
                  onPressed: _busy ? null : _pickFolder,
                  child: Text(_busy ? 'Loading…' : 'Choose folder'),
                ),
                OutlinedButton(
                  onPressed: lib.folderPath == null || lib.isLoading
                      ? null
                      : lib.rescan,
                  child: const Text('Rescan'),
                ),
              ],
            ),
            if (Platform.isAndroid) ...[
              const SizedBox(height: 28),
              Text('Storage access', style: theme.textTheme.titleMedium),
              const SizedBox(height: 8),
              const Text(
                'Full-disk read is needed so the sentence folder can live '
                'anywhere on the device.',
              ),
              const SizedBox(height: 8),
              OutlinedButton(
                onPressed: () => Permission.manageExternalStorage.request(),
                child: const Text('Grant full-disk access'),
              ),
            ],
            const SizedBox(height: 28),
            Text('Button remote (BLE)', style: theme.textTheme.titleMedium),
            const SizedBox(height: 8),
            _BleSection(ble: widget.ble),
            const SizedBox(height: 24),
            Text(
              widget.library.cards.isEmpty
                  ? 'Pick a folder with <stem>_it / <stem>_en audio pairs to '
                      'start.'
                  : 'Ready — go back to practise.',
              style: theme.textTheme.bodySmall,
            ),
          ],
        ),
      ),
    );
  }
}

class _BleSection extends StatelessWidget {
  const _BleSection({required this.ble});

  final BleButtonService ble;

  String get _statusText => switch (ble.status) {
        BleStatus.unsupported => 'Bluetooth not supported on this device',
        BleStatus.off => 'Bluetooth is off',
        BleStatus.disconnected => 'Not connected',
        BleStatus.scanning => 'Scanning…',
        BleStatus.connecting => 'Connecting…',
        BleStatus.connected => 'Connected to ${ble.connectedName ?? 'remote'}',
      };

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scanning = ble.status == BleStatus.scanning;

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(_statusText),
        if (ble.error != null) ...[
          const SizedBox(height: 4),
          Text(ble.error!, style: TextStyle(color: theme.colorScheme.error)),
        ],
        const SizedBox(height: 8),
        Wrap(
          spacing: 12,
          children: [
            FilledButton.tonal(
              onPressed: ble.status == BleStatus.unsupported
                  ? null
                  : () => scanning ? ble.stopScan() : ble.startScan(),
              child: Text(scanning ? 'Stop scan' : 'Scan'),
            ),
            if (ble.savedDeviceId != null)
              OutlinedButton(
                onPressed: ble.forget,
                child: const Text('Forget device'),
              ),
          ],
        ),
        for (final r in ble.scanResults)
          ListTile(
            dense: true,
            contentPadding: EdgeInsets.zero,
            title: Text(
              r.device.platformName.isNotEmpty
                  ? r.device.platformName
                  : r.advertisementData.advName.isNotEmpty
                      ? r.advertisementData.advName
                      : '(unnamed)',
            ),
            subtitle: Text(r.device.remoteId.str),
            trailing: const Icon(Icons.link),
            onTap: () => ble.connectTo(r.device),
          ),
      ],
    );
  }
}
