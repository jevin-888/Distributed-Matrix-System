#include "node/SlaveNodeApp.h"
#include "common/DiscoveryProtocol.h"
#include "common/Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <ifaddrs.h>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <arpa/inet.h>
#include <cctype>

using json = nlohmann::json;

namespace dms {

namespace {

struct NetworkInterfaceInfo {
    std::string name;
    std::string subnetMask;
};

std::string readSystemText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::size_t firstNull = value.find('\0');
    if (firstNull != std::string::npos) value.resize(firstNull);
    while (!value.empty() && (value.back() == '\0' || std::isspace(static_cast<unsigned char>(value.back())))) {
        value.pop_back();
    }
    const auto first = value.find_first_not_of(" \t\r\n\0");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n\0");
    return value.substr(first, last - first + 1);
}

NetworkInterfaceInfo getPreferredNetworkInterface(const std::string& configuredIp) {
    struct ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return {};

    NetworkInterfaceInfo preferred;
    int preferredRank = std::numeric_limits<int>::max();
    for (struct ifaddrs* interface = interfaces; interface != nullptr; interface = interface->ifa_next) {
        if (!interface->ifa_addr || interface->ifa_addr->sa_family != AF_INET) continue;
        const auto* address = reinterpret_cast<const struct sockaddr_in*>(interface->ifa_addr);
        char ipBuffer[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &address->sin_addr, ipBuffer, sizeof(ipBuffer)) ||
            std::strncmp(ipBuffer, "127.", 4) == 0) {
            continue;
        }
        const char* name = interface->ifa_name ? interface->ifa_name : "";
        const bool preferredName = std::strncmp(name, "eth", 3) == 0 ||
                                   std::strncmp(name, "en", 2) == 0;
        const int rank = !configuredIp.empty() && configuredIp == ipBuffer
            ? 0
            : preferredName ? 1 : 2;
        if (rank >= preferredRank) continue;

        preferredRank = rank;
        preferred.name = name;
        if (interface->ifa_netmask) {
            const auto* netmask = reinterpret_cast<const struct sockaddr_in*>(interface->ifa_netmask);
            char maskBuffer[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &netmask->sin_addr, maskBuffer, sizeof(maskBuffer))) {
                preferred.subnetMask = maskBuffer;
            }
        }
    }
    freeifaddrs(interfaces);
    return preferred;
}

std::string readDefaultGateway() {
    std::ifstream routes("/proc/net/route");
    std::string line;
    if (!std::getline(routes, line)) return {};
    while (std::getline(routes, line)) {
        std::istringstream fields(line);
        std::string interfaceName;
        std::string destinationText;
        std::string gatewayText;
        std::string flagsText;
        if (!(fields >> interfaceName >> destinationText >> gatewayText >> flagsText)) continue;
        (void)interfaceName;
        try {
            const unsigned long flags = std::stoul(flagsText, nullptr, 16);
            const uint32_t destination = static_cast<uint32_t>(std::stoul(destinationText, nullptr, 16));
            const uint32_t gateway = static_cast<uint32_t>(std::stoul(gatewayText, nullptr, 16));
            if ((flags & 0x1U) == 0 || destination != 0 || gateway == 0) continue;
            in_addr address{};
            address.s_addr = htonl(ntohl(gateway));
            char buffer[INET_ADDRSTRLEN] = {};
            return inet_ntop(AF_INET, &address, buffer, sizeof(buffer)) ? buffer : std::string{};
        } catch (...) {
            continue;
        }
    }
    return {};
}

std::string availableOrUnknown(const std::string& value) {
    return value.empty() ? "unknown" : value;
}

std::string readActiveDrmResolution() {
    const std::filesystem::path debugRoot("/sys/kernel/debug/dri");
    std::error_code error;
    if (!std::filesystem::exists(debugRoot, error)) {
        return {};
    }

    for (const auto& entry : std::filesystem::directory_iterator(debugRoot, error)) {
        if (error || !entry.is_directory()) {
            continue;
        }

        std::ifstream stateFile(entry.path() / "state");
        if (!stateFile) {
            continue;
        }

        bool insideCrtc = false;
        bool activeCrtc = false;
        std::string line;
        while (std::getline(stateFile, line)) {
            if (line.rfind("crtc[", 0) == 0) {
                insideCrtc = true;
                activeCrtc = false;
                continue;
            }
            if (insideCrtc && (line.rfind("plane[", 0) == 0 || line.rfind("connector[", 0) == 0)) {
                insideCrtc = false;
                activeCrtc = false;
            }
            if (!insideCrtc) {
                continue;
            }
            if (line.find("active=1") != std::string::npos) {
                activeCrtc = true;
                continue;
            }
            if (!activeCrtc) {
                continue;
            }

            const std::size_t modeOffset = line.find("mode:");
            const std::size_t quoteEnd = line.rfind('"');
            if (modeOffset == std::string::npos || quoteEnd == std::string::npos) {
                continue;
            }
            const std::size_t valuesOffset = line.find(':', quoteEnd);
            if (valuesOffset == std::string::npos) {
                continue;
            }

            uint64_t refresh = 0;
            uint64_t pixelClock = 0;
            uint32_t width = 0;
            uint32_t horizontalSyncStart = 0;
            uint32_t horizontalSyncEnd = 0;
            uint32_t horizontalTotal = 0;
            uint32_t height = 0;
            std::istringstream values(line.substr(valuesOffset + 1));
            if (values >> refresh >> pixelClock >> width
                       >> horizontalSyncStart >> horizontalSyncEnd >> horizontalTotal >> height &&
                width != 0 && height != 0) {
                return std::to_string(width) + "x" + std::to_string(height);
            }
        }
    }
    return {};
}

bool parseResolution(const std::string& value, uint32_t& width, uint32_t& height) {
    const std::size_t separator = value.find('x');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size()) {
        return false;
    }
    try {
        std::size_t widthLength = 0;
        std::size_t heightLength = 0;
        const unsigned long parsedWidth = std::stoul(value.substr(0, separator), &widthLength);
        const std::string heightText = value.substr(separator + 1);
        const unsigned long parsedHeight = std::stoul(heightText, &heightLength);
        if (widthLength != separator || heightLength != heightText.size() ||
            parsedWidth == 0 || parsedWidth > 16384 ||
            parsedHeight == 0 || parsedHeight > 16384) {
            return false;
        }
        width = static_cast<uint32_t>(parsedWidth);
        height = static_cast<uint32_t>(parsedHeight);
        return true;
    } catch (...) {
        return false;
    }
}

int64_t saturatingTimestampDifference(uint64_t value, uint64_t reference) {
    constexpr uint64_t MAX_SIGNED = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (value >= reference) {
        const uint64_t difference = value - reference;
        return difference > MAX_SIGNED
            ? std::numeric_limits<int64_t>::max()
            : static_cast<int64_t>(difference);
    }

    const uint64_t difference = reference - value;
    return difference > MAX_SIGNED
        ? -std::numeric_limits<int64_t>::max()
        : -static_cast<int64_t>(difference);
}

} // namespace

SlaveNodeApp::SlaveNodeApp()
    : m_nodeId(0)
    , m_nodeRole(NodeRole::UNASSIGNED)
    , m_startupAnimationTimeoutMs(12000)
    , m_hasAppliedNetworkConfig(false)
    , m_hasActiveSignalCrop(false)
    , m_activeSignalSourceWidth(0)
    , m_activeSignalSourceHeight(0)
    , m_initialized(false)
    , m_running(false)
    , m_playState(PlayState::IDLE)
    , m_audioOutputMode(AudioOutputMode::BOTH)
    , m_prepareGeneration(0)
    , m_currentCommandId(0)
    , m_pendingCommitCommandId(0)
    , m_commitReceived(false)
    , m_syncTimestamp(0)
    , m_actualStartTimestamp(0)
    , m_previousCpuTotal(0)
    , m_previousCpuIdle(0)
    , m_previousTxBytes(0)
    , m_previousRxBytes(0)
    , m_previousNetworkSampleMs(0)
    , m_discoverySocket(-1) {
}

SlaveNodeApp::~SlaveNodeApp() {
    shutdown();
}

