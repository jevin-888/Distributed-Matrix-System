#include "common/Logger.h"
#include "master/MasterNodeApp.h"
#include "node/SlaveNodeApp.h"
#include "oled/OledDisplay.h"

#include <arpa/inet.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

using json = nlohmann::json;
using namespace dms;

namespace {
volatile std::sig_atomic_t g_stopRequested = 0;

struct LogOptions {
    std::string level = "INFO";
    std::string output;
};

void signalHandler(int) {
    g_stopRequested = 1;
}

void printUsage(const char* programName) {
    std::cout
        << "Usage: " << programName << " --role <master|slave> [options]\n\n"
        << "       " << programName << " --oled-boot  (early boot screen only)\n\n"
        << "Common options:\n"
        << "  --config <path>        JSON configuration file\n"
        << "  --cache-dir <path>     Video cache directory\n"
        << "  --help                 Show this help message\n\n"
        << "Master options:\n"
        << "  --port <1-65535>       HTTP server port\n"
        << "  --web-root <path>      Static web console directory\n\n"
        << "Network options:\n"
        << "  --ip-mode <auto|manual> IP assignment mode (default: auto)\n"
        << "  --ip <address>         Manual local IPv4 address\n\n"
        << "Slave options:\n"
        << "  --node-id <id>         Unique node ID (1-" << MAX_NODES << ")\n"
        << "  --connector-id <id>    DRM connector ID (0 selects automatically)\n"
        << "  --plane-id <id>        DRM video plane ID (0 selects automatically)\n"
        << "  --background-plane-id <id> DRM background plane ID (0 disables separation)\n"
        << "  --overlay-plane-ids <id,id,...> DRM planes for hardware window layers\n"
        << "  --capture-device <path> V4L2 capture device\n"
        << "  --idle-image <path>    Image displayed while the node is idle\n";
}

std::string optionValue(int& index, int argc, char* argv[], const std::string& name) {
    if (index + 1 >= argc) {
        throw std::invalid_argument("Missing value for " + name);
    }
    return argv[++index];
}

uint64_t parseUnsigned(const std::string& value, const std::string& name, uint64_t maxValue) {
    size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed, 10);
    if (consumed != value.size() || parsed > maxValue) {
        throw std::invalid_argument("Invalid value for " + name + ": " + value);
    }
    return parsed;
}

json loadConfig(const std::string& path) {
    if (path.empty()) {
        return json::object();
    }
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open configuration file: " + path);
    }
    json config;
    input >> config;
    if (!config.is_object()) {
        throw std::runtime_error("Configuration root must be an object: " + path);
    }
    return config;
}

void requireObject(const json& value, const std::string& path) {
    if (!value.is_object()) {
        throw std::invalid_argument(path + " must be a JSON object");
    }
}

void validateKeys(const json& object, const std::set<std::string>& allowed, const std::string& path) {
    requireObject(object, path);
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (allowed.count(it.key()) == 0) {
            throw std::invalid_argument("Unknown configuration key: " + path + "." + it.key());
        }
    }
}

uint64_t configUnsigned(const json& object,
                        const char* key,
                        uint64_t current,
                        uint64_t minimum,
                        uint64_t maximum,
                        const std::string& path) {
    if (!object.contains(key)) {
        return current;
    }
    const json& value = object.at(key);
    if (!value.is_number_unsigned()) {
        throw std::invalid_argument(path + "." + key + " must be an unsigned integer");
    }
    const uint64_t parsed = value.get<uint64_t>();
    if (parsed < minimum || parsed > maximum) {
        throw std::invalid_argument(path + "." + key + " is out of range");
    }
    return parsed;
}

uint64_t cacheSizeBytes(const json& video, uint64_t current) {
    const uint64_t megabytes = configUnsigned(video, "max_cache_size_mb",
                                               current / (1024ULL * 1024ULL),
                                               1, 1024ULL * 1024ULL, "video");
    return megabytes * 1024ULL * 1024ULL;
}

