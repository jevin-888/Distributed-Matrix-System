import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;

typedef PreviewPlaceholderBuilder =
    Widget Function(BuildContext context, bool waitingForFirstFrame);

class LiveJpegPreview extends StatefulWidget {
  const LiveJpegPreview({
    super.key,
    required this.uri,
    required this.placeholderBuilder,
    this.client,
    this.interval = const Duration(milliseconds: 500),
    this.requestTimeout = const Duration(seconds: 3),
    this.fit = BoxFit.contain,
  });

  final Uri uri;
  final http.Client? client;
  final Duration interval;
  final Duration requestTimeout;
  final BoxFit fit;
  final PreviewPlaceholderBuilder placeholderBuilder;

  @override
  State<LiveJpegPreview> createState() => _LiveJpegPreviewState();
}

class _LiveJpegPreviewState extends State<LiveJpegPreview> {
  late final _PreviewFeedListener _listener;
  _PreviewFeed? _feed;
  ui.Image? _image;
  bool _lastRequestFailed = false;

  @override
  void initState() {
    super.initState();
    _listener = _handleFeedUpdate;
    _attachFeed();
  }

  @override
  void didUpdateWidget(covariant LiveJpegPreview oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.uri != widget.uri ||
        !identical(oldWidget.client, widget.client)) {
      _attachFeed();
    } else if (oldWidget.interval != widget.interval ||
        oldWidget.requestTimeout != widget.requestTimeout) {
      _feed?.updateOptions(widget.interval, widget.requestTimeout);
    }
  }

  void _attachFeed() {
    _detachFeed();
    _image = null;
    _lastRequestFailed = false;
    _feed = _PreviewFeedRegistry.acquire(
      widget.uri,
      widget.client,
      widget.interval,
      widget.requestTimeout,
      _listener,
    );
  }

  void _detachFeed() {
    final feed = _feed;
    if (feed == null) return;
    _PreviewFeedRegistry.release(feed, _listener);
    _feed = null;
  }

  void _handleFeedUpdate(
    ui.Image? image,
    bool requestFailed,
    bool waitingForFirstFrame,
  ) {
    _image = image;
    _lastRequestFailed = requestFailed && !waitingForFirstFrame;
    if (mounted) setState(() {});
  }

  @override
  void dispose() {
    _detachFeed();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final image = _image;
    if (image == null) {
      return widget.placeholderBuilder(context, !_lastRequestFailed);
    }
    return RepaintBoundary(
      child: RawImage(
        image: image,
        fit: widget.fit,
        filterQuality: FilterQuality.low,
      ),
    );
  }
}

typedef _PreviewFeedListener =
    void Function(
      ui.Image? image,
      bool requestFailed,
      bool waitingForFirstFrame,
    );

class _PreviewFeedRegistry {
  static final Map<String, _PreviewFeed> _feeds = <String, _PreviewFeed>{};

  static _PreviewFeed acquire(
    Uri uri,
    http.Client? client,
    Duration interval,
    Duration requestTimeout,
    _PreviewFeedListener listener,
  ) {
    final key = uri.toString();
    final feed = _feeds.putIfAbsent(
      key,
      () => _PreviewFeed(
        key: key,
        uri: uri,
        client: client,
        interval: interval,
        requestTimeout: requestTimeout,
      ),
    );
    feed.updateOptions(interval, requestTimeout);
    feed.addListener(listener);
    return feed;
  }

  static void release(_PreviewFeed feed, _PreviewFeedListener listener) {
    feed.removeListener(listener);
    if (feed.hasListeners) return;
    if (identical(_feeds[feed.key], feed)) _feeds.remove(feed.key);
    feed.dispose();
  }
}

class _PreviewFeed {
  _PreviewFeed({
    required this.key,
    required this.uri,
    required http.Client? client,
    required this.interval,
    required this.requestTimeout,
  }) : _client = client ?? http.Client(),
       _ownsClient = client == null;

  final String key;
  final Uri uri;
  final http.Client _client;
  final bool _ownsClient;
  final Set<_PreviewFeedListener> _listeners = <_PreviewFeedListener>{};
  Duration interval;
  Duration requestTimeout;
  StreamSubscription<List<int>>? _streamSubscription;
  Timer? _retryTimer;
  Uint8List? _pendingFrameBytes;
  ui.Image? _decodedImage;
  final Set<ui.Image> _retiredImages = <ui.Image>{};
  Timer? _retiredImageTimer;
  bool _decodeInFlight = false;
  bool _lastRequestFailed = false;
  bool _useSingleFrameFallback = false;
  bool _started = false;
  int _generation = 0;