bool SlaveNodeApp::initialize(const SlaveNodeOptions& options) {
    if (m_initialized.load()) {
        LOG_WARNING("SlaveNodeApp is already initialized");
        return false;
    }
    if (options.nodeId == 0 || options.nodeId > MAX_NODES) {
        setError("Node ID must be between 1 and " + std::to_string(MAX_NODES));
        return false;
    }
    if (options.cacheDir.empty() || options.maxCacheSizeBytes == 0 ||
        options.startupAnimationTimeoutMs == 0 ||
        options.network.commandMulticastAddress.empty() || options.network.commandPort == 0 ||
        options.network.heartbeatMulticastAddress.empty() || options.network.heartbeatPort == 0 ||
        options.network.discoveryPort == 0) {
        setError("Slave options are incomplete");
        return false;
    }
    struct in_addr multicastAddress {};
    if (inet_pton(AF_INET, options.network.commandMulticastAddress.c_str(), &multicastAddress) != 1 ||
        !IN_MULTICAST(ntohl(multicastAddress.s_addr)) ||
        inet_pton(AF_INET, options.network.heartbeatMulticastAddress.c_str(), &multicastAddress) != 1 ||
        !IN_MULTICAST(ntohl(multicastAddress.s_addr))) {
        setError("Invalid multicast configuration");
        return false;
    }

    m_nodeId.store(options.nodeId);
    m_nodeIdFile = options.nodeIdFile;
    if (!loadNodeId()) {
        setError("Invalid persisted node ID");
        return false;
    }
    m_nodeRole.store(options.nodeRole);
    m_nodeRoleFile = options.nodeRoleFile;
    loadNodeRole();
    m_cacheDir = options.cacheDir;
    m_idleImagePath = options.idleImagePath;
    m_startupAnimationPath = options.startupAnimationPath;
    m_startupAnimationTimeoutMs = options.startupAnimationTimeoutMs;
    m_networkOptions = options.network;
    m_networkConfigFile = options.networkConfigFile;
    m_appliedNetworkConfig = {};
    m_hasAppliedNetworkConfig = false;
    if (!loadNetworkConfig()) {
        setError("Invalid persisted node network configuration");
        return false;
    }
    m_sourceConfigFile = options.sourceConfigFile;
    m_sourceConfig = {};
    m_hasActiveSignalCrop = false;
    m_activeSignalSourceWidth = 0;
    m_activeSignalSourceHeight = 0;
    m_activeSignalCrop = {};
    if (!loadSourceConfig()) {
        setError("Invalid persisted node signal source configuration");
        return false;
    }
    std::error_code sourceConfigError;
    const bool hasPersistedSourceConfig =
        !m_sourceConfigFile.empty() &&
        std::filesystem::exists(m_sourceConfigFile, sourceConfigError);
    if (options.hdmiPreview && !hasPersistedSourceConfig &&
        m_sourceConfig.type == SignalSourceType::NONE) {
        const CaptureOptions& capture = options.player.capture;
        SignalSourceConfig source;
        source.type = SignalSourceType::CAPTURE;
        source.endpoint = capture.devicePath;
        source.width = capture.width;
        source.height = capture.height;
        source.framerateNumerator = capture.framerateNumerator;
        source.framerateDenominator = capture.framerateDenominator;
        source.pixelFormat = capture.pixelFormat;
        std::string sourceError;
        if (!Protocol::validateSignalSourceConfig(source, &sourceError)) {
            setError("Invalid HDMI preview capture source: " + sourceError);
            return false;
        }
        m_sourceConfig = std::move(source);
        if (m_sourceConfig.width != 0 && m_sourceConfig.height != 0) {
            m_activeSignalSourceWidth = m_sourceConfig.width;
            m_activeSignalSourceHeight = m_sourceConfig.height;
            m_activeSignalCrop = {
                m_nodeId.load(), 0, 0, m_sourceConfig.width, m_sourceConfig.height};
            m_hasActiveSignalCrop = true;
        }
        LOG_INFO("HDMI preview source enabled from playback.capture: device=%s size=%ux%u",
                 m_sourceConfig.endpoint.c_str(), m_sourceConfig.width,
                 m_sourceConfig.height);
    }
    m_audioOutputMode.store(options.player.capture.audioOutputMode);
    m_masterClock.reset();

    m_kvmAgent = std::make_unique<KvmAgent>();
    if (!m_kvmAgent->initialize(m_nodeId.load(), options.kvm)) {
        setError("Failed to initialize KVM agent");
        shutdown();
        return false;
    }
    if (m_networkOptions.ipMode == IpAssignmentMode::MANUAL) {
        struct in_addr address {};
        if (m_networkOptions.localIp.empty() ||
            inet_pton(AF_INET, m_networkOptions.localIp.c_str(), &address) != 1) {
            setError("Manual IP mode requires a valid local IPv4 address");
            return false;
        }
    } else if (!m_networkOptions.localIp.empty()) {
        setError("Automatic IP mode requires an empty local IPv4 address");
        return false;
    }

    m_cacheManager = std::make_unique<VideoCacheManager>();
    if (!m_cacheManager->initialize(options.cacheDir.c_str(), options.maxCacheSizeBytes)) {
        setError("Failed to initialize VideoCacheManager");
        shutdown();
        return false;
    }

    // Power and reset the OLED before the media pipeline starts.
    if (m_oled.open()) {
        const std::string sn = getSerialNumber();
        const std::string ip = getLocalIp();
        const auto rates = getNetworkRatesMbps();
        if (m_oled.showSystemInfo(sn, ip, rates.first, rates.second, m_nodeId.load(),
                                  nodeRoleOledLabel(m_nodeRole.load()))) {
            LOG_INFO("OLED: showing SN=%s IP=%s TX=%sMbps RX=%sMbps",
                     sn.c_str(), ip.c_str(), rates.first.c_str(), rates.second.c_str());
        } else {
            LOG_WARNING("OLED: initial display update failed");
            m_oled.close();
        }
    }

    MediaPlayerOptions playerOptions = options.player;
    const std::string outputResolution = getDisplayResolution();
    if (!parseResolution(outputResolution, playerOptions.outputWidth,
                         playerOptions.outputHeight)) {
        LOG_WARNING("Cannot determine active DRM output size; KMS will negotiate its native size");
    }
    m_player = std::make_unique<MediaPlayer>();
    if (!m_player->initialize(m_nodeId.load(), playerOptions)) {
        setError(m_player->getLastError());
        shutdown();
        return false;
    }
    m_kvmAgent->setPreviewFrameProvider([this]() {
        return m_player ? m_player->getLatestPreviewFrame()
                        : std::shared_ptr<const std::vector<uint8_t>>{};
    });

    m_receiver = std::make_unique<CommandReceiver>();
    auto callback = [this](const Protocol::Packet& packet) {
        const uint64_t localReceiveTimestamp = Protocol::getCurrentTimestamp();
        if (!m_masterClock.synchronizeTo(packet.header.timestamp, localReceiveTimestamp)) {
            setError("Invalid master command timestamp");
            return;
        }
        LOG_INFO("Master clock synchronized: offset=%lld ms",
                 static_cast<long long>(m_masterClock.getTimeOffset()));

        const auto commandType = static_cast<CommandType>(packet.header.commandType);
        const std::string jsonCommand(packet.payload.begin(), packet.payload.end());
        switch (commandType) {
            case CommandType::PREPARE_PLAY: {
                Protocol::SyncPlayCommand command;
                if (packet.header.sequenceId == 0 ||
                    !Protocol::parsePreparePlayCommand(jsonCommand, command)) {
                    setError("Failed to parse prepare command");
                    return;
                }
                onPreparePlayCommand(packet.header.sequenceId, command);
                break;
            }
            case CommandType::COMMIT_PLAY: {
                Protocol::CommitPlayCommand command;
                if (!Protocol::parseCommitPlayCommand(jsonCommand, command)) {
                    setError("Failed to parse commit command");
                    return;
                }
                onCommitPlayCommand(command.commandId);
                break;
            }
            case CommandType::SYNC_PLAY: {
                CommandType parsedType;
                Protocol::SyncPlayCommand command;
                if (!Protocol::parseJsonCommand(jsonCommand, parsedType, command) ||
                    parsedType != CommandType::SYNC_PLAY) {
                    setError("Failed to parse legacy sync command");
                    return;
                }
                onSyncPlayCommand(command.videoUrl, command.videoWidth, command.videoHeight,
                                  command.syncTimestamp, command.crops);
                break;
            }
            case CommandType::STOP:
            case CommandType::PAUSE:
            case CommandType::RESUME: {
                CommandType parsedType;
                Protocol::SyncPlayCommand unused;
                if (!Protocol::parseJsonCommand(jsonCommand, parsedType, unused) ||
                    parsedType != commandType) {
                    setError("Command header and JSON payload do not match");
                    return;
                }
                if (commandType == CommandType::STOP) {
                    onStopCommand();
                } else if (commandType == CommandType::PAUSE) {
                    onPauseCommand();
                } else {
                    onResumeCommand();
                }
                break;
            }
            case CommandType::SET_AUDIO_OUTPUT: {
                Protocol::SetAudioOutputCommand command;
                if (!Protocol::parseSetAudioOutputCommand(jsonCommand, command)) {
                    setError("Failed to parse set audio output command");
                    return;
                }
                onSetAudioOutputCommand(command.mode);
                break;
            }
            case CommandType::SET_AUDIO_VOLUME: {
                Protocol::SetAudioVolumeCommand command;
                if (!Protocol::parseSetAudioVolumeCommand(jsonCommand, command)) {
                    setError("Failed to parse set audio volume command");
                    return;
                }
                onSetAudioVolumeCommand(command.volumePercent);
                break;
            }
            case CommandType::KVM_ROUTE: {
                Protocol::KvmRouteCommand command;
                if (!Protocol::parseKvmRouteCommand(jsonCommand, command)) {
                    setError("Failed to parse KVM route command");
                    return;
                }
                if (m_kvmAgent) m_kvmAgent->applyRoute(command);
                break;
            }
            case CommandType::KVM_RELEASE: {
                Protocol::KvmReleaseCommand command;
                if (!Protocol::parseKvmReleaseCommand(jsonCommand, command)) {
                    setError("Failed to parse KVM release command");
                    return;
                }
                if (m_kvmAgent) m_kvmAgent->releaseSession(command.sessionId);
                break;
            }
            case CommandType::SET_NODE_ROLE: {
                Protocol::SetNodeRoleCommand command;
                if (!Protocol::parseSetNodeRoleCommand(jsonCommand, command)) {
                    setError("Failed to parse set node role command");
                    return;
                }
                if (command.nodeId == m_nodeId.load() && !setNodeRole(command.role, true)) {
                    setError("Failed to persist node role");
                }
                break;
            }
            case CommandType::SET_NODE_NETWORK: {
                Protocol::SetNodeNetworkCommand command;
                if (!Protocol::parseSetNodeNetworkCommand(jsonCommand, command)) {
                    setError("Failed to parse set node network command");
                    return;
                }
                if (command.nodeId == m_nodeId.load() && !setNodeNetwork(command)) {
                    setError("Failed to persist node network configuration");
                }
                break;
            }
            case CommandType::SET_DISPLAY_LAYOUT: {
                Protocol::SetDisplayLayoutCommand command;
                if (!Protocol::parseSetDisplayLayoutCommand(jsonCommand, command)) {
                    setError("Failed to parse display layout command");
                    return;
                }
                onSetDisplayLayoutCommand(command);
                break;
            }
            case CommandType::SET_WINDOW_LAYOUT: {
                Protocol::SetWindowLayoutCommand command;
                if (!Protocol::parseSetWindowLayoutCommand(jsonCommand, command)) {
                    setError("Failed to parse window layout command");
                    return;
                }
                if (command.targetNodeId == m_nodeId.load()) {
                    onSetWindowLayoutCommand(command);
                }
                break;
            }
            case CommandType::SET_NODE_SOURCE: {
                Protocol::SetNodeSourceCommand command;
                if (!Protocol::parseSetNodeSourceCommand(jsonCommand, command)) {
                    setError("Failed to parse node signal source command");
                    return;
                }
                onSetNodeSourceCommand(command);
                break;
            }
            case CommandType::SET_NODE_ID: {
                Protocol::SetNodeIdCommand command;
                if (!Protocol::parseSetNodeIdCommand(jsonCommand, command)) {
                    setError("Failed to parse node ID assignment command");
                    return;
                }
                onSetNodeIdCommand(command);
                break;
            }
            default:
                LOG_WARNING("Unsupported command type: %u", packet.header.commandType);
                break;
        }
    };

    if (!m_receiver->initialize(options.network.commandMulticastAddress.c_str(),
                                options.network.commandPort, callback)) {
        setError("Failed to initialize CommandReceiver");
        shutdown();
        return false;
    }

    m_playState.store(PlayState::IDLE);
    m_initialized.store(true);

    LOG_INFO("SlaveNodeApp initialized: nodeId=%u, cacheDir=%s",
             m_nodeId.load(), options.cacheDir.c_str());
    return true;
}

