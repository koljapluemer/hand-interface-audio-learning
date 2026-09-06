import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// Nordic UART Service — the ESP32-C3 firmware advertises this and pushes one
/// ASCII digit (`3`, `4` or `5`) on the TX characteristic per debounced press.
final Guid kNusService = Guid('6e400001-b5a3-f393-e0a9-e50e24dcca9e');
final Guid kNusTxChar = Guid('6e400003-b5a3-f393-e0a9-e50e24dcca9e');

const _prefsDeviceKey = 'ble_device_id';

enum BleStatus { unsupported, off, disconnected, scanning, connecting, connected }

/// BLE central: pairs with the button remote, keeps the link up, and turns
/// notifications into a stream of button numbers (3, 4, 5).
class BleButtonService extends ChangeNotifier {
  BleStatus status = BleStatus.disconnected;
  String? error;
  String? connectedName;
  String? savedDeviceId;
  final List<ScanResult> scanResults = [];

  final StreamController<int> _buttons = StreamController<int>.broadcast();
  Stream<int> get buttonEvents => _buttons.stream;

  BluetoothDevice? _device;
  bool _wantConnected = false;
  StreamSubscription<BluetoothAdapterState>? _adapterSub;
  StreamSubscription<List<ScanResult>>? _scanSub;
  StreamSubscription<bool>? _isScanningSub;
  StreamSubscription<BluetoothConnectionState>? _connSub;
  StreamSubscription<List<int>>? _valueSub;

  Future<void> init() async {
    final prefs = await SharedPreferences.getInstance();
    savedDeviceId = prefs.getString(_prefsDeviceKey);

    if (!await FlutterBluePlus.isSupported) {
      _set(BleStatus.unsupported);
      return;
    }
    _adapterSub = FlutterBluePlus.adapterState.listen((s) {
      if (s == BluetoothAdapterState.on) {
        if (status == BleStatus.off) _set(BleStatus.disconnected);
        if (savedDeviceId != null && status == BleStatus.disconnected) {
          reconnect();
        }
      } else {
        _set(BleStatus.off);
      }
    });
    if (savedDeviceId != null) reconnect();
  }

  /// Reconnect to the remembered device (called on launch and on BT re-enable).
  Future<void> reconnect() async {
    final id = savedDeviceId;
    if (id == null) return;
    await _connect(BluetoothDevice.fromId(id));
  }

  Future<void> startScan() async {
    error = null;
    scanResults.clear();
    _set(BleStatus.scanning);

    await _scanSub?.cancel();
    _scanSub = FlutterBluePlus.onScanResults.listen(
      (results) {
        scanResults
          ..clear()
          ..addAll(results.where((r) =>
              r.device.platformName.isNotEmpty ||
              r.advertisementData.advName.isNotEmpty));
        notifyListeners();
      },
      onError: (Object e) {
        error = '$e';
        notifyListeners();
      },
    );
    _isScanningSub ??= FlutterBluePlus.isScanning.listen((scanning) {
      if (!scanning && status == BleStatus.scanning) {
        _set(BleStatus.disconnected);
      }
    });

    try {
      await FlutterBluePlus.startScan(
        withServices: [kNusService],
        timeout: const Duration(seconds: 15),
      );
    } catch (_) {
      // Some builds advertise NUS only in the scan response — scan unfiltered.
      await FlutterBluePlus.startScan(timeout: const Duration(seconds: 15));
    }
  }

  Future<void> stopScan() async {
    if (FlutterBluePlus.isScanningNow) await FlutterBluePlus.stopScan();
  }

  Future<void> connectTo(BluetoothDevice device) async {
    await stopScan();
    await _connect(device, remember: true);
  }

  Future<void> forget() async {
    _wantConnected = false;
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_prefsDeviceKey);
    savedDeviceId = null;
    try {
      await _device?.disconnect();
    } catch (_) {}
    _device = null;
    connectedName = null;
    _set(BleStatus.disconnected);
  }

  Future<void> _connect(BluetoothDevice device, {bool remember = false}) async {
    if (status == BleStatus.connecting) return;
    _wantConnected = true;
    _device = device;
    _set(BleStatus.connecting);
    try {
      await _connSub?.cancel();
      _connSub = device.connectionState.listen(_onConnState);
      await device.connect(
        license: License.nonprofit,
        timeout: const Duration(seconds: 20),
      );
      if (remember) {
        savedDeviceId = device.remoteId.str;
        final prefs = await SharedPreferences.getInstance();
        await prefs.setString(_prefsDeviceKey, savedDeviceId!);
      }
      await _discover(device);
    } catch (e) {
      error = '$e';
      _set(BleStatus.disconnected);
    }
  }

  Future<void> _discover(BluetoothDevice device) async {
    final services = await device.discoverServices();
    for (final s in services) {
      if (s.uuid != kNusService) continue;
      for (final c in s.characteristics) {
        if (c.uuid != kNusTxChar) continue;
        await _valueSub?.cancel();
        _valueSub = c.onValueReceived.listen(_onData);
        await c.setNotifyValue(true);
        connectedName = device.platformName.isEmpty ? 'ESP32' : device.platformName;
        _set(BleStatus.connected);
        return;
      }
    }
    error = 'Nordic UART service not found on this device.';
    await device.disconnect();
    _set(BleStatus.disconnected);
  }

  void _onConnState(BluetoothConnectionState s) {
    if (s == BluetoothConnectionState.connected) {
      if (status != BleStatus.connected) _set(BleStatus.connecting);
    } else if (s == BluetoothConnectionState.disconnected) {
      _valueSub?.cancel();
      _set(BleStatus.disconnected);
      if (_wantConnected && _device != null) {
        Future.delayed(const Duration(seconds: 3), () {
          if (_wantConnected && status == BleStatus.disconnected) {
            _connect(_device!);
          }
        });
      }
    }
  }

  void _onData(List<int> bytes) {
    for (final b in bytes) {
      if (b >= 0x33 && b <= 0x35) _buttons.add(b - 0x30); // '3'..'5'
    }
  }

  void _set(BleStatus s) {
    status = s;
    notifyListeners();
  }

  @override
  void dispose() {
    _adapterSub?.cancel();
    _scanSub?.cancel();
    _isScanningSub?.cancel();
    _connSub?.cancel();
    _valueSub?.cancel();
    _buttons.close();
    super.dispose();
  }
}
