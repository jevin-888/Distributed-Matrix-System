import 'dart:async';

import 'package:flutter/material.dart';

import '../api/device_discovery.dart';
import '../theme/app_theme.dart';

class ConnectionDialog extends StatefulWidget {
  const ConnectionDialog({
    super.key,
    required this.serverAddress,
    required this.connect,
    this.discovery,
  });

  final String serverAddress;
  final Future<String?> Function(String address) connect;
  final DeviceDiscovery? discovery;

  @override
  State<ConnectionDialog> createState() => _ConnectionDialogState();
}

class _ConnectionDialogState extends State<ConnectionDialog> {
  late final DeviceDiscovery _discovery;
  late final TextEditingController _address;
  bool _connecting = false;
  String? _connectingAddress;
  String? _error;

  @override
  void initState() {
    super.initState();
    _address = TextEditingController(text: widget.serverAddress);
    _discovery = widget.discovery ?? DeviceDiscovery();
    _discovery.addListener(_changed);
    unawaited(_discovery.search(widget.serverAddress));
  }

  void _changed() {
    if (mounted) setState(() {});
  }

  Future<void> _connect(String address) async {
    if (_connecting) return;
    final value = address.trim();
    final uri = Uri.tryParse(value.contains('://') ? value : 'http://$value');
    if (uri == null ||
        uri.host.isEmpty ||
        !['http', 'https'].contains(uri.scheme) ||
        uri.userInfo.isNotEmpty) {
      setState(() => _error = '请输入有效的设备 IP 或 HTTP 地址。');
      return;
    }
    _discovery.cancel();
    setState(() {
      _connecting = true;
      _connectingAddress = uri.host;
      _error = null;
    });
    String? error;
    try {
      error = await widget.connect(uri.toString());
    } catch (exception) {
      error = '连接失败：$exception';
    }
    if (!mounted) return;
    setState(() => _connecting = false);
    if (error == null) {
      Navigator.of(context).pop();
    } else {
      setState(() => _error = error);
    }
  }

  @override
  void dispose() {
    _discovery.removeListener(_changed);
    _discovery.dispose();
    _address.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final devices = _discovery.devices;
    return PopScope(
      canPop: !_connecting,
      child: AlertDialog(
        backgroundColor: const Color(0xFF09243A),
        title: const Text('连接设备'),
        scrollable: true,
        content: SizedBox(
          width: 520,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text(
                '自动搜索局域网内在线设备，点击设备即可连接。',
                style: TextStyle(color: mutedText, fontSize: 12),
              ),
              const SizedBox(height: 16),
              Row(
                children: [
                  Expanded(
                    child: Text(
                      _connecting
                          ? '正在连接 $_connectingAddress…'
                          : _discovery.searching
                          ? '正在搜索 · 已发现 ${devices.length} 台设备'
                          : '发现 ${devices.length} 台在线设备',
                    ),
                  ),
                  TextButton.icon(
                    onPressed: _connecting || _discovery.searching
                        ? null
                        : () {
                            setState(() => _error = null);
                            unawaited(_discovery.search(_address.text));
                          },
                    icon: const Icon(Icons.refresh, size: 18),
                    label: const Text('重新搜索'),
                  ),
                ],
              ),
              if (_discovery.searching || _connecting)
                const LinearProgressIndicator(minHeight: 2),
              if (_discovery.searching && _discovery.total > 0)
                Padding(
                  padding: const EdgeInsets.only(top: 6),
                  child: Text(
                    '已检查 ${_discovery.checked} / ${_discovery.total} 个地址',
                    style: const TextStyle(color: mutedText, fontSize: 11),
                  ),
                ),
              const SizedBox(height: 8),
              if (devices.isEmpty)
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 22),
                  child: Text(
                    _discovery.searching
                        ? '正在查找在线设备…'
                        : '未发现在线设备，请确认设备已开机并与本机联网。',
                    style: const TextStyle(color: mutedText),
                  ),
                ),
              if (devices.isNotEmpty)
                SizedBox(
                  height: (devices.length * 76.0).clamp(76.0, 280.0),
                  child: ListView.builder(
                    itemCount: devices.length,
                    itemBuilder: (context, index) {
                      final device = devices[index];
                      return ListTile(
                        key: ValueKey('connect-device-${device.address}'),
                        contentPadding: const EdgeInsets.symmetric(
                          horizontal: 8,
                        ),
                        leading: Icon(
                          Icons.dns_outlined,
                          color: device.canConnect ? cyan : mutedText,
                        ),
                        title: Text(
                          device.name,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                        ),
                        subtitle: Text(
                          '${device.address}\n${device.canConnect
                              ? '在线 · 点击连接'
                              : _discovery.searching
                              ? '在线 · 正在检查控制服务'
                              : '在线 · 未开放主节点控制服务'}',
                          style: const TextStyle(
                            fontSize: 11,
                            color: mutedText,
                          ),
                        ),
                        trailing: device.canConnect
                            ? const Icon(Icons.chevron_right, color: cyan)
                            : null,
                        onTap: _connecting || !device.canConnect
                            ? null
                            : () => _connect(device.serverUri.toString()),
                      );
                    },
                  ),
                ),
              if (_error ?? _discovery.error ?? _discovery.notice
                  case final String message)
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 8),
                  child: Text(
                    message,
                    style: const TextStyle(color: amber, fontSize: 12),
                  ),
                ),
              ExpansionTile(
                tilePadding: EdgeInsets.zero,
                title: const Text('手动输入地址', style: TextStyle(fontSize: 12)),
                children: [
                  TextField(
                    controller: _address,
                    enabled: !_connecting,
                    keyboardType: TextInputType.url,
                    decoration: const InputDecoration(
                      labelText: '设备地址',
                      hintText: 'http://192.168.2.107:8080',
                    ),
                    onSubmitted: _connect,
                  ),
                  Align(
                    alignment: Alignment.centerRight,
                    child: TextButton(
                      onPressed: _connecting
                          ? null
                          : () => _connect(_address.text),
                      child: const Text('连接此地址'),
                    ),
                  ),
                ],
              ),
            ],
          ),
        ),
        actions: [
          TextButton(
            onPressed: _connecting ? null : () => Navigator.pop(context),
            child: const Text('取消'),
          ),
        ],
      ),
    );
  }
}
