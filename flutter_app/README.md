# Sentences

An audio-only phrase trainer for Android. Point it at a folder of Italian/English
sentence recordings on disk, put on headphones, and drive the whole session from
a 3-button ESP32-C3 BLE remote — the screen can stay off.

Scheduling is [FSRS](https://github.com/open-spaced-repetition) (via the `fsrs`
package); progress is stored locally on the device.

## Audio folder

A flat folder of `.mp3` (or `.m4a/.aac/.wav/.ogg/.opus/.flac`) files, one pair
per sentence, sharing a stem:

```
/wherever/Sentences/
  01_it.mp3   01_en.mp3
  02_it.mp3   02_en.mp3
  ...
```

`<stem>_it` is the front (played first, the prompt), `<stem>_en` is the back (the
reveal). Only stems that have **both** halves become cards. The stem (`01`) is
the stable id used for FSRS history, so renaming files resets their history.

The folder is chosen with the system folder picker and its path is remembered
(`shared_preferences`). On Android the app requests **All files access**
(`MANAGE_EXTERNAL_STORAGE`) so the folder can live anywhere; re-grant it from the
Setup screen if skipped.

## The remote (ESP32-C3 over BLE)

Firmware: [`../code/scripts/004_esp_c3_ble_buttons`](../code/scripts/004_esp_c3_ble_buttons).
It advertises a Nordic UART Service as `SentenceRemote` and notifies one ASCII
digit per debounced press. Buttons are on GPIO **3**, **4**, **5** (wired to GND,
`INPUT_PULLUP`).

BLE contract — shared by the firmware and `lib/services/ble_button_service.dart`:

| role | UUID |
|------|------|
| Service (NUS) | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX notify (device → app) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX write (app → device, unused) | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |

Pair once from the Setup screen (Scan → tap the device); the device id is
remembered and reconnected automatically on launch and whenever Bluetooth comes
back.

## Flow / state machine

`lib/services/practice_controller.dart`. On launch (folder set) it picks a card —
a never-practised one or one due per FSRS, at random — and auto-plays its Italian.

**hidden** (Italian played, meaning not shown yet)
- **5** → replay Italian
- **3** → reveal: play English → *revealed*

**revealed**
- **3** → replay Italian, then English
- **5** → score **Wrong** (`Rating.again`), reschedule, next card
- **4** → score **Correct** (`Rating.good`), reschedule, next card

Advancing auto-plays the next Italian and returns to *hidden*.

The on-screen number keys **3 / 4 / 5** (hardware keyboard) mirror the remote, for
testing without hardware. There are no on-screen practice buttons by design.

## Background audio

`just_audio` + `just_audio_background` run playback through a media-playback
foreground service (`MainActivity` extends `AudioServiceActivity`), so audio and
autoplay keep working with the screen off. The app requests `POST_NOTIFICATIONS`,
and holds `WAKE_LOCK` / `FOREGROUND_SERVICE_MEDIA_PLAYBACK`.

## Develop

```bash
flutter pub get
flutter analyze
flutter run -d android          # or: just dev
```

## Build / install

```bash
just apk                        # debug APK -> build/app/outputs/flutter-apk/
just install                    # build + adb install -r
```

Android only — the `ios/ macos/ linux/ windows/ web/` platform folders have been
removed.

## Layout

```
lib/
  models/sentence_card.dart          card model + folder scanner (runs off-isolate)
  services/
    sentence_library.dart            chosen folder + scanned card list
    review_store.dart                FSRS scheduler + shared_preferences persistence + pickNext()
    audio_service.dart               background AudioPlayer, gapless clip sequences
    ble_button_service.dart          BLE central: scan / connect / reconnect, button-digit stream
    practice_controller.dart         the hidden/revealed state machine
  screens/
    setup_screen.dart                folder picker, storage permission, BLE pairing
    practice_screen.dart             status-only UI + keyboard mirror of the remote
```

## Local data keys (`shared_preferences`)

- `sentences_folder` — chosen folder path
- `ble_device_id` — remembered remote
- `fsrs_cards_v1` — JSON map of `stem → fsrs Card`
