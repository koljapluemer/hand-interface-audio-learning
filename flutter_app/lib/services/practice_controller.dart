import 'dart:async';

import 'package:flutter/foundation.dart';

import '../models/sentence_card.dart';
import 'audio_service.dart';
import 'ble_button_service.dart';
import 'review_store.dart';
import 'sentence_library.dart';

/// Clean two-state practice machine.
///
/// * [hidden]   — front (Italian) has been auto-played; back is still hidden.
///     - button 5 → replay front
///     - button 3 → reveal: play back (English), go to [revealed]
/// * [revealed] — both languages are known.
///     - button 3 → replay front then back
///     - button 5 → score **Wrong**, schedule with FSRS, advance
///     - button 4 → score **Correct**, schedule with FSRS, advance
///
/// "Advance" picks the next card (a never-practised one or one due per FSRS),
/// returns to [hidden] and auto-plays its front.
enum Phase { idle, hidden, revealed }

class PracticeController extends ChangeNotifier {
  PracticeController({
    required this.library,
    required this.reviews,
    required this.audio,
    required this.ble,
  });

  final SentenceLibrary library;
  final ReviewStore reviews;
  final AudioService audio;
  final BleButtonService ble;

  Phase phase = Phase.idle;
  SentenceCard? current;
  bool audioBusy = false;
  int lastButton = 0;

  int _playToken = 0;
  bool _transitioning = false;
  StreamSubscription<int>? _btnSub;

  void start() {
    _btnSub = ble.buttonEvents.listen(injectButton);
    library.addListener(_onLibraryChanged);
    if (library.cards.isNotEmpty) _advance();
  }

  void _onLibraryChanged() {
    if (phase == Phase.idle && library.cards.isNotEmpty && !_transitioning) {
      _advance();
    }
  }

  /// Feed a button number (3/4/5) into the machine. Also called by the on-device
  /// number keys so the app is testable without the hardware remote.
  void injectButton(int button) {
    lastButton = button;
    notifyListeners();
    if (_transitioning) return;
    switch (phase) {
      case Phase.idle:
        break;
      case Phase.hidden:
        if (button == 5) _play([_front()]);
        if (button == 3) _reveal();
        break;
      case Phase.revealed:
        if (button == 3) _play([_front(), _back()]);
        if (button == 5) _grade(correct: false);
        if (button == 4) _grade(correct: true);
        break;
    }
  }

  Future<void> _reveal() async {
    phase = Phase.revealed;
    notifyListeners();
    await _play([_back()]);
  }

  Future<void> _grade({required bool correct}) async {
    final card = current;
    if (card == null || _transitioning) return;
    _transitioning = true;
    try {
      await reviews.grade(card.id, correct: correct);
    } finally {
      _transitioning = false;
    }
    await _advance();
  }

  Future<void> _advance() async {
    final next = reviews.pickNext(library.cards, excludeId: current?.id);
    current = next;
    phase = next == null ? Phase.idle : Phase.hidden;
    notifyListeners();
    if (next != null) await _play([_front()]);
  }

  Future<void> _play(List<Clip> clips) async {
    final token = ++_playToken;
    audioBusy = true;
    notifyListeners();
    try {
      await audio.play(clips);
    } catch (_) {
      // A newer play()/stop() superseded this one, or the file was unreadable.
    }
    if (token == _playToken) {
      audioBusy = false;
      notifyListeners();
    }
  }

  Clip _front() => Clip(current!.frontPath, 'IT · ${current!.id}');
  Clip _back() => Clip(current!.backPath, 'EN · ${current!.id}');

  @override
  void dispose() {
    _btnSub?.cancel();
    library.removeListener(_onLibraryChanged);
    super.dispose();
  }
}
