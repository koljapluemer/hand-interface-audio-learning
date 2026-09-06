import 'package:just_audio/just_audio.dart';
import 'package:just_audio_background/just_audio_background.dart';

/// A single audio clip to play, with the metadata shown in the media
/// notification.
class Clip {
  const Clip(this.path, this.title);
  final String path;
  final String title;
}

/// Thin wrapper around a single background [AudioPlayer]. Playback keeps going
/// with the screen off thanks to `just_audio_background`'s foreground service.
class AudioService {
  final AudioPlayer _player = AudioPlayer();

  /// Bumped on every [play]/[stop] so a superseded sequence stops touching the
  /// player instead of racing the new one.
  int _generation = 0;

  /// Plays [clips] back to back, awaiting the natural end of each one. A newer
  /// [play] or [stop] call cancels an in-flight sequence.
  Future<void> play(List<Clip> clips) async {
    final gen = ++_generation;
    await _player.stop();
    for (final clip in clips) {
      if (gen != _generation) return;
      await _player.setAudioSource(
        AudioSource.uri(
          Uri.file(clip.path),
          tag: MediaItem(id: clip.path, album: 'Sentences', title: clip.title),
        ),
      );
      if (gen != _generation) return;
      await _player.seek(Duration.zero);
      // Completes when the clip finishes or playback is stopped elsewhere.
      await _player.play();
    }
  }

  Future<void> stop() {
    _generation++;
    return _player.stop();
  }

  Future<void> dispose() => _player.dispose();
}
