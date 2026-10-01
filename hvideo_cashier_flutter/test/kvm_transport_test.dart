import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:distributed_playback_system/src/kvm/kvm_transport.dart';
import 'package:distributed_playback_system/src/models/matrix_models.dart';

void main() {
  test('KVM session exposes the authenticated live preview URI', () {
    const session = KvmSession(
      sessionId: 9,
      targetNodeId: 1,
      targetNodeLabel: '001',
      state: 'acquired',
      acquiredAt: 1,
      expiresAt: 60000,
      targetIp: '192.168.2.101',
      targetPort: 9101,
      sessionToken: 99,
    );

    expect(
      session.previewUri.toString(),
      'http://192.168.2.101:9102/kvm/preview.jpg?sessionId=9&token=99',
    );
  });

  test('matrix node reuses the live preview endpoint for the canvas', () {
    const node = MatrixNode(
      nodeId: 1,
      ip: '192.168.2.101',
      resolution: '1920x1080',
      status: 'online',
      playState: 'idle',
      cpu: 0,
      memory: 0,
      masterClockSynchronized: true,
      masterClockOffsetMs: 0,
      lastSeen: 1,
      kvmEnabled: true,
      kvmRole: 'target',
      kvmPort: 9101,
      signalSourceType: 'capture',
      signalSourceConfigured: true,
    );

    expect(
      node.signalPreviewUri.toString(),
      'http://192.168.2.101:9102/kvm/preview.jpg',
    );
  });

  test('KVM frame uses the exact 28-byte big-endian wire header', () {
    final frame = KvmTransport.encodeFrame(
      type: 2,
      sessionId: 0x01020304,
      sequence: 0x05060708,
      sessionToken: 0x0102030405060708,
      payload: Uint8List.fromList(<int>[1, 2, 3]),
    );
    final data = ByteData.sublistView(frame);

    expect(frame.length, 31);
    expect(data.getUint32(0, Endian.big), 0x4b564d31);
    expect(data.getUint16(4, Endian.big), 1);
    expect(data.getUint16(6, Endian.big), 2);
    expect(data.getUint32(8, Endian.big), 0x01020304);
    expect(data.getUint32(12, Endian.big), 0x05060708);
    expect(data.getUint64(16, Endian.big), 0x0102030405060708);
    expect(data.getUint32(24, Endian.big), 3);
    expect(frame.sublist(28), <int>[1, 2, 3]);
  });

  test('KVM transport waits for target ACK and maintains heartbeat', () async {
    final server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    Socket? target;
    final served = () async {
      target = await server.first;
      final reader = _SocketReader(target!);
      final hello = await reader.readExact(KvmTransport.headerLength);
      final helloData = ByteData.sublistView(hello);
      expect(helloData.getUint16(6, Endian.big), 1);
      expect(helloData.getUint32(8, Endian.big), 9);
      expect(helloData.getUint64(16, Endian.big), 99);
      target!.add(
        KvmTransport.encodeFrame(
          type: 4,
          sessionId: 9,
          sequence: 0,
          sessionToken: 99,
          payload: Uint8List(0),
        ),
      );
      await target!.flush();

      final ping = await reader.readExact(
        KvmTransport.headerLength,
        timeout: const Duration(seconds: 3),
      );
      final pingData = ByteData.sublistView(ping);
      expect(pingData.getUint16(6, Endian.big), 5);
      target!.add(
        KvmTransport.encodeFrame(
          type: 4,
          sessionId: 9,
          sequence: pingData.getUint32(12, Endian.big),
          sessionToken: 99,
          payload: Uint8List(0),
        ),
      );
      await target!.flush();
    }();

    final channel = await KvmTransport.connect(
      KvmSession(
        sessionId: 9,
        targetNodeId: 1,
        targetNodeLabel: '001',
        state: 'acquired',
        acquiredAt: 1,
        expiresAt: 60000,
        targetIp: InternetAddress.loopbackIPv4.address,
        targetPort: server.port,
        sessionToken: 99,
      ),
    );
    expect(channel.isConnected, isTrue);
    await served;
    expect(channel.isConnected, isTrue);
    await channel.close();
    expect(channel.isConnected, isFalse);
    await target?.close();
    await server.close();
  });
}

class _SocketReader {
  _SocketReader(Socket socket) : _iterator = StreamIterator<List<int>>(socket);

  final StreamIterator<List<int>> _iterator;
  final List<int> _buffer = <int>[];

  Future<Uint8List> readExact(
    int length, {
    Duration timeout = const Duration(seconds: 2),
  }) async {
    while (_buffer.length < length) {
      final hasData = await _iterator.moveNext().timeout(timeout);
      if (!hasData) throw const SocketException('Socket closed before frame');
      _buffer.addAll(_iterator.current);
    }
    final result = Uint8List.fromList(_buffer.sublist(0, length));
    _buffer.removeRange(0, length);
    return result;
  }
}
