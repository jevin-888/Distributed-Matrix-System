import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/services.dart';

import '../models/matrix_models.dart';

abstract interface class KvmChannel {
  bool get isConnected;
  Future<void> get done;
  Object? get lastError;
  Future<void> handleKeyEvent(KeyEvent event);
  Future<void> sendMouse({
    required int buttons,
    int deltaX = 0,
    int deltaY = 0,
    int wheel = 0,
  });
  Future<void> close();
}

typedef KvmChannelConnector = Future<KvmChannel> Function(KvmSession session);

class KvmTransport implements KvmChannel {
  KvmTransport._(this._socket, this._sessionId, this._sessionToken) {
    _subscription = _socket.listen(
      _handleData,
      onError: _handleSocketError,
      onDone: _handleSocketDone,
      cancelOnError: true,
    );
  }

  static const int headerLength = 28;
  static const int _magic = 0x4b564d31;
  static const int _version = 1;
  static const int _helloType = 1;
  static const int _keyboardType = 2;
  static const int _mouseType = 3;
  static const int _ackType = 4;
  static const int _pingType = 5;

  final Socket _socket;
  final int _sessionId;
  final int _sessionToken;
  final Set<int> _pressedUsages = <int>{};
  final List<int> _receiveBuffer = <int>[];
  final Completer<void> _handshake = Completer<void>();
  final Completer<void> _done = Completer<void>();
  late final StreamSubscription<List<int>> _subscription;
  Timer? _heartbeat;
  DateTime _lastAck = DateTime.now();
  Object? _lastError;
  int _sequence = 1;
  bool _closed = false;

  @override
  bool get isConnected => !_closed;

  @override
  Future<void> get done => _done.future;

  @override
  Object? get lastError => _lastError;

  static Future<KvmChannel> connect(KvmSession session) async {
    final socket = await Socket.connect(
      session.targetIp,
      session.targetPort,
      timeout: const Duration(seconds: 5),
    );
    socket.setOption(SocketOption.tcpNoDelay, true);
    final transport = KvmTransport._(
      socket,
      session.sessionId,
      session.sessionToken,
    );
    transport._writeFrame(_helloType, Uint8List(0), sequence: 0);
    await socket.flush();
    try {
      await transport._handshake.future.timeout(const Duration(seconds: 4));
    } catch (error) {
      await transport._abort(
        error is TimeoutException
            ? const KvmTransportException('设备未确认 KVM 接管，请检查固件版本和 HID 连接')
            : error,
      );
      Error.throwWithStackTrace(
        transport.lastError ?? error,
        StackTrace.current,
      );
    }
    transport._heartbeat = Timer.periodic(
      const Duration(seconds: 1),
      (_) => transport._sendHeartbeat(),
    );
    return transport;
  }

  void _handleData(List<int> bytes) {
    if (_closed) return;
    _receiveBuffer.addAll(bytes);
    while (_receiveBuffer.length >= headerLength) {
      final header = Uint8List.fromList(
        _receiveBuffer.sublist(0, headerLength),
      );
      final data = ByteData.sublistView(header);
      final payloadLength = data.getUint32(24, Endian.big);
      if (payloadLength > 8) {
        unawaited(_abort(const KvmTransportException('设备返回了无效的 KVM 数据帧')));
        return;
      }
      final frameLength = headerLength + payloadLength;
      if (_receiveBuffer.length < frameLength) return;
      _receiveBuffer.removeRange(0, frameLength);
      final valid =
          data.getUint32(0, Endian.big) == _magic &&
          data.getUint16(4, Endian.big) == _version &&
          data.getUint16(6, Endian.big) == _ackType &&
          data.getUint32(8, Endian.big) == _sessionId &&
          data.getUint64(16, Endian.big) == _sessionToken &&
          payloadLength == 0;
      if (!valid) {
        unawaited(_abort(const KvmTransportException('设备返回了无效的 KVM 握手确认')));
        return;
      }
      _lastAck = DateTime.now();
      if (!_handshake.isCompleted) _handshake.complete();
    }
  }