void applyNetworkConfig(const json& network, NetworkOptions& options, bool master) {
    const std::set<std::string> keys = master
        ? std::set<std::string>{"ip_mode", "local_ip", "http_port", "multicast_address", "multicast_port",
                                "heartbeat_address", "heartbeat_port", "discovery_port",
                                "discovery_networks"}
        : std::set<std::string>{"ip_mode", "local_ip", "multicast_address", "multicast_port",
                                "heartbeat_address", "heartbeat_port", "discovery_port"};
    validateKeys(network, keys, "network");
    const bool hasIpMode = network.contains("ip_mode");
    const bool hasLocalIp = network.contains("local_ip");
    if (hasIpMode) {
        if (!network.at("ip_mode").is_string() ||
            !parseIpAssignmentMode(network.at("ip_mode").get<std::string>(), options.ipMode)) {
            throw std::invalid_argument("network.ip_mode must be auto or manual");
        }
    }
    if (hasLocalIp) {
        if (!network.at("local_ip").is_string()) {
            throw std::invalid_argument("network.local_ip must be an IPv4 address or empty");
        }
        options.localIp = network.at("local_ip").get<std::string>();
        if (!options.localIp.empty()) {
            struct in_addr address {};
            if (inet_pton(AF_INET, options.localIp.c_str(), &address) != 1) {
                throw std::invalid_argument("network.local_ip must be a valid IPv4 address");
            }
        }
    }
    // Before ip_mode was introduced, a non-empty local_ip meant manual mode.
    if (!hasIpMode && hasLocalIp) {
        options.ipMode = options.localIp.empty()
            ? IpAssignmentMode::AUTO
            : IpAssignmentMode::MANUAL;
    }
    if (options.ipMode == IpAssignmentMode::MANUAL && options.localIp.empty()) {
        throw std::invalid_argument("network.local_ip is required when network.ip_mode is manual");
    }
    if (options.ipMode == IpAssignmentMode::AUTO && !options.localIp.empty()) {
        throw std::invalid_argument("network.local_ip must be empty when network.ip_mode is auto");
    }
    options.commandMulticastAddress = network.value("multicast_address", options.commandMulticastAddress);
    options.commandPort = static_cast<uint16_t>(configUnsigned(
        network, "multicast_port", options.commandPort, 1, 65535, "network"));
    options.heartbeatMulticastAddress = network.value("heartbeat_address", options.heartbeatMulticastAddress);
    options.heartbeatPort = static_cast<uint16_t>(configUnsigned(
        network, "heartbeat_port", options.heartbeatPort, 1, 65535, "network"));
    options.discoveryPort = static_cast<uint16_t>(configUnsigned(
        network, "discovery_port", options.discoveryPort, 1, 65535, "network"));
    if (master && network.contains("discovery_networks")) {
        const json& networks = network.at("discovery_networks");
        if (!networks.is_array()) {
            throw std::invalid_argument("network.discovery_networks must be an array");
        }
        options.discoveryNetworks.clear();
        for (const auto& value : networks) {
            if (!value.is_string() || value.get<std::string>().empty()) {
                throw std::invalid_argument("network.discovery_networks must contain non-empty strings");
            }
            options.discoveryNetworks.push_back(value.get<std::string>());
        }
    }
}

void applyLoggingConfig(const json& logging, LogOptions& options) {
    validateKeys(logging, {"level", "output"}, "logging");
    options.level = logging.value("level", options.level);
    options.output = logging.value("output", options.output);
}