bool SlaveNodeApp::start() {
    if (!m_initialized.load()) {
        setError("SlaveNodeApp is not initialized");
        return false;
    }

    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) {
        LOG_WARNING("SlaveNodeApp is already running");
        return false;
    }

    showStartupAnimation();
    // Refresh once after startup, when DHCP normally has an IPv4 lease.
    if (m_oled.isOpen()) {
        const std::string sn = getSerialNumber();
        const std::string ip = getLocalIp();
        const auto rates = getNetworkRatesMbps();
        if (!m_oled.showSystemInfo(sn, ip, rates.first, rates.second, m_nodeId.load(),
                                   nodeRoleOledLabel(m_nodeRole.load()))) {
            LOG_WARNING("OLED: startup refresh failed");
            m_oled.close();
        }
    }
    if (!showIdleImage()) {
        m_running.store(false);
        return false;
    }
    if (!m_receiver || !m_receiver->start()) {
        m_player->stop();
        m_running.store(false);
        setError("Failed to start CommandReceiver");
        return false;
    }

    m_discoverySocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_discoverySocket < 0) {
        setError(std::string("Failed to create discovery socket: ") + std::strerror(errno));
        shutdown();
        return false;
    }
    int reuse = 1;
    timeval receiveTimeout{};
    receiveTimeout.tv_usec = 200000;
    sockaddr_in discoveryAddress{};
    discoveryAddress.sin_family = AF_INET;
    discoveryAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    discoveryAddress.sin_port = htons(m_networkOptions.discoveryPort);
    if (setsockopt(m_discoverySocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        setsockopt(m_discoverySocket, SOL_SOCKET, SO_RCVTIMEO,
                   &receiveTimeout, sizeof(receiveTimeout)) < 0 ||
        bind(m_discoverySocket, reinterpret_cast<sockaddr*>(&discoveryAddress),
             sizeof(discoveryAddress)) < 0) {
        setError(std::string("Failed to bind discovery UDP port: ") + std::strerror(errno));
        shutdown();
        return false;
    }
    if (m_kvmAgent && !m_kvmAgent->start()) {
        setError("Failed to start KVM agent");
        shutdown();
        return false;
    }

    try {
        m_schedulerThread = std::thread(&SlaveNodeApp::schedulerLoop, this);
        m_heartbeatThread = std::thread(&SlaveNodeApp::heartbeatThread, this);
        m_discoveryThread = std::thread(&SlaveNodeApp::discoveryThread, this);
    } catch (const std::exception& error) {
        setError(std::string("Failed to start slave worker threads: ") + error.what());
        shutdown();
        return false;
    }
    LOG_INFO("SlaveNodeApp started: nodeId=%u", m_nodeId.load());
    return true;
}

void SlaveNodeApp::shutdown() {
    m_running.store(false);
    m_prepareGeneration.fetch_add(1);
    if (m_kvmAgent) {
        m_kvmAgent->shutdown();
    }
    if (m_receiver) {
        m_receiver->shutdown();
    }
    if (m_cacheManager) {
        m_cacheManager->shutdown();
    }

    std::vector<std::future<void>> tasks;
    {
        std::lock_guard<std::mutex> lock(m_prepareTasksMutex);
        tasks.swap(m_prepareTasks);
    }
    for (auto& task : tasks) {
        if (task.valid()) {
            try {
                task.get();
            } catch (const std::exception& error) {
                LOG_ERROR("Video preparation task failed: %s", error.what());
            }
        }
    }

    stopPlayback(false);
    if (m_schedulerThread.joinable() && m_schedulerThread.get_id() != std::this_thread::get_id()) {
        m_schedulerThread.join();
    }
    if (m_heartbeatThread.joinable() && m_heartbeatThread.get_id() != std::this_thread::get_id()) {
        m_heartbeatThread.join();
    }
    if (m_discoveryThread.joinable() && m_discoveryThread.get_id() != std::this_thread::get_id()) {
        m_discoveryThread.join();
    }
    if (m_discoverySocket >= 0) {
        close(m_discoverySocket);
        m_discoverySocket = -1;
    }

    const bool wasInitialized = m_initialized.exchange(false);
    if (wasInitialized) {
        LOG_INFO("SlaveNodeApp shutdown: nodeId=%u", m_nodeId.load());
    }
}

void SlaveNodeApp::onPreparePlayCommand(uint32_t commandId,
                                         const Protocol::SyncPlayCommand& command) {
    LOG_INFO("Received prepare_play command: id=%u, url=%s, size=%ux%u, timestamp=%llu, crops=%zu",
             commandId, command.videoUrl.c_str(), command.videoWidth, command.videoHeight,
             static_cast<unsigned long long>(command.syncTimestamp), command.crops.size());

    const auto cropIt = std::find_if(command.crops.begin(), command.crops.end(), [this](const auto& crop) {
        return crop.nodeId == m_nodeId.load();
    });
    if (cropIt == command.crops.end()) {
        LOG_WARNING("No crop region found for nodeId=%u; prepare ignored", m_nodeId.load());
        return;
    }

    const uint32_t queuedCommitId = m_pendingCommitCommandId.load();
    const bool queuedCommit = m_commitReceived.load();
    const uint64_t generation = m_prepareGeneration.fetch_add(1) + 1;
    if (m_playState.load() != PlayState::IDLE) {
        stopPlayback(false);
    }
    {
        std::lock_guard<std::mutex> displayLock(m_displayModeMutex);
        m_playState.store(PlayState::PREPARING);
    }
    m_currentCommandId.store(commandId);
    const bool commitMatchesPrepare = commandId != 0 && queuedCommit && queuedCommitId == commandId;
    m_pendingCommitCommandId.store(commitMatchesPrepare ? commandId : 0);
    m_commitReceived.store(commitMatchesPrepare);
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_currentVideoUrl = command.videoUrl;
        m_syncTimestamp = command.syncTimestamp;
        m_actualStartTimestamp = 0;
        m_lastError.clear();
    }

    try {
        std::lock_guard<std::mutex> lock(m_prepareTasksMutex);
        m_prepareTasks.emplace_back(std::async(std::launch::async,
            [this, command, crop = *cropIt, generation] {
                if (prepareVideo(command.videoUrl, command.videoWidth, command.videoHeight,
                                 crop, generation) &&
                    m_running.load() && m_prepareGeneration.load() == generation) {
                    m_playState.store(PlayState::READY);
                    LOG_INFO("Video prepared; waiting for COMMIT_PLAY id=%u at timestamp %llu",
                             m_currentCommandId.load(),
                             static_cast<unsigned long long>(command.syncTimestamp));
                } else if (m_running.load() && m_prepareGeneration.load() == generation) {
                    showIdleImage();
                    m_playState.store(PlayState::ERROR);
                }
            }));
    } catch (const std::exception& error) {
        setError(std::string("Failed to start preparation task: ") + error.what());
        m_playState.store(PlayState::ERROR);
    }
}

