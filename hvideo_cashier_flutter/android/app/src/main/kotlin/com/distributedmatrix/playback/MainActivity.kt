package com.distributedmatrix.playback

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import java.net.Inet4Address
import java.net.NetworkInterface

class MainActivity : FlutterActivity() {
    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(
            flutterEngine.dartExecutor.binaryMessenger,
            "distributed_playback_system/network_discovery",
        ).setMethodCallHandler { call, result ->
            if (call.method != "ipv4Networks") {
                result.notImplemented()
                return@setMethodCallHandler
            }
            try {
                val networks = NetworkInterface.getNetworkInterfaces().toList()
                    .filter { it.isUp && !it.isLoopback }
                    .flatMap { it.interfaceAddresses }
                    .filter { it.address is Inet4Address && !it.address.isLinkLocalAddress }
                    .map { mapOf("address" to it.address.hostAddress,
                        "prefixLength" to it.networkPrefixLength.toInt()) }
                result.success(networks)
            } catch (error: Exception) {
                result.error("network_interfaces", error.message, null)
            }
        }
        MethodChannel(
            flutterEngine.dartExecutor.binaryMessenger,
            "distributed_playback_system/admin_console",
        ).setMethodCallHandler { call, result ->
            if (call.method != "open") {
                result.notImplemented()
                return@setMethodCallHandler
            }
            val url = call.argument<String>("url")
            val uri = url?.let(Uri::parse)
            if (uri == null || (uri.scheme != "http" && uri.scheme != "https")) {
                result.error("invalid_url", "Only HTTP and HTTPS URLs are allowed", null)
                return@setMethodCallHandler
            }
            try {
                startActivity(Intent(Intent.ACTION_VIEW, uri))
                result.success(true)
            } catch (_: ActivityNotFoundException) {
                result.success(false)
            }
        }
    }
}