void applyMasterConfig(const json& config, MasterNodeOptions& options, LogOptions& logging) {
    if (config.empty()) return;
    validateKeys(config, {"network", "video", "web", "sync", "layout", "logging"}, "config");
    const json network = config.value("network", json::object());
    const json video = config.value("video", json::object());
    const json sync = config.value("sync", json::object());
    const json web = config.value("web", json::object());
    const json layout = config.value("layout", json::object());
    const json log = config.value("logging", json::object());
    applyNetworkConfig(network, options.network, true);
    validateKeys(video, {"cache_directory", "max_cache_size_mb"}, "video");
    validateKeys(sync, {"sync_delay_ms"}, "sync");
    validateKeys(web, {"root_directory", "regions_file"}, "web");
    validateKeys(layout, {"rows", "cols", "screen_width", "screen_height"}, "layout");
    options.httpPort = static_cast<uint16_t>(configUnsigned(
        network, "http_port", options.httpPort, 1, 65535, "network"));
    options.cacheDir = video.value("cache_directory", options.cacheDir);
    options.maxCacheSizeBytes = cacheSizeBytes(video, options.maxCacheSizeBytes);
    options.defaultSyncDelayMs = configUnsigned(
        sync, "sync_delay_ms", options.defaultSyncDelayMs, 0, 24ULL * 60ULL * 60ULL * 1000ULL, "sync");
    options.webRoot = web.value("root_directory", options.webRoot);
    options.regionsFile = web.value("regions_file", options.regionsFile);
    options.layout.rows = static_cast<uint32_t>(configUnsigned(layout, "rows", options.layout.rows, 1, 8, "layout"));
    options.layout.cols = static_cast<uint32_t>(configUnsigned(layout, "cols", options.layout.cols, 1, 8, "layout"));
    options.layout.width = static_cast<uint32_t>(configUnsigned(layout, "screen_width", options.layout.width, 1, 16384, "layout"));
    options.layout.height = static_cast<uint32_t>(configUnsigned(layout, "screen_height", options.layout.height, 1, 16384, "layout"));
    applyLoggingConfig(log, logging);
}