void SlaveNodeApp::onCommitPlayCommand(uint32_t commandId) {
    if (commandId == 0) {
        LOG_WARNING("Ignoring COMMIT_PLAY with zero command id");
        return;
    }
    const uint32_t currentCommandId = m_currentCommandId.load();
    if (currentCommandId != 0 && commandId != currentCommandId) {
        LOG_WARNING("Ignoring COMMIT_PLAY for unknown command id=%u (current=%u)",
                    commandId, currentCommandId);
        return;
    }
    m_pendingCommitCommandId.store(commandId);
    m_commitReceived.store(true);
    LOG_INFO("Received COMMIT_PLAY id=%u; state=%s", commandId,
             playStateName(m_playState.load()));
}

void SlaveNodeApp::onSyncPlayCommand(const std::string& videoUrl,
                                     uint32_t videoWidth,
                                     uint32_t videoHeight,
                                     uint64_t syncTimestamp,
                                     const std::vector<Protocol::CropRegion>& crops) {
    Protocol::SyncPlayCommand command;
    command.videoUrl = videoUrl;
    command.videoWidth = videoWidth;
    command.videoHeight = videoHeight;
    command.syncTimestamp = syncTimestamp;
    command.crops = crops;
    onPreparePlayCommand(0, command);
    m_pendingCommitCommandId.store(0);
    m_commitReceived.store(true);
}

void SlaveNodeApp::onStopCommand() {
    LOG_INFO("Received stop command");
    m_prepareGeneration.fetch_add(1);
    stopPlayback();
}

void SlaveNodeApp::onPauseCommand() {
    LOG_INFO("Received pause command");
    pausePlayback();
}

void SlaveNodeApp::onResumeCommand() {
    LOG_INFO("Received resume command");
    resumePlayback();
}

void SlaveNodeApp::onSetAudioOutputCommand(AudioOutputMode mode) {
    if (!m_player || !m_player->setCaptureAudioOutputMode(mode)) {
        setError("Failed to switch capture audio output to " +
                 std::string(audioOutputModeName(mode)));
        return;
    }
    m_audioOutputMode.store(mode);
    LOG_INFO("Capture audio output switched to %s", audioOutputModeName(mode));
}

void SlaveNodeApp::onSetAudioVolumeCommand(uint32_t volumePercent) {
    if (!m_player || !m_player->setCaptureAudioVolumePercent(volumePercent)) {
        setError("Failed to set capture audio volume");
        return;
    }
    LOG_INFO("Capture audio volume set to %u%%", volumePercent);
}

void SlaveNodeApp::onSetDisplayLayoutCommand(
    const Protocol::SetDisplayLayoutCommand& command) {
    std::lock_guard<std::mutex> displayLock(m_displayModeMutex);
    Protocol::CropRegion crop{};
    const auto cropIt = std::find_if(command.crops.begin(), command.crops.end(),
        [this](const Protocol::CropRegion& value) { return value.nodeId == m_nodeId.load(); });
    const bool selected = cropIt != command.crops.end();
    if (selected) crop = *cropIt;
    LOG_INFO("Received SET_DISPLAY_LAYOUT: node=%u source=%ux%u crops=%zu selected=%s",
             m_nodeId.load(), command.sourceWidth, command.sourceHeight, command.crops.size(),
             selected ? "true" : "false");

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_activeSignalSourceWidth = command.sourceWidth;
        m_activeSignalSourceHeight = command.sourceHeight;
        m_activeSignalCrop = crop;
        m_hasActiveSignalCrop = selected;
    }

    if (!selected || m_sourceConfig.type == SignalSourceType::NONE) {
        if (m_running.load() && !showIdleImage()) {
            m_playState.store(PlayState::ERROR);
        }
        LOG_INFO("Display layout cleared for node %u; background remains visible and signal "
                 "preview stays available", m_nodeId.load());
        return;
    }

    if (m_player && m_player->isCaptureRunning()) {
        if (m_player->isSignalOutputActive() &&
            m_player->updateSignalCrop(command.sourceWidth, command.sourceHeight, crop)) {
            LOG_INFO("Display layout applied live for node %u", m_nodeId.load());
            return;
        }
        if (!m_player->isSignalOutputActive() && m_player->setSignalOutputActive(true) &&
            m_player->updateSignalCrop(command.sourceWidth, command.sourceHeight, crop)) {
            LOG_INFO("Signal preview promoted to physical output for node %u", m_nodeId.load());
            return;
        }
    }
    // showSignal() tears down the preview-only capture pipeline before
    // creating the physical KMS branch. Do not probe the V4L2 device first:
    // that probe would see the still-running preview pipeline as busy.
    if (!m_player || !m_player->showSignal(m_sourceConfig) ||
        !m_player->updateSignalCrop(command.sourceWidth, command.sourceHeight, crop)) {
        setError(m_player ? m_player->getLastError() : "Media player is unavailable");
        if (!showIdleFallbackImage()) m_playState.store(PlayState::ERROR);
    }
}

void SlaveNodeApp::onSetWindowLayoutCommand(
    const Protocol::SetWindowLayoutCommand& command) {
    std::lock_guard<std::mutex> displayLock(m_displayModeMutex);
    LOG_INFO("Received SET_WINDOW_LAYOUT: node=%u sources=%zu layers=%zu output=%ux%u",
             m_nodeId.load(), command.sources.size(), command.layers.size(),
             command.outputWidth, command.outputHeight);
    if (command.layers.empty()) {
        if (m_player) m_player->stop();
        if (m_running.load() && !showIdleImage()) m_playState.store(PlayState::ERROR);
        return;
    }
    if (!m_player || !m_player->showWindowLayout(command)) {
        setError(m_player ? m_player->getLastError() : "Media player is unavailable");
        if (!showIdleFallbackImage()) m_playState.store(PlayState::ERROR);
        return;
    }
    m_playState.store(PlayState::PLAYING);
}

void SlaveNodeApp::onSetNodeSourceCommand(
    const Protocol::SetNodeSourceCommand& command) {
    if (command.nodeId != m_nodeId.load()) return;
    std::string error;
    if (!Protocol::validateSignalSourceConfig(command.source, &error) ||
        !persistSourceConfig(command.source)) {
        setError(error.empty() ? "Failed to persist node signal source" : error);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_sourceConfig = command.source;
    }
    std::lock_guard<std::mutex> displayLock(m_displayModeMutex);
    if (m_running.load()) {
        if (m_player) m_player->stop();
        if (!showIdleImage()) m_playState.store(PlayState::ERROR);
    }
    LOG_INFO("Node signal source updated: nodeId=%u type=%s endpoint=%s",
             m_nodeId.load(), signalSourceTypeName(command.source.type), command.source.endpoint.c_str());
}

void SlaveNodeApp::onSetNodeIdCommand(
    const Protocol::SetNodeIdCommand& command) {
    const uint32_t currentNodeId = m_nodeId.load();
    if (command.currentNodeId != currentNodeId || command.newNodeId == 0 ||
        command.newNodeId > MAX_NODES || command.targetMacAddress != getMacAddress()) {
        return;
    }
    if (!persistNodeId(command.newNodeId)) {
        setError("Failed to persist assigned node ID");
        return;
    }
    if (command.newNodeId == currentNodeId) return;
    if ((m_player && !m_player->setNodeId(command.newNodeId)) ||
        (m_kvmAgent && !m_kvmAgent->setNodeId(command.newNodeId))) {
        setError("Failed to apply assigned node ID");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_hasActiveSignalCrop) m_activeSignalCrop.nodeId = command.newNodeId;
    }
    m_nodeId.store(command.newNodeId);
    LOG_INFO("Node ID reassigned: old=%u new=%u mac=%s",
             currentNodeId, command.newNodeId, command.targetMacAddress.c_str());
    broadcastHeartbeat();
}

bool SlaveNodeApp::prepareVideo(const std::string& videoUrl,
                                uint32_t videoWidth,
                                uint32_t videoHeight,
                                const Protocol::CropRegion& cropRegion,
                                uint64_t generation) {
    std::string localPath;
    if (!m_cacheManager || !m_cacheManager->getVideo(videoUrl, localPath)) {
        if (m_running.load() && m_prepareGeneration.load() == generation) {
            setError("Failed to cache video: " + videoUrl);
        }
        return false;
    }
    if (!m_running.load() || m_prepareGeneration.load() != generation) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_currentVideoPath = localPath;
        m_currentCropRegion = cropRegion;
    }

    if (!m_running.load() || m_prepareGeneration.load() != generation) {
        return false;
    }
    if (!m_player || !m_player->prepare(localPath, videoWidth, videoHeight, cropRegion)) {
        setError(m_player ? m_player->getLastError() : "Media player is unavailable");
        return false;
    }

    LOG_INFO("Video prepared: %s -> %s", videoUrl.c_str(), localPath.c_str());
    return true;
}