  void _handleSocketError(Object error, StackTrace stackTrace) {
    unawaited(_abort(KvmTransportException('KVM 网络连接异常：$error')));
  }

  void _handleSocketDone() {
    if (!_closed) {
      unawaited(_abort(const KvmTransportException('设备已断开 KVM 接管通道')));
    }
  }

  void _sendHeartbeat() {
    if (_closed) return;
    if (DateTime.now().difference(_lastAck) > const Duration(seconds: 4)) {
      unawaited(_abort(const KvmTransportException('设备 KVM 心跳超时')));
      return;
    }
    _writeFrame(_pingType, Uint8List(0));
  }

  static Uint8List encodeFrame({
    required int type,
    required int sessionId,
    required int sequence,
    required int sessionToken,
    required Uint8List payload,
  }) {
    final bytes = Uint8List(headerLength + payload.length);
    final data = ByteData.sublistView(bytes);
    data.setUint32(0, _magic, Endian.big);
    data.setUint16(4, _version, Endian.big);
    data.setUint16(6, type, Endian.big);
    data.setUint32(8, sessionId, Endian.big);
    data.setUint32(12, sequence, Endian.big);
    data.setUint64(16, sessionToken, Endian.big);
    data.setUint32(24, payload.length, Endian.big);
    bytes.setRange(headerLength, bytes.length, payload);
    return bytes;
  }

  @override
  Future<void> handleKeyEvent(KeyEvent event) async {
    if (_closed || event is KeyRepeatEvent) return;
    final usage = event.physicalKey.usbHidUsage & 0xffff;
    if (usage == 0) return;
    if (event is KeyDownEvent) {
      _pressedUsages.add(usage);
    } else if (event is KeyUpEvent) {
      _pressedUsages.remove(usage);
    } else {
      return;
    }
    _writeFrame(_keyboardType, _keyboardReport());
  }

  Uint8List _keyboardReport() {
    final report = Uint8List(8);
    final regularKeys = <int>[];
    for (final usage in _pressedUsages) {
      if (usage >= 0xe0 && usage <= 0xe7) {
        report[0] |= 1 << (usage - 0xe0);
      } else if (regularKeys.length < 6) {
        regularKeys.add(usage);
      }
    }
    for (var index = 0; index < regularKeys.length; index++) {
      report[index + 2] = regularKeys[index];
    }
    return report;
  }

  @override
  Future<void> sendMouse({
    required int buttons,
    int deltaX = 0,
    int deltaY = 0,
    int wheel = 0,
  }) async {
    if (_closed) return;
    final report = Uint8List(4)
      ..[0] = buttons & 0x07
      ..[1] = _signedByte(deltaX)
      ..[2] = _signedByte(deltaY)
      ..[3] = _signedByte(wheel);
    _writeFrame(_mouseType, report);
  }

  int _signedByte(int value) => value.clamp(-127, 127).toInt() & 0xff;

  void _writeFrame(int type, Uint8List payload, {int? sequence}) {
    if (_closed) return;
    try {
      _socket.add(
        encodeFrame(
          type: type,
          sessionId: _sessionId,
          sequence: sequence ?? _sequence++,
          sessionToken: _sessionToken,
          payload: payload,
        ),
      );
    } catch (error) {
      unawaited(_abort(KvmTransportException('KVM 数据发送失败：$error')));
    }
  }

  @override
  Future<void> close() async {
    if (_closed) return;
    _writeFrame(_keyboardType, Uint8List(8));
    _writeFrame(_mouseType, Uint8List(4));
    await _shutdown();
  }

  Future<void> _abort(Object error) async {
    if (_closed) return;
    _lastError = error;
    if (!_handshake.isCompleted) _handshake.completeError(error);
    await _shutdown();
  }

  Future<void> _shutdown() async {
    if (_closed) return;
    _closed = true;
    _heartbeat?.cancel();
    try {
      await _socket.flush();
    } finally {
      await _subscription.cancel();
      await _socket.close();
      if (!_done.isCompleted) _done.complete();
    }
  }
}

class KvmTransportException implements Exception {
  const KvmTransportException(this.message);
  final String message;

  @override
  String toString() => message;
}