void applySlaveConfig(const json& config, SlaveNodeOptions& options, bool& hasNodeId, LogOptions& logging) {
    if (config.empty()) return;
    validateKeys(config, {"node_id", "node_id_file", "node_role", "node_role_file", "network", "video",
                          "playback", "kvm", "logging"}, "config");
    const json network = config.value("network", json::object());
    const json video = config.value("video", json::object());
    const json playback = config.value("playback", json::object());
    const json capture = playback.value("capture", json::object());
    const json kvm = config.value("kvm", json::object());
    const json log = config.value("logging", json::object());
    applyNetworkConfig(network, options.network, false);
    validateKeys(video, {"cache_directory", "max_cache_size_mb"}, "video");
    validateKeys(playback, {"connector_id", "plane_id", "background_plane_id", "overlay_plane_ids",
                            "fullscreen", "startup_timeout_ms",
                            "idle_image_path", "hdmi_preview", "capture",
                            "startup_animation_path", "startup_animation_timeout_ms"},
                 "playback");
    validateKeys(capture, {"device", "pixel_format", "width", "height",
                           "framerate_numerator", "framerate_denominator", "colorimetry",
                           "audio_enabled", "audio_capture_device", "audio_output_mode",
                           "hdmi_audio_device", "hdmi_audio_volume",
                           "analog_audio_device", "analog_audio_volume"},
                 "playback.capture");
    validateKeys(kvm, {"enabled", "role", "listen_port", "input_devices",
                       "keyboard_hid_device", "mouse_hid_device"}, "kvm");
    options.nodeId = static_cast<uint32_t>(configUnsigned(
        config, "node_id", options.nodeId, 1, MAX_NODES, "config"));
    hasNodeId = options.nodeId != 0;
    options.nodeIdFile = config.value("node_id_file", options.nodeIdFile);
    if (config.contains("node_role")) {
        if (!config.at("node_role").is_string() ||
            !parseNodeRole(config.at("node_role").get<std::string>(), options.nodeRole)) {
            throw std::invalid_argument("node_role must be unassigned, encode, decode, or codec");
        }
    }
    options.nodeRoleFile = config.value("node_role_file", options.nodeRoleFile);
    options.cacheDir = video.value("cache_directory", options.cacheDir);
    options.maxCacheSizeBytes = cacheSizeBytes(video, options.maxCacheSizeBytes);
    options.player.connectorId = static_cast<uint32_t>(configUnsigned(
        playback, "connector_id", options.player.connectorId, 0,
        static_cast<uint64_t>(std::numeric_limits<int>::max()), "playback"));
    options.player.planeId = static_cast<uint32_t>(configUnsigned(
        playback, "plane_id", options.player.planeId, 0,
        static_cast<uint64_t>(std::numeric_limits<int>::max()), "playback"));
    options.player.backgroundPlaneId = static_cast<uint32_t>(configUnsigned(
        playback, "background_plane_id", options.player.backgroundPlaneId, 0,
        static_cast<uint64_t>(std::numeric_limits<int>::max()), "playback"));
    if (playback.contains("overlay_plane_ids")) {
        const json& planes = playback.at("overlay_plane_ids");
        if (!planes.is_array()) throw std::invalid_argument("playback.overlay_plane_ids must be an array");
        options.player.overlayPlaneIds.clear();
        for (const auto& plane : planes) {
            if (!plane.is_number_unsigned() || plane.get<uint64_t>() == 0 ||
                plane.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                throw std::invalid_argument("playback.overlay_plane_ids entries must be non-zero DRM IDs");
            }
            options.player.overlayPlaneIds.push_back(plane.get<uint32_t>());
        }
    }
    options.player.fullscreen = playback.value("fullscreen", options.player.fullscreen);
    options.idleImagePath = playback.value("idle_image_path", options.idleImagePath);
    options.hdmiPreview = playback.value("hdmi_preview", options.hdmiPreview);
    options.player.capture.devicePath = capture.value("device", options.player.capture.devicePath);
    options.player.capture.pixelFormat = capture.value("pixel_format", options.player.capture.pixelFormat);
    options.player.capture.width = static_cast<uint32_t>(configUnsigned(
        capture, "width", options.player.capture.width, 0, 16384, "playback.capture"));
    options.player.capture.height = static_cast<uint32_t>(configUnsigned(
        capture, "height", options.player.capture.height, 0, 16384, "playback.capture"));
    options.player.capture.framerateNumerator = static_cast<uint32_t>(configUnsigned(
        capture, "framerate_numerator", options.player.capture.framerateNumerator,
        0, 1000, "playback.capture"));
    options.player.capture.framerateDenominator = static_cast<uint32_t>(configUnsigned(
        capture, "framerate_denominator", options.player.capture.framerateDenominator,
        1, 1000, "playback.capture"));
    options.player.capture.colorimetry = capture.value("colorimetry", options.player.capture.colorimetry);
    options.player.capture.audioEnabled = capture.value("audio_enabled", options.player.capture.audioEnabled);
    options.player.capture.audioCaptureDevice = capture.value(
        "audio_capture_device", options.player.capture.audioCaptureDevice);
    if (capture.contains("audio_output_mode")) {
        const std::string mode = capture.at("audio_output_mode").get<std::string>();
        if (!parseAudioOutputMode(mode, options.player.capture.audioOutputMode)) {
            throw std::invalid_argument("playback.capture.audio_output_mode must be hdmi, analog, or both");
        }
    }
    options.player.capture.hdmiAudioOutput.device = capture.value(
        "hdmi_audio_device", options.player.capture.hdmiAudioOutput.device);
    options.player.capture.hdmiAudioOutput.volume = capture.value(
        "hdmi_audio_volume", options.player.capture.hdmiAudioOutput.volume);
    options.player.capture.analogAudioOutput.device = capture.value(
        "analog_audio_device", options.player.capture.analogAudioOutput.device);
    options.player.capture.analogAudioOutput.volume = capture.value(
        "analog_audio_volume", options.player.capture.analogAudioOutput.volume);
    options.startupAnimationPath = playback.value("startup_animation_path", options.startupAnimationPath);
    options.startupAnimationTimeoutMs = static_cast<uint32_t>(configUnsigned(
        playback, "startup_animation_timeout_ms", options.startupAnimationTimeoutMs, 1, 600000, "playback"));
    options.player.startupTimeoutMs = static_cast<uint32_t>(configUnsigned(
        playback, "startup_timeout_ms", options.player.startupTimeoutMs, 1, 600000, "playback"));
    options.kvm.enabled = kvm.value("enabled", options.kvm.enabled);
    if (kvm.contains("role")) {
        if (!kvm.at("role").is_string() ||
            !parseKvmRole(kvm.at("role").get<std::string>(), options.kvm.role)) {
            throw std::invalid_argument("kvm.role must be disabled, controller, target, or both");
        }
    }
    options.kvm.listenPort = static_cast<uint16_t>(configUnsigned(
        kvm, "listen_port", options.kvm.listenPort, 1, 65535, "kvm"));
    if (kvm.contains("input_devices")) {
        if (!kvm.at("input_devices").is_array()) {
            throw std::invalid_argument("kvm.input_devices must be an array");
        }
        options.kvm.inputDevices.clear();
        for (const auto& device : kvm.at("input_devices")) {
            if (!device.is_string() || device.get<std::string>().empty()) {
                throw std::invalid_argument("kvm.input_devices entries must be non-empty strings");
            }
            options.kvm.inputDevices.push_back(device.get<std::string>());
        }
    }
    options.kvm.keyboardHidDevice = kvm.value(
        "keyboard_hid_device", options.kvm.keyboardHidDevice);
    options.kvm.mouseHidDevice = kvm.value("mouse_hid_device", options.kvm.mouseHidDevice);
    if (options.kvm.enabled && options.kvm.role == KvmRole::DISABLED) {
        throw std::invalid_argument("kvm.enabled requires a non-disabled role");
    }
    if (!options.kvm.enabled && options.kvm.role != KvmRole::DISABLED) {
        throw std::invalid_argument("disabled KVM requires role disabled");
    }
    if (options.kvm.enabled && kvmCanControl(options.kvm.role) && options.kvm.inputDevices.empty()) {
        throw std::invalid_argument("KVM controller requires at least one input device");
    }
    if (options.kvm.enabled && kvmCanTarget(options.kvm.role) &&
        (options.kvm.keyboardHidDevice.empty() || options.kvm.mouseHidDevice.empty())) {
        throw std::invalid_argument("KVM target HID device paths must not be empty");
    }
    applyLoggingConfig(log, logging);
}

