package com.example.sentences

import com.ryanheise.audioservice.AudioServiceActivity

// AudioServiceActivity (a FlutterActivity subclass) is required by
// just_audio_background so playback survives with the screen off.
class MainActivity : AudioServiceActivity()