  bool get hasListeners => _listeners.isNotEmpty;

  void updateOptions(Duration interval, Duration requestTimeout) {
    if (interval < this.interval) this.interval = interval;
    if (requestTimeout < this.requestTimeout) {
      this.requestTimeout = requestTimeout;
    }
  }

  void addListener(_PreviewFeedListener listener) {
    _listeners.add(listener);
    listener(
      _decodedImage,
      _lastRequestFailed,
      _decodedImage == null && !_lastRequestFailed,
    );
    if (!_started) {
      _started = true;
      unawaited(_connect(++_generation));
    }
  }

  void removeListener(_PreviewFeedListener listener) {
    _listeners.remove(listener);
  }

  Future<void> _connect(int generation) async {
    if (!_hasGeneration(generation)) return;
    if (_useSingleFrameFallback) {
      await _connectSingleFrame(generation);
      return;
    }
    final streamUri = uri.replace(
      queryParameters: <String, String>{...uri.queryParameters, 'stream': '1'},
    );
    try {
      final request = http.Request('GET', streamUri)
        ..headers['Accept'] = 'multipart/x-mixed-replace, image/jpeg';
      final response = await _client.send(request).timeout(requestTimeout);
      if (!_hasGeneration(generation)) return;
      if (response.statusCode != 200) {
        await response.stream.drain();
        if (response.statusCode == 403 || response.statusCode == 404) {
          _useSingleFrameFallback = true;
          await _connectSingleFrame(generation);
        } else {
          _markFailed(generation);
        }
        return;
      }

      final contentType = response.headers['content-type']?.toLowerCase() ?? '';
      if (!contentType.startsWith('multipart/x-mixed-replace')) {
        final bytes = await response.stream.toBytes().timeout(requestTimeout);
        if (_looksLikeJpeg(bytes)) {
          _useSingleFrameFallback = true;
          _showFrame(bytes, generation);
          _scheduleRetry(generation);
        } else {
          _markFailed(generation);
        }
        return;
      }

      final parser = _MjpegParser();
      final completed = Completer<void>();
      late final StreamSubscription<List<int>> subscription;
      subscription = response.stream.listen(
        (chunk) {
          if (!_hasGeneration(generation)) return;
          for (final frame in parser.add(chunk)) {
            _showFrame(frame, generation);
          }
        },
        onError: (_) {
          if (!completed.isCompleted) completed.complete();
        },
        onDone: () {
          if (!completed.isCompleted) completed.complete();
        },
        cancelOnError: true,
      );
      _streamSubscription = subscription;
      await completed.future;
      if (_streamSubscription == subscription) _streamSubscription = null;
      _scheduleRetry(generation);
    } catch (_) {
      _markFailed(generation);
    }
  }

  Future<void> _connectSingleFrame(int generation) async {
    final query = <String, String>{
      ...uri.queryParameters,
      'v': DateTime.now().microsecondsSinceEpoch.toString(),
    }..remove('stream');
    try {
      final response = await _client
          .get(
            uri.replace(queryParameters: query),
            headers: const <String, String>{'Accept': 'image/jpeg'},
          )
          .timeout(requestTimeout);
      if (!_hasGeneration(generation)) return;
      if (response.statusCode == 200 && _looksLikeJpeg(response.bodyBytes)) {
        _showFrame(response.bodyBytes, generation);
        _scheduleRetry(generation);
      } else {
        _markFailed(generation);
      }
    } catch (_) {
      _markFailed(generation);
    }
  }

  bool _hasGeneration(int generation) =>
      _listeners.isNotEmpty && generation == _generation;

  bool _looksLikeJpeg(List<int> bytes) =>
      bytes.length >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8;

  void _showFrame(Uint8List bytes, int generation) {
    if (!_hasGeneration(generation) || bytes.isEmpty) return;
    _pendingFrameBytes = bytes;
    _startDecodeIfNeeded();
  }

  void _markFailed(int generation) {
    if (!_hasGeneration(generation)) return;
    if (!_lastRequestFailed) {
      _lastRequestFailed = true;
      _notifyListeners();
    }
    _scheduleRetry(generation);
  }

