import 'dart:convert';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:fsrs/fsrs.dart' as fsrs;
import 'package:shared_preferences/shared_preferences.dart';

import '../models/sentence_card.dart';

const _prefsCardsKey = 'fsrs_cards_v1';

/// Spaced-repetition state for every sentence, keyed by [SentenceCard.id].
/// Wraps the `fsrs` scheduler and persists card state to shared preferences.
class ReviewStore extends ChangeNotifier {
  final fsrs.Scheduler _scheduler = fsrs.Scheduler(desiredRetention: 0.9);
  final Map<String, fsrs.Card> _cards = {};
  final Random _random = Random();

  Future<void> init() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_prefsCardsKey);
    if (raw == null) return;
    try {
      final decoded = jsonDecode(raw) as Map<String, dynamic>;
      _cards.clear();
      decoded.forEach((id, map) {
        _cards[id] = fsrs.Card.fromMap(
          Map<String, dynamic>.from(map as Map),
        );
      });
    } catch (_) {
      // Corrupt state: start fresh rather than crash.
    }
  }

  Future<void> _persist() async {
    final prefs = await SharedPreferences.getInstance();
    final map = _cards.map((id, card) => MapEntry(id, card.toMap()));
    await prefs.setString(_prefsCardsKey, jsonEncode(map));
  }

  bool isNew(String id) => !_cards.containsKey(id);

  DateTime? dueAt(String id) => _cards[id]?.due;

  bool isDue(String id, [DateTime? now]) {
    final due = _cards[id]?.due;
    if (due == null) return false;
    return !due.isAfter(now ?? DateTime.now());
  }

  int newCount(List<SentenceCard> all) => all.where((c) => isNew(c.id)).length;

  int dueCount(List<SentenceCard> all) {
    final now = DateTime.now();
    return all.where((c) => isDue(c.id, now)).length;
  }

  /// Applies a review outcome and reschedules the card.
  Future<void> grade(String id, {required bool correct}) async {
    final rating = correct ? fsrs.Rating.good : fsrs.Rating.again;
    final card = _cards[id] ?? fsrs.Card(cardId: _stableCardId(id));
    final result = _scheduler.reviewCard(card, rating);
    _cards[id] = result.card;
    await _persist();
    notifyListeners();
  }

  /// Chooses the next sentence to practise: a never-seen card or one that is
  /// due, at random. Falls back to whichever card comes due soonest. Avoids
  /// repeating [excludeId] unless it is the only option.
  SentenceCard? pickNext(List<SentenceCard> all, {String? excludeId}) {
    if (all.isEmpty) return null;
    final now = DateTime.now();

    final pool = all
        .where((c) => isNew(c.id) || isDue(c.id, now))
        .toList();
    _dropExcluded(pool, excludeId);
    if (pool.isNotEmpty) return pool[_random.nextInt(pool.length)];

    final byDue = [...all]
      ..sort((a, b) => (dueAt(a.id) ?? now).compareTo(dueAt(b.id) ?? now));
    _dropExcluded(byDue, excludeId);
    return byDue.first;
  }

  void _dropExcluded(List<SentenceCard> list, String? excludeId) {
    if (excludeId != null && list.length > 1) {
      list.removeWhere((c) => c.id == excludeId);
    }
  }

  int _stableCardId(String id) =>
      int.tryParse(id) ?? id.codeUnits.fold(0, (a, b) => (a * 31 + b) & 0x7fffffff);
}