LogLevel parseLogLevel(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (value == "DEBUG") return LogLevel::DEBUG;
    if (value == "INFO") return LogLevel::INFO;
    if (value == "WARNING" || value == "WARN") return LogLevel::WARNING;
    if (value == "ERROR") return LogLevel::ERROR;
    throw std::invalid_argument("logging.level must be DEBUG, INFO, WARNING, or ERROR");
}

void configureLogger(const LogOptions& options) {
    Logger& logger = Logger::getInstance();
    logger.setLevel(parseLogLevel(options.level));
    if (!logger.setOutputFile(options.output)) {
        throw std::runtime_error("Cannot open logging.output: " + options.output);
    }
}

std::string findRole(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--role") return optionValue(i, argc, argv, "--role");
    }
    return {};
}

std::string findConfigPath(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config") return optionValue(i, argc, argv, "--config");
    }
    return {};
}

int runMaster(int argc, char* argv[], const json& config) {
    MasterNodeOptions options;
    LogOptions logging;
    applyMasterConfig(config, options, logging);
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--role" || arg == "--config") { ++i; }
        else if (arg == "--port") options.httpPort = static_cast<uint16_t>(parseUnsigned(optionValue(i, argc, argv, arg), arg, 65535));
        else if (arg == "--cache-dir") options.cacheDir = optionValue(i, argc, argv, arg);
        else if (arg == "--ip-mode") {
            if (!parseIpAssignmentMode(optionValue(i, argc, argv, arg), options.network.ipMode)) {
                throw std::invalid_argument("--ip-mode must be auto or manual");
            }
            if (options.network.ipMode == IpAssignmentMode::AUTO) options.network.localIp.clear();
        }
        else if (arg == "--ip") {
            options.network.localIp = optionValue(i, argc, argv, arg);
            options.network.ipMode = IpAssignmentMode::MANUAL;
        }
        else if (arg == "--web-root") options.webRoot = optionValue(i, argc, argv, arg);
        else throw std::invalid_argument("Unknown master option: " + arg);
    }
    if (options.httpPort == 0) throw std::invalid_argument("HTTP port must be between 1 and 65535");
    configureLogger(logging);

    MasterNodeApp app;
    if (!app.initialize(options)) {
        std::cerr << "Failed to initialize master node\n";
        return 1;
    }
    std::cout << "Master node running at " << app.getLocalHttpUrl() << " (Ctrl+C to stop)\n";
    while (!g_stopRequested && app.isRunning()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    app.shutdown();
    return 0;
}