  void _scheduleRetry(int generation) {
    if (!_hasGeneration(generation) || _retryTimer?.isActive == true) return;
    _retryTimer = Timer(interval, () {
      if (_hasGeneration(generation)) unawaited(_connect(generation));
    });
  }

  void _notifyListeners() {
    final waiting = _decodedImage == null && !_lastRequestFailed;
    for (final listener in List<_PreviewFeedListener>.of(_listeners)) {
      listener(_decodedImage, _lastRequestFailed, waiting);
    }
  }

  void _startDecodeIfNeeded() {
    if (_decodeInFlight || _pendingFrameBytes == null || _listeners.isEmpty) {
      return;
    }
    _decodeInFlight = true;
    unawaited(_drainDecodeQueue());
  }

  Future<void> _drainDecodeQueue() async {
    try {
      while (_listeners.isNotEmpty && _pendingFrameBytes != null) {
        final bytes = _pendingFrameBytes;
        _pendingFrameBytes = null;
        if (bytes == null) continue;
        ui.Codec? codec;
        try {
          codec = await ui.instantiateImageCodec(bytes);
          final decoded = await codec.getNextFrame();
          if (_listeners.isEmpty) {
            decoded.image.dispose();
            continue;
          }
          final previous = _decodedImage;
          _decodedImage = decoded.image;
          if (previous != null) {
            _retiredImages.add(previous);
            _scheduleRetiredImageCleanup();
          }
          _lastRequestFailed = false;
          _notifyListeners();
        } catch (_) {
          // Keep the last good frame and let the feed retry.
        } finally {
          codec?.dispose();
        }
      }
    } finally {
      _decodeInFlight = false;
      if (_listeners.isNotEmpty) _startDecodeIfNeeded();
    }
  }

  void dispose() {
    _generation++;
    _retryTimer?.cancel();
    _pendingFrameBytes = null;
    unawaited(_streamSubscription?.cancel());
    _retiredImageTimer?.cancel();
    for (final image in _retiredImages) {
      image.dispose();
    }
    _retiredImages.clear();
    _decodedImage?.dispose();
    _decodedImage = null;
    if (_ownsClient) _client.close();
    _listeners.clear();
  }

  void _scheduleRetiredImageCleanup() {
    if (_retiredImageTimer?.isActive == true) return;
    _retiredImageTimer = Timer(const Duration(milliseconds: 120), () {
      _retiredImageTimer = null;
      for (final image in _retiredImages) {
        image.dispose();
      }
      _retiredImages.clear();
    });
  }
}

class _MjpegParser {
  final List<int> _buffer = <int>[];

  List<Uint8List> add(List<int> chunk) {
    _buffer.addAll(chunk);
    final frames = <Uint8List>[];
    while (true) {
      final headerEnd = _indexOf(_buffer, const <int>[13, 10, 13, 10]);
      if (headerEnd < 0) break;
      final header = ascii.decode(
        _buffer.sublist(0, headerEnd),
        allowInvalid: true,
      );
      final match = RegExp(
        r'content-length:\s*(\d+)',
        caseSensitive: false,
      ).firstMatch(header);
      if (match == null) {
        _buffer.removeRange(0, headerEnd + 4);
        continue;
      }
      final length = int.tryParse(match.group(1) ?? '') ?? 0;
      final bodyStart = headerEnd + 4;
      if (length <= 0 || length > 8 * 1024 * 1024) {
        _buffer.removeRange(0, bodyStart);
        continue;
      }
      if (_buffer.length < bodyStart + length) break;
      frames.add(
        Uint8List.fromList(_buffer.sublist(bodyStart, bodyStart + length)),
      );
      var consumed = bodyStart + length;
      if (_buffer.length >= consumed + 2 &&
          _buffer[consumed] == 13 &&
          _buffer[consumed + 1] == 10) {
        consumed += 2;
      }
      _buffer.removeRange(0, consumed);
    }
    return frames;
  }

  static int _indexOf(List<int> data, List<int> pattern) {
    if (pattern.isEmpty || data.length < pattern.length) return -1;
    for (var index = 0; index <= data.length - pattern.length; index++) {
      var matched = true;
      for (var offset = 0; offset < pattern.length; offset++) {
        if (data[index + offset] != pattern[offset]) {
          matched = false;
          break;
        }
      }
      if (matched) return index;
    }
    return -1;
  }
}
