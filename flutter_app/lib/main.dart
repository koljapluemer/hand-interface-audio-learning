import 'dart:io';

import 'package:flutter/material.dart';
import 'package:just_audio_background/just_audio_background.dart';
import 'package:permission_handler/permission_handler.dart';

import 'services/audio_service.dart';
import 'services/ble_button_service.dart';
import 'services/practice_controller.dart';
import 'services/review_store.dart';
import 'services/sentence_library.dart';
import 'screens/practice_screen.dart';
import 'screens/setup_screen.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await JustAudioBackground.init(
    androidNotificationChannelId: 'com.example.sentences.playback',
    androidNotificationChannelName: 'Sentence playback',
    androidNotificationOngoing: true,
  );
  runApp(const SentencesApp());
}

class SentencesApp extends StatefulWidget {
  const SentencesApp({super.key});

  @override
  State<SentencesApp> createState() => _SentencesAppState();
}

class _SentencesAppState extends State<SentencesApp> {
  final _library = SentenceLibrary();
  final _reviews = ReviewStore();
  final _audio = AudioService();
  final _ble = BleButtonService();
  late final _controller = PracticeController(
    library: _library,
    reviews: _reviews,
    audio: _audio,
    ble: _ble,
  );

  bool _ready = false;

  @override
  void initState() {
    super.initState();
    _library.addListener(_refresh);
    _bootstrap();
  }

  @override
  void dispose() {
    _library.removeListener(_refresh);
    _controller.dispose();
    _ble.dispose();
    _audio.dispose();
    _reviews.dispose();
    _library.dispose();
    super.dispose();
  }

  void _refresh() => setState(() {});

  Future<void> _bootstrap() async {
    if (Platform.isAndroid) {
      await [
        Permission.notification,
        Permission.bluetoothScan,
        Permission.bluetoothConnect,
      ].request();
      // Full-disk read so the sentence folder can live anywhere on the device.
      await Permission.manageExternalStorage.request();
    }
    await _reviews.init();
    await _library.init();
    await _ble.init();
    _controller.start();
    if (mounted) setState(() => _ready = true);
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Sentences',
      theme: ThemeData(colorSchemeSeed: Colors.indigo, useMaterial3: true),
      darkTheme: ThemeData(
        colorSchemeSeed: Colors.indigo,
        brightness: Brightness.dark,
        useMaterial3: true,
      ),
      home: _home(),
    );
  }

  Widget _home() {
    if (!_ready) {
      return const Scaffold(body: Center(child: CircularProgressIndicator()));
    }
    if (_library.folderPath == null || _library.cards.isEmpty) {
      return SetupScreen(library: _library, ble: _ble);
    }
    return PracticeScreen(
      controller: _controller,
      library: _library,
      reviews: _reviews,
      ble: _ble,
    );
  }
}