bool SlaveNodeApp::startPlayback(uint64_t syncTimestamp) {
    if ((m_playState.load() != PlayState::READY && m_playState.load() != PlayState::READY_PAUSED) ||
        !m_player || !m_player->isPrepared()) {
        return false;
    }

    m_masterClock.waitUntilTimestamp(syncTimestamp, 1);
    const int64_t requestSkewMs = saturatingTimestampDifference(
        m_masterClock.getCurrentTimestamp(), syncTimestamp);

    if (!m_player->play()) {
        setError(m_player->getLastError());
        showIdleImage();
        return false;
    }

    const int64_t transitionSkewMs = saturatingTimestampDifference(
        m_masterClock.getCurrentTimestamp(), syncTimestamp);
    if (std::llabs(transitionSkewMs) > 20) {
        LOG_WARNING("Synchronized start skew exceeded tolerance: request=%lld ms, transition=%lld ms",
                    static_cast<long long>(requestSkewMs),
                    static_cast<long long>(transitionSkewMs));
    } else {
        LOG_INFO("Synchronized start skew: request=%lld ms, transition=%lld ms",
                 static_cast<long long>(requestSkewMs),
                 static_cast<long long>(transitionSkewMs));
    }

    if (m_playbackThread.joinable()) {
        m_playbackThread.join();
    }
    m_playState.store(PlayState::PLAYING);
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_actualStartTimestamp = m_masterClock.getCurrentTimestamp();
    }
    m_playbackThread = std::thread(&SlaveNodeApp::playbackMonitorLoop, this);
    return true;
}

bool SlaveNodeApp::showStartupAnimation() {
    if (m_startupAnimationPath.empty()) {
        return true;
    }
    if (!m_player || !m_player->playStartupAnimation(
            m_startupAnimationPath, m_startupAnimationTimeoutMs)) {
        LOG_WARNING("Startup animation skipped: %s",
                    m_player ? m_player->getLastError().c_str() : "Media player is unavailable");
        return false;
    }
    return true;
}

bool SlaveNodeApp::showIdleImage() {
    SignalSourceConfig source;
    Protocol::CropRegion crop{};
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    bool shouldShowSignal = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        source = m_sourceConfig;
        crop = m_activeSignalCrop;
        sourceWidth = m_activeSignalSourceWidth;
        sourceHeight = m_activeSignalSourceHeight;
        shouldShowSignal = m_hasActiveSignalCrop && source.type != SignalSourceType::NONE;
    }
    if (shouldShowSignal && m_player) {
        if (m_player->isCaptureRunning()) {
            if (m_player->isSignalOutputActive() &&
                m_player->updateSignalCrop(sourceWidth, sourceHeight, crop)) {
                return true;
            }
            if (!m_player->isSignalOutputActive() && m_player->setSignalOutputActive(true) &&
                m_player->updateSignalCrop(sourceWidth, sourceHeight, crop)) {
                return true;
            }
        }
        // Only replace the stable idle image after V4L2 reports a locked,
        // usable format. Starting v4l2src without a signal blanks kmssink;
        // the watchdog then tears it down and the scheduler would loop.
        if (m_player->captureAvailable(source) && m_player->showSignal(source) &&
            m_player->updateSignalCrop(sourceWidth, sourceHeight, crop)) {
            return true;
        }
        LOG_WARNING("Configured signal is unavailable: %s",
                    m_player->getLastError().c_str());
    }

    // Disable the physical sink before creating the idle background. The
    // deployed DRM driver keeps the last framebuffer until the next plane
    // commit, so reversing this order causes stale video and flicker.
    if (!shouldShowSignal && m_player && m_player->isCaptureRunning() &&
        m_player->isSignalOutputActive()) {
        if (!m_player->setSignalOutputActive(false)) {
            LOG_WARNING("Could not disable physical signal output before showing idle background: %s",
                        m_player->getLastError().c_str());
        }
    }

    const bool backgroundVisible = showIdleFallbackImage();
    if (source.type == SignalSourceType::NONE || !m_player) {
        return backgroundVisible;
    }

    // A configured source remains captured while it is not placed on the
    // wall. That keeps node-list and canvas previews live without assigning
    // the source to the DRM video plane or playing its audio.
    if (m_player->isCaptureRunning()) {
        if (!m_player->isSignalOutputActive()) return backgroundVisible;
        if (m_player->setSignalOutputActive(false)) return backgroundVisible;
    }
    if (m_player->captureAvailable(source) && m_player->showSignalPreview(source)) {
        // A headless or mode-less DRM connector can reject the idle image
        // while the HDMI capture/preview path is healthy. Keep the node
        // alive so it can still receive output-wall window commands.
        return true;
    }
    LOG_WARNING("Configured signal preview is unavailable: %s",
                m_player->getLastError().c_str());
    return backgroundVisible;
}

bool SlaveNodeApp::showIdleFallbackImage() {
    if (m_idleImagePath.empty()) {
        LOG_WARNING("Idle image display is disabled because playback.idle_image_path is empty");
        return false;
    }
    if (!m_player) {
        setError("Media player is unavailable");
        return false;
    }
    // The scheduler polls signal state periodically. Reusing the existing
    // image pipeline prevents a visible KMS blanking interval on every poll.
    if (m_player->isImageRunning()) {
        return true;
    }
    if (!m_player->showImage(m_idleImagePath, getLocalIp())) {
        setError(m_player->getLastError());
        return false;
    }
    return true;
}

void SlaveNodeApp::stopPlayback(bool restoreIdleImage) {
    m_playState.store(PlayState::IDLE);
    m_commitReceived.store(false);
    m_pendingCommitCommandId.store(0);
    m_currentCommandId.store(0);
    if (m_player) {
        m_player->stop();
    }
    if (m_playbackThread.joinable() && m_playbackThread.get_id() != std::this_thread::get_id()) {
        m_playbackThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_currentVideoUrl.clear();
        m_currentVideoPath.clear();
        m_syncTimestamp = 0;
        m_actualStartTimestamp = 0;
    }

    if (restoreIdleImage && m_running.load()) {
        std::lock_guard<std::mutex> displayLock(m_displayModeMutex);
        if (!showIdleImage()) {
            m_playState.store(PlayState::ERROR);
        }
    }
}

void SlaveNodeApp::pausePlayback() {
    if (m_playState.load() == PlayState::READY) {
        m_playState.store(PlayState::READY_PAUSED);
        return;
    }
    if (m_playState.load() != PlayState::PLAYING) {
        LOG_WARNING("Pause ignored because playback is not active");
        return;
    }
    if (!m_player || !m_player->pause()) {
        setError(m_player ? m_player->getLastError() : "Media player is unavailable");
        m_playState.store(PlayState::ERROR);
        return;
    }
    m_playState.store(PlayState::PAUSED);
}

void SlaveNodeApp::resumePlayback() {
    if (m_playState.load() == PlayState::READY_PAUSED) {
        m_playState.store(PlayState::READY);
        return;
    }
    if (m_playState.load() != PlayState::PAUSED) {
        LOG_WARNING("Resume ignored because playback is not paused");
        return;
    }
    if (!m_player || !m_player->resume()) {
        setError(m_player ? m_player->getLastError() : "Media player is unavailable");
        m_playState.store(PlayState::ERROR);
        return;
    }
    m_playState.store(PlayState::PLAYING);
}

