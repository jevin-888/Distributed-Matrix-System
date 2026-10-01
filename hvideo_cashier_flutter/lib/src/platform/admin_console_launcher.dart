import 'package:flutter/services.dart';

typedef AdminConsoleLauncher = Future<bool> Function(Uri uri);

const MethodChannel _channel = MethodChannel(
  'distributed_playback_system/admin_console',
);

Future<bool> openAdminConsole(Uri uri) async {
  if (uri.scheme != 'http' && uri.scheme != 'https') return false;
  return await _channel.invokeMethod<bool>('open', <String, String>{
        'url': uri.toString(),
      }) ??
      false;
}
