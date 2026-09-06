import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../models/sentence_card.dart';

const _prefsFolderKey = 'sentences_folder';

/// Owns the chosen folder and the list of [SentenceCard]s scanned from it.
class SentenceLibrary extends ChangeNotifier {
  String? _folderPath;
  bool _loading = false;
  String? _error;
  List<SentenceCard> _cards = [];

  String? get folderPath => _folderPath;
  bool get isLoading => _loading;
  String? get error => _error;
  List<SentenceCard> get cards => List.unmodifiable(_cards);
  int get count => _cards.length;

  Future<void> init() async {
    final prefs = await SharedPreferences.getInstance();
    _folderPath = prefs.getString(_prefsFolderKey);
    if (_folderPath != null) await rescan();
  }

  Future<void> setFolder(String path) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_prefsFolderKey, path);
    _folderPath = path;
    await rescan();
  }

  Future<void> rescan() async {
    final path = _folderPath;
    if (path == null) return;
    _loading = true;
    _error = null;
    notifyListeners();
    try {
      final parsed = await compute(scanSentenceFolderIsolate, path);
      _cards = [
        for (final e in parsed)
          SentenceCard(
            id: e['id']!,
            frontPath: e['front']!,
            backPath: e['back']!,
          ),
      ];
      if (_cards.isEmpty) {
        _error = 'No "<name>_$kFrontSuffix" / "<name>_$kBackSuffix" audio '
            'pairs found in this folder.';
      }
    } catch (e) {
      _error = e.toString();
      _cards = [];
    }
    _loading = false;
    notifyListeners();
  }
}