void SlaveNodeApp::playbackMonitorLoop() {
    LOG_INFO("Playback monitor started");
    bool restoreIdleImage = false;
    while (m_running.load()) {
        const PlayState state = m_playState.load();
        if (state != PlayState::PLAYING && state != PlayState::PAUSED) {
            break;
        }
        if (!m_player || !m_player->isRunning()) {
            if (!m_player) {
                setError("Media player is unavailable");
                m_playState.store(PlayState::ERROR);
            } else {
                const std::string playerError = m_player->getLastError();
                if (!playerError.empty()) {
                    setError(playerError);
                    m_playState.store(PlayState::ERROR);
                    restoreIdleImage = true;
                } else {
                    LOG_INFO("Media player reached end of stream");
                    m_playState.store(PlayState::IDLE);
                    restoreIdleImage = true;
                }
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (restoreIdleImage && m_running.load()) {
        std::lock_guard<std::mutex> displayLock(m_displayModeMutex);
        m_player->stop();
        if (!showIdleImage()) {
            m_playState.store(PlayState::ERROR);
        }
    }
    LOG_INFO("Playback monitor stopped");
}

void SlaveNodeApp::schedulerLoop() {
    LOG_INFO("Playback scheduler started");
    auto nextCaptureCheck = std::chrono::steady_clock::now();
    auto nextOledCheck = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    bool captureUnavailable = false;

    while (m_running.load()) {
        const PlayState state = m_playState.load();
        if (state == PlayState::READY && m_commitReceived.load() &&
            m_pendingCommitCommandId.load() == m_currentCommandId.load()) {
            uint64_t timestamp = 0;
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                timestamp = m_syncTimestamp;
            }
            const uint64_t now = m_masterClock.getCurrentTimestamp();
            const uint64_t startWindow = timestamp > 50 ? timestamp - 50 : 0;
            if (timestamp != 0 && now >= startWindow && !startPlayback(timestamp) &&
                m_playState.load() == PlayState::READY) {
                setError("Failed to start synchronized playback");
                m_playState.store(PlayState::ERROR);
            }
        }

        const auto steadyNow = std::chrono::steady_clock::now();
        if (steadyNow >= nextOledCheck) {
            if (!m_oled.isOpen()) {
                m_oled.open();
            }
            if (m_oled.isOpen()) {
                const std::string sn = getSerialNumber();
                const std::string ip = getLocalIp();
                const auto rates = getNetworkRatesMbps();
                if (!m_oled.showSystemInfo(sn, ip, rates.first, rates.second, m_nodeId.load(),
                                           nodeRoleOledLabel(m_nodeRole.load()))) {
                    LOG_WARNING("OLED: periodic display update failed; reconnecting");
                    m_oled.close();
                }
            }
            nextOledCheck = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }

        if (state == PlayState::IDLE && steadyNow >= nextCaptureCheck) {
            std::lock_guard<std::mutex> displayLock(m_displayModeMutex);
            if (m_running.load() && m_playState.load() == PlayState::IDLE) {
                bool sourceConfigured = false;
                {
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    sourceConfigured = m_sourceConfig.type != SignalSourceType::NONE;
                }
                const bool signalRunning = m_player && m_player->isCaptureRunning();
                if (!sourceConfigured || signalRunning) {
                    captureUnavailable = false;
                } else {
                    if (showIdleImage() && m_player && m_player->isCaptureRunning()) {
                        {
                            std::lock_guard<std::mutex> lock(m_stateMutex);
                            m_lastError.clear();
                        }
                        LOG_INFO("Configured signal recovered");
                        captureUnavailable = false;
                    } else {
                        const std::string error = m_player
                            ? m_player->getLastError()
                            : "Media player is unavailable";
                        if (!captureUnavailable) {
                            LOG_WARNING("Configured signal unavailable; keeping idle image: %s",
                                        error.c_str());
                        }
                        captureUnavailable = true;

                        if (!m_player || !m_player->isImageRunning()) {
                            if (!showIdleFallbackImage()) {
                                m_playState.store(PlayState::ERROR);
                            }
                        }
                    }
                }
            }
            nextCaptureCheck = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    LOG_INFO("Playback scheduler stopped");
}

void SlaveNodeApp::heartbeatThread() {
    LOG_INFO("Heartbeat thread started for node %u", m_nodeId.load());
    while (m_running.load()) {
        broadcastHeartbeat();
        for (int interval = 0; interval < 20 && m_running.load(); ++interval) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    LOG_INFO("Heartbeat thread stopped for node %u", m_nodeId.load());
}

void SlaveNodeApp::discoveryThread() {
    LOG_INFO("Discovery listener started on UDP port %u", m_networkOptions.discoveryPort);
    char buffer[1024];
    while (m_running.load()) {
        sockaddr_in sender{};
        socklen_t senderLength = sizeof(sender);
        const ssize_t length = recvfrom(m_discoverySocket, buffer, sizeof(buffer), 0,
                                        reinterpret_cast<sockaddr*>(&sender), &senderLength);
        if (length < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && m_running.load()) {
                LOG_WARNING("Discovery receive failed: %s", std::strerror(errno));
            }
            continue;
        }
        uint64_t requestId = 0;
        if (length == 0 || !parseDiscoveryRequest(
                std::string(buffer, static_cast<size_t>(length)), requestId)) {
            continue;
        }
        const std::string heartbeat = createHeartbeatMessage();
        const ssize_t sent = sendto(m_discoverySocket, heartbeat.data(), heartbeat.size(), 0,
                                    reinterpret_cast<const sockaddr*>(&sender), senderLength);
        if (sent != static_cast<ssize_t>(heartbeat.size())) {
            LOG_WARNING("Discovery heartbeat reply failed: %s", std::strerror(errno));
        }
    }
    LOG_INFO("Discovery listener stopped for node %u", m_nodeId.load());
}

bool SlaveNodeApp::broadcastHeartbeat() {
    const int socketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd < 0) {
        LOG_ERROR("Failed to create heartbeat socket: %s", std::strerror(errno));
        return false;
    }

    unsigned char ttl = 1;
    if (setsockopt(socketFd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        LOG_ERROR("Failed to set heartbeat multicast TTL: %s", std::strerror(errno));
        close(socketFd);
        return false;
    }

    const std::string message = createHeartbeatMessage();

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(m_networkOptions.heartbeatPort);
    if (inet_pton(AF_INET, m_networkOptions.heartbeatMulticastAddress.c_str(), &address.sin_addr) != 1) {
        close(socketFd);
        return false;
    }
    const ssize_t sent = sendto(socketFd, message.data(), message.size(), 0,
                                reinterpret_cast<struct sockaddr*>(&address), sizeof(address));
    close(socketFd);
    if (sent != static_cast<ssize_t>(message.size())) {
        LOG_ERROR("Failed to send complete heartbeat: %s", std::strerror(errno));
        return false;
    }
    return true;
}

std::string SlaveNodeApp::createHeartbeatMessage() {
    const PlayState state = m_playState.load();
    uint64_t actualStartTimestamp = 0;
    std::string lastError;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        actualStartTimestamp = m_actualStartTimestamp;
        lastError = m_lastError;
    }
    const KvmAgentStatus kvmStatus = m_kvmAgent ? m_kvmAgent->getStatus() : KvmAgentStatus{};
    SignalSourceConfig source;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        source = m_sourceConfig;
    }
    // Hardware window playback also uses MediaPlayer::State::Capture. It is
    // not this node's configured signal source, so never advertise it as an
    // active input. A contradictory heartbeat (configured=false, active=true)
    // is correctly rejected by discovery and previously made output nodes
    // disappear as soon as a saved window layout was replayed after boot.
    const bool signalSourceActive = source.type != SignalSourceType::NONE &&
                                    m_player && m_player->isCaptureRunning();
    const std::string signalSourceTransport =
        source.type == SignalSourceType::CAPTURE && m_player && m_player->supportsH264Rtp()
            ? "rtp-h264" : "mjpeg";
    const json heartbeat = {
        {"type", "heartbeat"},
        {"nodeId", m_nodeId.load()},
        {"nodeRole", nodeRoleName(m_nodeRole.load())},
        {"ipMode", ipAssignmentModeName(m_networkOptions.ipMode)},
        {"ip", getLocalIp()},
        {"deviceModel", getDeviceModel()},
        {"deviceName", getDeviceName()},
        {"softwareVersion", getSoftwareVersion()},
        {"boardInfo", getBoardInfo()},
        {"macAddress", getMacAddress()},
        {"subnetMask", getSubnetMask()},
        {"gateway", getGateway()},
        {"deviceType", getDeviceType()},
        {"status", "online"},
        {"playState", playStateName(state)},
        {"resolution", getDisplayResolution()},
        {"cpu", std::round(getCpuUsagePercent() * 10.0) / 10.0},
        {"memory", std::round(getMemoryUsagePercent() * 10.0) / 10.0},
        {"audioOutputMode", audioOutputModeName(m_audioOutputMode.load())},
        {"masterClockSynchronized", m_masterClock.isSynchronized()},
        {"masterClockOffsetMs", m_masterClock.getTimeOffset()},
        {"commandId", m_currentCommandId.load()},
        {"actualStartTimestamp", actualStartTimestamp},
        {"lastError", lastError},
        {"kvmEnabled", kvmStatus.enabled},
        {"kvmRole", kvmRoleName(kvmStatus.role)},
        {"kvmPort", kvmStatus.port},
        {"kvmSessionId", kvmStatus.sessionId},
        {"kvmState", kvmStatus.state},
        {"kvmLastError", kvmStatus.lastError},
        {"signalSourceType", signalSourceTypeName(source.type)},
        {"signalSourceConfigured", source.type != SignalSourceType::NONE},
        {"signalSourceActive", signalSourceActive},
        {"signalSourceEndpoint", source.endpoint},
        {"signalSourceWidth", source.width},
        {"signalSourceHeight", source.height},
        {"signalSourceFramerateNumerator", source.framerateNumerator},
        {"signalSourceFramerateDenominator", source.framerateDenominator},
        {"signalSourcePixelFormat", source.pixelFormat},
        {"signalSourceTransport", signalSourceTransport},
        {"timestamp", Protocol::getCurrentTimestamp()}
    };
    return heartbeat.dump();
}

bool SlaveNodeApp::loadNodeId() {
    if (m_nodeIdFile.empty()) return true;
    std::ifstream input(m_nodeIdFile);
    if (!input) return true;
    uint64_t value = 0;
    input >> value;
    if (!input || value == 0 || value > MAX_NODES) return false;
    input >> std::ws;
    if (!input.eof()) return false;
    m_nodeId.store(static_cast<uint32_t>(value));
    return true;
}

bool SlaveNodeApp::persistNodeId(uint32_t nodeId) {
    if (m_nodeIdFile.empty() || nodeId == 0 || nodeId > MAX_NODES) return false;
    const std::filesystem::path target(m_nodeIdFile);
    const std::filesystem::path temporary = target.string() + ".tmp";
    std::error_code error;
    if (!target.parent_path().empty()) {
        std::filesystem::create_directories(target.parent_path(), error);
        if (error) return false;
    }
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return false;
        output << nodeId << '\n';
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary);
            return false;
        }
    }
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::filesystem::remove(temporary);
        return false;
    }
    return true;
}

