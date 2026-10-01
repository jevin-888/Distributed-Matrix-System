import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:http/http.dart' as http;

import '../kvm/kvm_transport.dart';
import '../models/matrix_models.dart';
import '../widgets/live_jpeg_preview.dart';

class KvmTakeoverPage extends StatefulWidget {
  const KvmTakeoverPage({
    super.key,
    required this.session,
    required this.onRelease,
    this.connector = KvmTransport.connect,
    this.previewClient,
  });

  final KvmSession session;
  final Future<void> Function(int sessionId) onRelease;
  final KvmChannelConnector connector;
  final http.Client? previewClient;

  @override
  State<KvmTakeoverPage> createState() => _KvmTakeoverPageState();
}

class _KvmTakeoverPageState extends State<KvmTakeoverPage> {
  final FocusNode _focusNode = FocusNode(debugLabel: 'KVM keyboard');
  KvmChannel? _channel;
  String _state = '正在建立 HID 通道';
  String? _error;
  int _mouseButtons = 0;
  bool _connecting = false;
  bool _closing = false;
  bool _released = false;

  @override
  void initState() {
    super.initState();
    unawaited(
      SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky),
    );
    unawaited(_connect());
  }

  Future<void> _connect() async {
    if (_connecting || _closing) return;
    _connecting = true;
    final previousChannel = _channel;
    _channel = null;
    await previousChannel?.close();
    if (mounted) {
      setState(() {
        _state = '正在连接';
        _error = null;
      });
    }
    try {
      final channel = await widget.connector(widget.session);
      if (!mounted || _closing) {
        await channel.close();
        return;
      }
      setState(() {
        _channel = channel;
        _state = 'HID 已接管';
      });
      _focusNode.requestFocus();
      unawaited(
        channel.done.then((_) {
          if (!mounted || _closing || _channel != channel) return;
          setState(() {
            _channel = null;
            _state = '连接已断开';
            _error = channel.lastError?.toString() ?? '设备已断开 KVM 接管通道';
          });
        }),
      );
    } catch (error) {
      if (!mounted) return;
      setState(() {
        _state = '连接失败';
        _error = error.toString();
      });
    } finally {
      _connecting = false;
    }
  }

  Future<void> _release() async {
    if (_released) return;
    _released = true;
    try {
      await widget.onRelease(widget.session.sessionId);
    } catch (_) {
      // The local input channel still closes even if Master is unreachable.
    }
  }

  Future<void> _exit() async {
    if (_closing) return;
    _closing = true;
    if (mounted) setState(() => _state = '正在释放接管');
    try {
      await _channel?.close();
      await _release();
    } finally {
      await SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge);
      if (mounted) Navigator.of(context).pop();
    }
  }

  void _sendPointerDelta(Offset delta) {
    final channel = _channel;
    if (channel == null || !channel.isConnected) return;
    unawaited(
      channel.sendMouse(
        buttons: _mouseButtons,
        deltaX: delta.dx.round(),
        deltaY: delta.dy.round(),
      ),
    );
  }

  void _updateButtons(int flutterButtons) {
    var buttons = 0;
    if (flutterButtons & kPrimaryMouseButton != 0) buttons |= 1;
    if (flutterButtons & kSecondaryMouseButton != 0) buttons |= 2;
    if (flutterButtons & kMiddleMouseButton != 0) buttons |= 4;
    _setMouseButtons(buttons);
  }

  void _setMouseButtons(int buttons) {
    _mouseButtons = buttons;
    unawaited(_channel?.sendMouse(buttons: buttons));
  }

  void _click(int button) {
    final channel = _channel;
    if (channel == null || !channel.isConnected) return;
    unawaited(() async {
      await channel.sendMouse(buttons: button);
      await Future<void>.delayed(const Duration(milliseconds: 45));
      await channel.sendMouse(buttons: 0);
    }());
  }

  @override
  void dispose() {
    _focusNode.dispose();
    unawaited(_channel?.close());
    unawaited(_release());
    unawaited(SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge));
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final label = widget.session.targetNodeLabel.isEmpty
        ? widget.session.targetNodeId.toString().padLeft(3, '0')
        : widget.session.targetNodeLabel;
    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, result) {
        if (!didPop) unawaited(_exit());
      },
      child: Scaffold(
        backgroundColor: const Color(0xff030609),
        body: KeyboardListener(
          focusNode: _focusNode,
          autofocus: true,
          onKeyEvent: (event) => unawaited(_channel?.handleKeyEvent(event)),
          child: Stack(
            fit: StackFit.expand,
            children: <Widget>[
              Listener(
                behavior: HitTestBehavior.opaque,
                onPointerMove: (event) => _sendPointerDelta(event.delta),
                onPointerDown: (event) => _updateButtons(
                  event.kind == PointerDeviceKind.touch
                      ? kPrimaryMouseButton
                      : event.buttons,
                ),
                onPointerUp: (_) => _updateButtons(0),
                onPointerCancel: (_) => _updateButtons(0),
                onPointerSignal: (event) {
                  if (event is PointerScrollEvent) {
                    unawaited(
                      _channel?.sendMouse(
                        buttons: _mouseButtons,
                        wheel: (-event.scrollDelta.dy / 20).round(),
                      ),
                    );
                  }
                },
                child: _LiveNodePreview(
                  uri: widget.session.previewUri,
                  client: widget.previewClient,
                ),
              ),
              Positioned(
                top: math.max(MediaQuery.paddingOf(context).top, 12),
                left: 12,
                right: 12,
                child: _SessionBar(
                  nodeLabel: label,
                  targetIp: widget.session.targetIp,
                  state: _state,
                  connected: _channel?.isConnected == true,
                  onRetry: _error == null || _connecting || _closing
                      ? null
                      : _connect,
                  onFocus: _channel?.isConnected == true
                      ? _focusNode.requestFocus
                      : null,
                  onExit: _closing ? null : _exit,
                ),
              ),
              Positioned(
                left: 16,
                right: 16,
                bottom: math.max(MediaQuery.paddingOf(context).bottom, 16),
                child: Align(
                  alignment: Alignment.bottomCenter,
                  child: _TouchButtons(
                    enabled: _channel?.isConnected == true,
                    onLeft: () => _click(1),
                    onRight: () => _click(2),
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _LiveNodePreview extends StatelessWidget {
  const _LiveNodePreview({required this.uri, this.client});

  final Uri uri;
  final http.Client? client;

  @override
  Widget build(BuildContext context) {
    return ColoredBox(
      color: const Color(0xff070b10),
      child: LiveJpegPreview(
        key: const ValueKey('kvm-live-preview'),
        uri: uri,
        client: client,
        interval: const Duration(milliseconds: 200),
        fit: BoxFit.contain,
        placeholderBuilder: (context, waiting) => Center(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              const Icon(
                Icons.videocam_outlined,
                color: Color(0xff7790a5),
                size: 44,
              ),
              const SizedBox(height: 12),
              Text(
                waiting ? '正在加载节点采集画面' : '节点实时预览连接失败',
                style: const TextStyle(
                  color: Color(0xffa8bac8),
                  fontSize: 16,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _SessionBar extends StatelessWidget {
  const _SessionBar({
    required this.nodeLabel,
    required this.targetIp,
    required this.state,
    required this.connected,
    required this.onRetry,
    required this.onFocus,
    required this.onExit,
  });

  final String nodeLabel;
  final String targetIp;
  final String state;
  final bool connected;
  final VoidCallback? onRetry;
  final VoidCallback? onFocus;
  final VoidCallback? onExit;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 760),
        child: DecoratedBox(
          decoration: BoxDecoration(
            color: const Color(0xf2265f91),
            border: Border.all(color: const Color(0xff163f62)),
          ),
          child: SizedBox(
            height: 58,
            child: Row(
              children: <Widget>[
                Expanded(
                  child: ColoredBox(
                    color: connected
                        ? const Color(0xff237f72)
                        : const Color(0xffb84f52),
                    child: Center(
                      child: Text(
                        '$targetIp · $nodeLabel · $state',
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: const TextStyle(
                          color: Colors.white,
                          fontSize: 16,
                          fontWeight: FontWeight.w600,
                        ),
                      ),
                    ),
                  ),
                ),
                _BarAction(
                  tooltip: state,
                  icon: connected
                      ? Icons.desktop_windows_rounded
                      : Icons.desktop_access_disabled_rounded,
                  onPressed: onRetry,
                ),
                _BarAction(
                  tooltip: '捕获键盘输入',
                  icon: Icons.keyboard_alt_outlined,
                  onPressed: onFocus,
                ),
                _BarAction(
                  tooltip: '重新连接',
                  icon: Icons.refresh_rounded,
                  onPressed: onRetry,
                ),
                _BarAction(
                  key: const ValueKey('exit-kvm-button'),
                  tooltip: '退出 KVM 接管',
                  icon: Icons.close_rounded,
                  onPressed: onExit,
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _BarAction extends StatelessWidget {
  const _BarAction({
    super.key,
    required this.tooltip,
    required this.icon,
    required this.onPressed,
  });

  final String tooltip;
  final IconData icon;
  final VoidCallback? onPressed;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 58,
      height: 58,
      child: IconButton(
        tooltip: tooltip,
        onPressed: onPressed,
        icon: Icon(icon),
        color: const Color(0xff25d6c0),
        disabledColor: const Color(0xff6d91a9),
      ),
    );
  }
}

class _TouchButtons extends StatelessWidget {
  const _TouchButtons({
    required this.enabled,
    required this.onLeft,
    required this.onRight,
  });

  final bool enabled;
  final VoidCallback onLeft;
  final VoidCallback onRight;

  @override
  Widget build(BuildContext context) {
    return SegmentedButton<int>(
      showSelectedIcon: false,
      emptySelectionAllowed: true,
      segments: const <ButtonSegment<int>>[
        ButtonSegment<int>(
          value: 1,
          icon: Icon(Icons.mouse_rounded),
          label: Text('左键'),
        ),
        ButtonSegment<int>(
          value: 2,
          icon: Icon(Icons.ads_click_rounded),
          label: Text('右键'),
        ),
      ],
      selected: const <int>{},
      onSelectionChanged: enabled
          ? (selection) {
              if (selection.contains(1)) onLeft();
              if (selection.contains(2)) onRight();
            }
          : null,
    );
  }
}
