import 'dart:io';

import 'package:path/path.dart' as p;

/// One practice item: a pair of audio files sharing a numeric stem in the
/// chosen folder, e.g. `03_it.mp3` (front, Italian) and `03_en.mp3` (back,
/// English).
class SentenceCard {
  const SentenceCard({
    required this.id,
    required this.frontPath,
    required this.backPath,
  });

  /// Stable identifier, taken from the shared filename stem (`03`). Used as the
  /// FSRS card key so review history survives folder rescans.
  final String id;

  /// Absolute path to the Italian audio file.
  final String frontPath;

  /// Absolute path to the English audio file.
  final String backPath;

  @override
  String toString() => 'SentenceCard($id)';
}

/// Front/back language suffixes. Files are matched as `<stem>_<suffix>.<ext>`.
const String kFrontSuffix = 'it';
const String kBackSuffix = 'en';

const Set<String> _audioExts = {
  '.mp3', '.m4a', '.aac', '.wav', '.ogg', '.opus', '.flac',
};

/// Scans a flat directory for `<stem>_it.<ext>` / `<stem>_en.<ext>` pairs.
/// Only stems that have *both* halves become cards. Safe to run via `compute`.
List<Map<String, String>> scanSentenceFolderIsolate(String folderPath) {
  final results = <Map<String, String>>[];
  final dir = Directory(folderPath);
  if (!dir.existsSync()) return results;

  final fronts = <String, String>{};
  final backs = <String, String>{};

  for (final entity in dir.listSync()) {
    if (entity is! File) continue;
    final path = entity.path;
    if (!_audioExts.contains(p.extension(path).toLowerCase())) continue;
    final name = p.basenameWithoutExtension(path); // e.g. "03_it"
    final underscore = name.lastIndexOf('_');
    if (underscore <= 0) continue;
    final stem = name.substring(0, underscore);
    switch (name.substring(underscore + 1).toLowerCase()) {
      case kFrontSuffix:
        fronts[stem] = path;
      case kBackSuffix:
        backs[stem] = path;
    }
  }

  final stems = fronts.keys.where(backs.containsKey).toList()
    ..sort(_naturalCompare);
  for (final stem in stems) {
    results.add({'id': stem, 'front': fronts[stem]!, 'back': backs[stem]!});
  }
  return results;
}

int _naturalCompare(String a, String b) {
  final na = int.tryParse(a);
  final nb = int.tryParse(b);
  if (na != null && nb != null) return na.compareTo(nb);
  return a.compareTo(b);
}