bool SlaveNodeApp::loadNodeRole() {
    if (m_nodeRoleFile.empty()) return true;
    std::ifstream input(m_nodeRoleFile);
    if (!input) return true;
    std::string value;
    input >> value;
    NodeRole role;
    if (!parseNodeRole(value, role)) {
        LOG_WARNING("Ignoring invalid persisted node role in %s", m_nodeRoleFile.c_str());
        return false;
    }
    m_nodeRole.store(role);
    return true;
}

bool SlaveNodeApp::setNodeRole(NodeRole role, bool persist) {
    if (persist && !m_nodeRoleFile.empty()) {
        const std::filesystem::path rolePath(m_nodeRoleFile);
        const std::filesystem::path temporaryPath = rolePath.string() + ".tmp";
        std::error_code error;
        if (!rolePath.parent_path().empty()) {
            std::filesystem::create_directories(rolePath.parent_path(), error);
            if (error) return false;
        }
        {
            std::ofstream output(temporaryPath, std::ios::trunc);
            if (!output || !(output << nodeRoleName(role) << '\n')) return false;
        }
        std::filesystem::rename(temporaryPath, rolePath, error);
        if (error) {
            std::filesystem::remove(temporaryPath);
            return false;
        }
    }
    m_nodeRole.store(role);
    LOG_INFO("Node role updated: nodeId=%u role=%s", m_nodeId.load(), nodeRoleName(role));
    return true;
}

bool SlaveNodeApp::loadNetworkConfig() {
    if (m_networkConfigFile.empty()) return true;
    std::ifstream input(m_networkConfigFile);
    if (!input) return true;

    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line)) {
        const size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0) return false;
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if (key != "IP_MODE" && key != "IP_ADDRESS" && key != "PREFIX_LENGTH" &&
            key != "GATEWAY" && key != "DNS_SERVERS") return false;
        if (!values.emplace(key, value).second) return false;
    }
    if (values.size() != 5 || !values.count("IP_MODE") || !values.count("IP_ADDRESS") ||
        !values.count("PREFIX_LENGTH") || !values.count("GATEWAY") ||
        !values.count("DNS_SERVERS")) return false;

    Protocol::SetNodeNetworkCommand command;
    command.nodeId = m_nodeId.load();
    command.mode = values.at("IP_MODE");
    command.address = values.at("IP_ADDRESS");
    command.gateway = values.at("GATEWAY");
    try {
        size_t parsed = 0;
        const unsigned long prefix = std::stoul(values.at("PREFIX_LENGTH"), &parsed);
        if (parsed != values.at("PREFIX_LENGTH").size() || prefix > UINT32_MAX) return false;
        command.prefixLength = static_cast<uint32_t>(prefix);
    } catch (const std::exception&) {
        return false;
    }
    std::stringstream dns(values.at("DNS_SERVERS"));
    std::string value;
    while (std::getline(dns, value, ',')) {
        if (!value.empty()) command.dnsServers.push_back(value);
    }
    if (!Protocol::validateSetNodeNetworkCommand(command)) return false;
    m_networkOptions.ipMode = command.mode == "manual"
        ? IpAssignmentMode::MANUAL : IpAssignmentMode::AUTO;
    m_networkOptions.localIp = command.address;
    m_appliedNetworkConfig = command;
    m_hasAppliedNetworkConfig = true;
    return true;
}

bool SlaveNodeApp::persistNetworkConfig(const Protocol::SetNodeNetworkCommand& command) {
    if (m_networkConfigFile.empty() || !Protocol::validateSetNodeNetworkCommand(command)) {
        return false;
    }
    const std::filesystem::path configPath(m_networkConfigFile);
    const std::filesystem::path temporaryPath = configPath.string() + ".tmp";
    std::error_code error;
    if (!configPath.parent_path().empty()) {
        std::filesystem::create_directories(configPath.parent_path(), error);
        if (error) return false;
    }
    std::ofstream output(temporaryPath, std::ios::trunc);
    if (!output) return false;
    output << "IP_MODE=" << command.mode << '\n'
           << "IP_ADDRESS=" << command.address << '\n'
           << "PREFIX_LENGTH=" << command.prefixLength << '\n'
           << "GATEWAY=" << command.gateway << '\n'
           << "DNS_SERVERS=";
    for (size_t i = 0; i < command.dnsServers.size(); ++i) {
        if (i != 0) output << ',';
        output << command.dnsServers[i];
    }
    output << '\n';
    output.close();
    if (!output) {
        std::filesystem::remove(temporaryPath);
        return false;
    }
    std::filesystem::rename(temporaryPath, configPath, error);
    if (error) {
        std::filesystem::remove(temporaryPath);
        return false;
    }
    return true;
}

bool SlaveNodeApp::loadSourceConfig() {
    if (m_sourceConfigFile.empty()) {
        m_sourceConfig = {};
        return true;
    }
    std::ifstream input(m_sourceConfigFile);
    if (!input) {
        m_sourceConfig = {};
        return true;
    }
    try {
        const json value = json::parse(input);
        if (!value.is_object() || value.size() != 7 ||
            !value.contains("source_type") || !value.at("source_type").is_string() ||
            !value.contains("endpoint") || !value.at("endpoint").is_string() ||
            !value.contains("width") || !value.contains("height") ||
            !value.contains("framerate_numerator") ||
            !value.contains("framerate_denominator") ||
            !value.contains("pixel_format") || !value.at("pixel_format").is_string()) {
            return false;
        }
        SignalSourceConfig source;
        if (!parseSignalSourceType(value.at("source_type").get<std::string>(), source.type) ||
            !value.at("width").is_number_unsigned() ||
            !value.at("height").is_number_unsigned() ||
            !value.at("framerate_numerator").is_number_unsigned() ||
            !value.at("framerate_denominator").is_number_unsigned()) {
            return false;
        }
        source.endpoint = value.at("endpoint").get<std::string>();
        source.width = value.at("width").get<uint32_t>();
        source.height = value.at("height").get<uint32_t>();
        source.framerateNumerator = value.at("framerate_numerator").get<uint32_t>();
        source.framerateDenominator = value.at("framerate_denominator").get<uint32_t>();
        source.pixelFormat = value.at("pixel_format").get<std::string>();
        if (!Protocol::validateSignalSourceConfig(source)) return false;
        m_sourceConfig = std::move(source);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool SlaveNodeApp::persistSourceConfig(const SignalSourceConfig& source) {
    if (m_sourceConfigFile.empty() || !Protocol::validateSignalSourceConfig(source)) {
        return false;
    }
    const std::filesystem::path target(m_sourceConfigFile);
    const std::filesystem::path temporary = target.string() + ".tmp";
    std::error_code error;
    if (!target.parent_path().empty()) {
        std::filesystem::create_directories(target.parent_path(), error);
        if (error) return false;
    }
    const json value = {
        {"source_type", signalSourceTypeName(source.type)},
        {"endpoint", source.endpoint},
        {"width", source.width},
        {"height", source.height},
        {"framerate_numerator", source.framerateNumerator},
        {"framerate_denominator", source.framerateDenominator},
        {"pixel_format", source.pixelFormat}
    };
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return false;
        output << value.dump(2) << '\n';
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary);
            return false;
        }
    }
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::filesystem::remove(temporary);
        return false;
    }
    return true;
}

bool SlaveNodeApp::setNodeNetwork(const Protocol::SetNodeNetworkCommand& command) {
    if (command.nodeId != m_nodeId.load() || !Protocol::validateSetNodeNetworkCommand(command)) {
        return false;
    }
    const bool unchanged = m_hasAppliedNetworkConfig &&
        command.nodeId == m_appliedNetworkConfig.nodeId &&
        command.mode == m_appliedNetworkConfig.mode &&
        command.address == m_appliedNetworkConfig.address &&
        command.prefixLength == m_appliedNetworkConfig.prefixLength &&
        command.gateway == m_appliedNetworkConfig.gateway &&
        command.dnsServers == m_appliedNetworkConfig.dnsServers;
    if (unchanged) {
        LOG_INFO("Node network unchanged: nodeId=%u; ignoring duplicate command", m_nodeId.load());
        return true;
    }
    const bool modeChanged = command.mode != ipAssignmentModeName(m_networkOptions.ipMode);
    if (!persistNetworkConfig(command)) return false;

    m_networkOptions.ipMode = command.mode == "manual"
        ? IpAssignmentMode::MANUAL : IpAssignmentMode::AUTO;
    m_networkOptions.localIp = command.address;
    m_appliedNetworkConfig = command;
    m_hasAppliedNetworkConfig = true;
    LOG_INFO("Node network updated: nodeId=%u mode=%s address=%s/%u",
             m_nodeId.load(), command.mode.c_str(), command.address.c_str(), command.prefixLength);
    const char* applyCommand = modeChanged
        ? "(sleep 1; /etc/init.d/S41dhcpcd reload; /etc/init.d/S99distributed-matrix restart) >/dev/null 2>&1 &"
        : "(sleep 1; /etc/init.d/S41dhcpcd reload) >/dev/null 2>&1 &";
    const int result = std::system(applyCommand);
    return result == 0;
}