int runSlave(int argc, char* argv[], const json& config) {
    SlaveNodeOptions options;
    bool hasNodeId = false;
    LogOptions logging;
    applySlaveConfig(config, options, hasNodeId, logging);
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--role" || arg == "--config") { ++i; }
        else if (arg == "--node-id") { options.nodeId = static_cast<uint32_t>(parseUnsigned(optionValue(i, argc, argv, arg), arg, MAX_NODES)); hasNodeId = options.nodeId != 0; }
        else if (arg == "--cache-dir") options.cacheDir = optionValue(i, argc, argv, arg);
        else if (arg == "--ip-mode") {
            if (!parseIpAssignmentMode(optionValue(i, argc, argv, arg), options.network.ipMode)) {
                throw std::invalid_argument("--ip-mode must be auto or manual");
            }
            if (options.network.ipMode == IpAssignmentMode::AUTO) options.network.localIp.clear();
        }
        else if (arg == "--ip") {
            options.network.localIp = optionValue(i, argc, argv, arg);
            options.network.ipMode = IpAssignmentMode::MANUAL;
        }
        else if (arg == "--connector-id") options.player.connectorId = static_cast<uint32_t>(
            parseUnsigned(optionValue(i, argc, argv, arg), arg,
                          static_cast<uint64_t>(std::numeric_limits<int>::max())));
        else if (arg == "--plane-id") options.player.planeId = static_cast<uint32_t>(
            parseUnsigned(optionValue(i, argc, argv, arg), arg,
                          static_cast<uint64_t>(std::numeric_limits<int>::max())));
        else if (arg == "--background-plane-id") options.player.backgroundPlaneId =
            static_cast<uint32_t>(parseUnsigned(
                optionValue(i, argc, argv, arg), arg,
                static_cast<uint64_t>(std::numeric_limits<int>::max())));
        else if (arg == "--overlay-plane-ids") {
            const std::string value = optionValue(i, argc, argv, arg);
            options.player.overlayPlaneIds.clear();
            size_t start = 0;
            while (start < value.size()) {
                const size_t comma = value.find(',', start);
                const std::string token = value.substr(start, comma == std::string::npos ? comma : comma - start);
                options.player.overlayPlaneIds.push_back(static_cast<uint32_t>(parseUnsigned(
                    token, arg, static_cast<uint64_t>(std::numeric_limits<int>::max()))));
                start = comma == std::string::npos ? value.size() : comma + 1;
            }
            if (options.player.overlayPlaneIds.empty() ||
                std::any_of(options.player.overlayPlaneIds.begin(), options.player.overlayPlaneIds.end(),
                            [](uint32_t id) { return id == 0; })) {
                throw std::invalid_argument("--overlay-plane-ids requires non-zero IDs");
            }
        }
        else if (arg == "--capture-device") options.player.capture.devicePath = optionValue(i, argc, argv, arg);
        else if (arg == "--idle-image") options.idleImagePath = optionValue(i, argc, argv, arg);
        else throw std::invalid_argument("Unknown slave option: " + arg);
    }
    if (!hasNodeId) throw std::invalid_argument("Slave role requires a non-zero --node-id or node_id in config");
    configureLogger(logging);

    SlaveNodeApp app;
    if (!app.initialize(options) || !app.start()) {
        std::cerr << "Failed to initialize/start slave node: " << app.getLastError() << "\n";
        app.shutdown();
        return 1;
    }
    std::cout << "Slave node " << app.getNodeId() << " running (Ctrl+C to stop)\n";
    while (!g_stopRequested && app.isRunning()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    app.shutdown();
    return 0;
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--oled-boot") {
            OledDisplay display;
            // Twelve 6-pixel glyphs centered within the 128-pixel second row.
            if (!display.open() || !display.showText(1, 28, "system login")) return 1;
            display.handoffBootDisplay();
            return 0;
        }
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--help") { printUsage(argv[0]); return 0; }
        }
        const std::string role = findRole(argc, argv);
        if (role.empty()) { printUsage(argv[0]); return 1; }
        const json config = loadConfig(findConfigPath(argc, argv));
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);
        if (role == "master") return runMaster(argc, argv, config);
        if (role == "slave") return runSlave(argc, argv, config);
        throw std::invalid_argument("Role must be 'master' or 'slave'");
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}