std::string SlaveNodeApp::getHostname() const {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') {
        return buf;
    }
    return "dms-node";
}

std::string SlaveNodeApp::getSerialNumber() const {
    // Generate SN from current date + node_id: YYYYMMDD + 4-digit nodeId
    // e.g. "202608151001"
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d%04u",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, m_nodeId.load());
    return buf;
}

std::pair<std::string, std::string> SlaveNodeApp::getNetworkRatesMbps() {
    std::ifstream txFile("/sys/class/net/eth0/statistics/tx_bytes");
    std::ifstream rxFile("/sys/class/net/eth0/statistics/rx_bytes");
    uint64_t txBytes = 0;
    uint64_t rxBytes = 0;
    if (!(txFile >> txBytes) || !(rxFile >> rxBytes)) {
        return {"0", "0"};
    }

    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    std::lock_guard<std::mutex> lock(m_networkRateMutex);
    if (m_previousNetworkSampleMs == 0 || nowMs <= m_previousNetworkSampleMs ||
        txBytes < m_previousTxBytes || rxBytes < m_previousRxBytes) {
        m_previousTxBytes = txBytes;
        m_previousRxBytes = rxBytes;
        m_previousNetworkSampleMs = nowMs;
        return {"0", "0"};
    }

    const double elapsedSeconds = static_cast<double>(nowMs - m_previousNetworkSampleMs) / 1000.0;
    const double txRate = static_cast<double>(txBytes - m_previousTxBytes) * 8.0 /
                          (elapsedSeconds * 1000000.0);
    const double rxRate = static_cast<double>(rxBytes - m_previousRxBytes) * 8.0 /
                          (elapsedSeconds * 1000000.0);
    m_previousTxBytes = txBytes;
    m_previousRxBytes = rxBytes;
    m_previousNetworkSampleMs = nowMs;

    const auto formatRate = [](double rate) {
        const long rounded = std::lround(std::clamp(rate, 0.0, 999.0));
        return std::to_string(rounded);
    };
    return {formatRate(txRate), formatRate(rxRate)};
}

std::string SlaveNodeApp::getDeviceModel() const {
    return availableOrUnknown(readSystemText("/proc/device-tree/model"));
}

std::string SlaveNodeApp::getDeviceName() const {
    return getHostname();
}

std::string SlaveNodeApp::getSoftwareVersion() const {
#ifdef DMS_VERSION
    return DMS_VERSION;
#else
    return "unknown";
#endif
}

std::string SlaveNodeApp::getBoardInfo() const {
    std::string board = readSystemText("/proc/device-tree/compatible");
    if (board.empty()) board = readSystemText("/proc/device-tree/model");
    return availableOrUnknown(board);
}

std::string SlaveNodeApp::getMacAddress() const {
    const NetworkInterfaceInfo interface = getPreferredNetworkInterface(
        m_networkOptions.ipMode == IpAssignmentMode::MANUAL ? m_networkOptions.localIp : "");
    const std::string interfaceName = interface.name.empty() ? "eth0" : interface.name;
    std::string address = readSystemText(
        std::filesystem::path("/sys/class/net") / interfaceName / "address");
    std::transform(address.begin(), address.end(), address.begin(), [](unsigned char value) {
        return static_cast<char>(std::toupper(value));
    });
    return availableOrUnknown(address);
}

std::string SlaveNodeApp::getSubnetMask() const {
    const NetworkInterfaceInfo interface = getPreferredNetworkInterface(
        m_networkOptions.ipMode == IpAssignmentMode::MANUAL ? m_networkOptions.localIp : "");
    return availableOrUnknown(interface.subnetMask);
}

std::string SlaveNodeApp::getGateway() const {
    return availableOrUnknown(readDefaultGateway());
}

std::string SlaveNodeApp::getDeviceType() const {
    switch (m_nodeRole.load()) {
        case NodeRole::UNASSIGNED: return "unassigned";
        case NodeRole::ENCODE: return "input";
        case NodeRole::DECODE: return "output";
        case NodeRole::CODEC: return "input/output";
    }
    return "unknown";
}

std::string SlaveNodeApp::getLocalIp() const {
    if (m_networkOptions.ipMode == IpAssignmentMode::MANUAL) {
        return m_networkOptions.localIp;
    }
    struct ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return "unknown";
    }

    // 优先选择 eth*/en* 网卡，其次选任意非回环地址
    std::string localIp;
    std::string fallbackIp;
    for (struct ifaddrs* interface = interfaces; interface != nullptr; interface = interface->ifa_next) {
        if (!interface->ifa_addr || interface->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto* address = reinterpret_cast<const struct sockaddr_in*>(interface->ifa_addr);
        char buffer[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer))) {
            continue;
        }
        if (std::strncmp(buffer, "127.", 4) == 0) {
            continue;
        }
        const char* name = interface->ifa_name ? interface->ifa_name : "";
        if (std::strncmp(name, "eth", 3) == 0 || std::strncmp(name, "en", 2) == 0) {
            localIp = buffer;
            break;
        }
        if (fallbackIp.empty()) {
            fallbackIp = buffer;
        }
    }
    freeifaddrs(interfaces);
    return localIp.empty() ? (fallbackIp.empty() ? "unknown" : fallbackIp) : localIp;
}

std::string SlaveNodeApp::getDisplayResolution() const {
    const std::string activeDrmResolution = readActiveDrmResolution();
    if (!activeDrmResolution.empty()) {
        return activeDrmResolution;
    }

    if (const char* configured = std::getenv("DMS_DISPLAY_RESOLUTION")) {
        if (*configured != '\0') {
            return configured;
        }
    }

    std::ifstream framebuffer("/sys/class/graphics/fb0/virtual_size");
    std::string size;
    if (std::getline(framebuffer, size) && !size.empty()) {
        std::replace(size.begin(), size.end(), ',', 'x');
        return size;
    }
    return "unknown";
}

double SlaveNodeApp::getCpuUsagePercent() {
    std::ifstream statFile("/proc/stat");
    std::string cpuLabel;
    uint64_t user = 0, nice = 0, system = 0, idle = 0, ioWait = 0;
    uint64_t irq = 0, softIrq = 0, steal = 0;
    if (!(statFile >> cpuLabel >> user >> nice >> system >> idle >> ioWait >> irq >> softIrq >> steal) ||
        cpuLabel != "cpu") {
        return 0.0;
    }

    const uint64_t idleTotal = idle + ioWait;
    const uint64_t total = user + nice + system + idle + ioWait + irq + softIrq + steal;
    std::lock_guard<std::mutex> lock(m_cpuMutex);
    if (m_previousCpuTotal == 0 || total <= m_previousCpuTotal) {
        m_previousCpuTotal = total;
        m_previousCpuIdle = idleTotal;
        return 0.0;
    }

    const uint64_t totalDelta = total - m_previousCpuTotal;
    const uint64_t idleDelta = idleTotal - m_previousCpuIdle;
    m_previousCpuTotal = total;
    m_previousCpuIdle = idleTotal;
    return totalDelta == 0 ? 0.0 : 100.0 * static_cast<double>(totalDelta - idleDelta) /
                                      static_cast<double>(totalDelta);
}

double SlaveNodeApp::getMemoryUsagePercent() const {
    std::ifstream memInfo("/proc/meminfo");
    std::string key;
    uint64_t value = 0;
    std::string unit;
    uint64_t total = 0;
    uint64_t available = 0;
    while (memInfo >> key >> value >> unit) {
        if (key == "MemTotal:") {
            total = value;
        } else if (key == "MemAvailable:") {
            available = value;
        }
        if (total > 0 && available > 0) {
            break;
        }
    }
    if (total == 0 || available > total) {
        return 0.0;
    }
    return 100.0 * static_cast<double>(total - available) / static_cast<double>(total);
}

const char* SlaveNodeApp::playStateName(PlayState state) {
    switch (state) {
        case PlayState::IDLE: return "idle";
        case PlayState::PREPARING: return "preparing";
        case PlayState::READY: return "ready";
        case PlayState::READY_PAUSED: return "paused";
        case PlayState::PLAYING: return "playing";
        case PlayState::PAUSED: return "paused";
        case PlayState::ERROR: return "error";
    }
    return "unknown";
}

void SlaveNodeApp::setError(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_lastError = error;
    }
    LOG_ERROR("SlaveNodeApp: %s", error.c_str());
}

std::string SlaveNodeApp::getLastError() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_lastError;
}

} // namespace dms
