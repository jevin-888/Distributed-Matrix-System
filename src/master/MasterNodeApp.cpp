#include "master/MasterNodeApp.h"
#include "common/Logger.h"
#include "master/LayoutCalculator.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <cstdlib>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace dms {
namespace {


int64_t calculateStartSkew(uint64_t actualTimestamp, uint64_t expectedTimestamp) {
    if (actualTimestamp >= expectedTimestamp) {
        const uint64_t delta = actualTimestamp - expectedTimestamp;
        return delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
            ? std::numeric_limits<int64_t>::max()
            : static_cast<int64_t>(delta);
    }
    const uint64_t delta = expectedTimestamp - actualTimestamp;
    return delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
        ? -std::numeric_limits<int64_t>::max()
        : -static_cast<int64_t>(delta);
}

std::string detectLocalIpv4() {
    struct ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return "127.0.0.1";
    }

    std::string result = "127.0.0.1";
    for (struct ifaddrs* interface = interfaces; interface != nullptr; interface = interface->ifa_next) {
        if (!interface->ifa_addr || interface->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto* address = reinterpret_cast<const struct sockaddr_in*>(interface->ifa_addr);
        char buffer[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer))) {
            continue;
        }
        if (std::strncmp(buffer, "127.", 4) != 0) {
            result = buffer;
            break;
        }
    }
    freeifaddrs(interfaces);
    return result;
}

} // namespace

MasterNodeApp::MasterNodeApp()
    : m_localHttpPort(8080)
    , m_screenLayout{2, 2, 1920, 1080}
    , m_audioOutputMode(AudioOutputMode::BOTH)
    , m_audioVolumePercent(100)
    , m_running(false) {
}

MasterNodeApp::~MasterNodeApp() {
    shutdown();
}

bool MasterNodeApp::initialize(const MasterNodeOptions& options) {
    if (m_running.load() || options.httpPort == 0 || options.cacheDir.empty() ||
        options.maxCacheSizeBytes == 0 || options.defaultSyncDelayMs > 24ULL * 60ULL * 60ULL * 1000ULL ||
        options.network.commandMulticastAddress.empty() || options.network.commandPort == 0 ||
        options.network.heartbeatMulticastAddress.empty() || options.network.heartbeatPort == 0 ||
        options.network.discoveryPort == 0) {
        return false;
    }

    struct in_addr multicastAddress {};
    if (inet_pton(AF_INET, options.network.commandMulticastAddress.c_str(), &multicastAddress) != 1 ||
        !IN_MULTICAST(ntohl(multicastAddress.s_addr)) ||
        inet_pton(AF_INET, options.network.heartbeatMulticastAddress.c_str(), &multicastAddress) != 1 ||
        !IN_MULTICAST(ntohl(multicastAddress.s_addr))) {
        LOG_ERROR("Master multicast address is invalid");
        return false;
    }

    m_localHttpPort = options.httpPort;
    m_cacheDir = options.cacheDir;
    if (options.network.ipMode == IpAssignmentMode::MANUAL &&
        !setLocalIp(options.network.localIp)) {
        return false;
    }
    if (!setScreenLayout(options.layout)) {
        return false;
    }

    m_broadcaster = std::make_unique<CommandBroadcaster>();
    m_kvmSessionManager = std::make_unique<KvmSessionManager>();
    if (!m_broadcaster->initialize(options.network.commandMulticastAddress.c_str(),
                                   options.network.commandPort)) {
        LOG_ERROR("Failed to initialize CommandBroadcaster");
        shutdown();
        return false;
    }

    m_cacheManager = std::make_unique<VideoCacheManager>();
    if (!m_cacheManager->initialize(options.cacheDir.c_str(), options.maxCacheSizeBytes)) {
        LOG_ERROR("Failed to initialize VideoCacheManager");
        shutdown();
        return false;
    }

    m_nodeDiscovery = std::make_unique<NodeDiscovery>();
    if (!m_nodeDiscovery->initialize(options.network.heartbeatMulticastAddress,
                                     options.network.heartbeatPort,
                                     options.network.discoveryPort,
                                     options.network.discoveryNetworks)) {
        LOG_ERROR("Failed to initialize NodeDiscovery");
        shutdown();
        return false;
    }
    if (!m_nodeDiscovery->start()) {
        LOG_ERROR("Failed to start NodeDiscovery");
        shutdown();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_localIp.empty()) {
            m_localIp = detectLocalIpv4();
        }
        m_localHttpUrl = "http://" + m_localIp + ":" + std::to_string(m_localHttpPort);
    }

    const std::string regionsFile = options.regionsFile.empty()
        ? (std::filesystem::path(options.cacheDir) / "regions.json").string()
        : options.regionsFile;
    m_httpServer = std::make_unique<dms::HTTPFileServer>(
        options.cacheDir, options.httpPort, 32, options.webRoot, regionsFile,
        options.defaultSyncDelayMs);
    m_httpServer->setNodeDiscovery(m_nodeDiscovery.get());
    m_httpServer->setMasterApp(this);
    if (!m_httpServer->start()) {
        LOG_ERROR("Failed to start HTTP server");
        shutdown();
        return false;
    }

    m_running.store(true);
    m_kvmLeaseThread = std::thread(&MasterNodeApp::kvmLeaseLoop, this);
    m_layoutSyncThread = std::thread(&MasterNodeApp::layoutSyncLoop, this);
    updatePlaybackStatus("idle");
    LOG_INFO("MasterNodeApp initialized: http=%s, cache=%s",
             getLocalHttpUrl().c_str(), options.cacheDir.c_str());
    return true;
}

void MasterNodeApp::shutdown() {
    m_running.store(false);
    if (m_kvmLeaseThread.joinable()) m_kvmLeaseThread.join();
    if (m_layoutSyncThread.joinable()) m_layoutSyncThread.join();
    if (m_httpServer) {
        m_httpServer->stop();
    }
    if (m_nodeDiscovery) {
        m_nodeDiscovery->shutdown();
    }
    if (m_broadcaster) {
        m_broadcaster->shutdown();
    }
    if (m_cacheManager) {
        m_cacheManager->shutdown();
    }

    m_httpServer.reset();
    m_nodeDiscovery.reset();
    m_broadcaster.reset();
    m_kvmSessionManager.reset();
    m_cacheManager.reset();
    clearExpectedNodeIds();
    updatePlaybackStatus("stopped");
}

bool MasterNodeApp::syncPlay(const std::string& videoUrl,
                             const VideoInfo& videoInfo,
                             const std::vector<Protocol::CropRegion>& layoutConfig,
                             uint64_t delayMs) {
    if (!m_running.load() || !m_broadcaster || videoUrl.empty() ||
        videoInfo.width == 0 || videoInfo.height == 0 ||
        videoInfo.width > 65535 || videoInfo.height > 65535 ||
        layoutConfig.empty() || layoutConfig.size() > MAX_NODES || delayMs > 24ULL * 60ULL * 60ULL * 1000ULL) {
        return false;
    }

    std::set<uint32_t> nodeIds;
    for (const auto& crop : layoutConfig) {
        if (crop.nodeId == 0 || crop.cropWidth == 0 || crop.cropHeight == 0 ||
            crop.cropX > std::numeric_limits<uint32_t>::max() - crop.cropWidth ||
            crop.cropY > std::numeric_limits<uint32_t>::max() - crop.cropHeight ||
            crop.cropX + crop.cropWidth > videoInfo.width ||
            crop.cropY + crop.cropHeight > videoInfo.height ||
            !nodeIds.insert(crop.nodeId).second) {
            LOG_ERROR("Invalid or duplicate crop region for node %u", crop.nodeId);
            return false;
        }
    }

    const std::string accessibleUrl = convertToAccessibleUrl(videoUrl);
    if (accessibleUrl.empty()) {
        return false;
    }
    const uint64_t now = Protocol::getCurrentTimestamp();
    if (delayMs > std::numeric_limits<uint64_t>::max() - now) {
        return false;
    }
    const uint64_t syncTimestamp = now + delayMs;
    uint32_t commandId = 0;
    if (!m_broadcaster->broadcastPreparePlay(
            accessibleUrl, videoInfo.width, videoInfo.height, syncTimestamp, layoutConfig, commandId)) {
        updatePlaybackStatus("error", accessibleUrl, commandId, syncTimestamp);
        return false;
    }
    setExpectedNodeIds(layoutConfig);
    updatePlaybackStatus("preparing", accessibleUrl, commandId, syncTimestamp);
    updatePlaybackStatus("committing", accessibleUrl, commandId, syncTimestamp);
    if (!m_broadcaster->broadcastCommitPlay(commandId)) {
        updatePlaybackStatus("error", accessibleUrl, commandId, syncTimestamp);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_currentVideoInfo = videoInfo;
        m_currentPlaybackDelayMs = delayMs;
    }

    updatePlaybackStatus("scheduled", accessibleUrl, commandId, syncTimestamp);
    LOG_INFO("Synchronized playback committed: command=%u, url=%s, timestamp=%lu, nodes=%zu",
             commandId, accessibleUrl.c_str(), syncTimestamp, layoutConfig.size());
    return true;
}

bool MasterNodeApp::play(const std::string& videoUrl,
                         const VideoInfo& videoInfo,
                         uint64_t delayMs) {
    std::lock_guard<std::mutex> commandLock(m_commandMutex);
    try {
        dms::LayoutCalculator calculator;
        calculator.setLayout(getScreenLayout());
        calculator.setVideoInfo(videoInfo);
        calculator.setPlacements(getScreenPlacements());
        return syncPlay(videoUrl, videoInfo, calculator.calculateCropRegions(), delayMs);
    } catch (const std::exception& error) {
        LOG_ERROR("Cannot calculate playback layout: %s", error.what());
        return false;
    }
}

bool MasterNodeApp::stop() {
    std::lock_guard<std::mutex> commandLock(m_commandMutex);
    const MasterPlaybackStatus status = getPlaybackStatus();
    if (!m_running.load() || status.state == "idle" || status.state == "stopped" ||
        !m_broadcaster || !m_broadcaster->broadcastStop()) {
        return false;
    }
    clearExpectedNodeIds();
    updatePlaybackStatus("stopped");
    return true;
}

bool MasterNodeApp::pause() {
    std::lock_guard<std::mutex> commandLock(m_commandMutex);
    const MasterPlaybackStatus status = getPlaybackStatus();
    if (!m_running.load() || (status.state != "scheduled" && status.state != "playing") ||
        !m_broadcaster || !m_broadcaster->broadcastPause()) {
        return false;
    }
    updatePlaybackStatus("paused", status.videoUrl, status.commandId, status.syncTimestamp);
    return true;
}

bool MasterNodeApp::resume() {
    std::lock_guard<std::mutex> commandLock(m_commandMutex);
    const MasterPlaybackStatus status = getPlaybackStatus();
    if (!m_running.load() || status.state != "paused" ||
        !m_broadcaster || !m_broadcaster->broadcastResume()) {
        return false;
    }
    const std::string nextState = Protocol::getCurrentTimestamp() < status.syncTimestamp
        ? "scheduled" : "playing";
    updatePlaybackStatus(nextState, status.videoUrl, status.commandId, status.syncTimestamp);
    return true;
}

bool MasterNodeApp::setAudioOutputMode(AudioOutputMode mode) {
    std::lock_guard<std::mutex> commandLock(m_commandMutex);
    if (!m_running.load() || !m_broadcaster ||
        !m_broadcaster->broadcastSetAudioOutput(mode)) {
        return false;
    }
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    m_audioOutputMode = mode;
    return true;
}

AudioOutputMode MasterNodeApp::getAudioOutputMode() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_audioOutputMode;
}

bool MasterNodeApp::setAudioVolumePercent(uint32_t volumePercent) {
    std::lock_guard<std::mutex> commandLock(m_commandMutex);
    if (volumePercent > 100 || !m_running.load() || !m_broadcaster ||
        !m_broadcaster->broadcastSetAudioVolume(volumePercent)) {
        return false;
    }
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    m_audioVolumePercent = volumePercent;
    return true;
}

uint32_t MasterNodeApp::getAudioVolumePercent() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_audioVolumePercent;
}

KvmSession MasterNodeApp::getKvmSession() const {
    return m_kvmSessionManager ? m_kvmSessionManager->getSession() : KvmSession{};
}

bool MasterNodeApp::acquireKvmSession(uint32_t controllerNodeId, uint32_t targetNodeId,
                                      uint64_t leaseMs, KvmSession& session) {
    if (!m_running.load() || !m_nodeDiscovery || !m_broadcaster || !m_kvmSessionManager) return false;
    DiscoveredNode target;
    bool foundTarget = false;
    for (const auto& node : m_nodeDiscovery->getNodes()) {
        if (node.nodeId == targetNodeId) {
            target = node;
            foundTarget = true;
        }
    }
    if (!foundTarget || !target.kvmEnabled || !kvmCanTarget(target.kvmRole) ||
        target.ip.empty() || target.kvmPort == 0) return false;
    std::lock_guard<std::mutex> lock(m_commandMutex);
    if (!m_kvmSessionManager->acquire(controllerNodeId, targetNodeId, target.ip,
                                      target.kvmPort, leaseMs, session)) return false;
    if (m_broadcaster->broadcastKvmRoute(session)) return true;
    KvmSession released;
    m_kvmSessionManager->release(session.sessionId, released);
    session = {};
    return false;
}

bool MasterNodeApp::releaseKvmSession(uint32_t sessionId) {
    if (!m_running.load() || !m_broadcaster || !m_kvmSessionManager || sessionId == 0) return false;
    std::lock_guard<std::mutex> lock(m_commandMutex);
    KvmSession released;
    if (!m_kvmSessionManager->release(sessionId, released)) return false;
    return m_broadcaster->broadcastKvmRelease(sessionId);
}

bool MasterNodeApp::setNodeRole(uint32_t nodeId, NodeRole role) {
    if (!m_running.load() || !m_broadcaster || !m_nodeDiscovery || nodeId == 0) return false;
    const auto nodes = m_nodeDiscovery->getNodes();
    const auto target = std::find_if(nodes.begin(), nodes.end(), [&](const DiscoveredNode& node) {
        return node.nodeId == nodeId && node.status == "online";
    });
    if (target == nodes.end() || target->advertisedNodeId == 0 || target->ip.empty()) return false;
    std::lock_guard<std::mutex> commandLock(m_commandMutex);
    return m_broadcaster->sendSetNodeRole(target->ip, target->advertisedNodeId, role);
}

bool MasterNodeApp::setNodeNetwork(const Protocol::SetNodeNetworkCommand& command) {
    if (!m_running.load() || !m_broadcaster || !m_nodeDiscovery ||
        !Protocol::validateSetNodeNetworkCommand(command)) {
        return false;
    }
    const auto nodes = m_nodeDiscovery->getNodes();
    const auto target = std::find_if(nodes.begin(), nodes.end(), [&](const DiscoveredNode& node) {
        return node.nodeId == command.nodeId && node.status == "online";
    });
    if (target == nodes.end() || target->advertisedNodeId == 0 || target->ip.empty()) return false;
    Protocol::SetNodeNetworkCommand targetCommand = command;
    targetCommand.nodeId = target->advertisedNodeId;
    std::lock_guard<std::mutex> commandLock(m_commandMutex);
    return m_broadcaster->sendSetNodeNetwork(target->ip, targetCommand);
}

bool MasterNodeApp::setNodeSource(const Protocol::SetNodeSourceCommand& command) {
    if (!m_running.load() || !m_broadcaster || !m_nodeDiscovery || command.nodeId == 0 ||
        !Protocol::validateSignalSourceConfig(command.source)) {
        return false;
    }
    const auto nodes = m_nodeDiscovery->getNodes();
    const auto target = std::find_if(nodes.begin(), nodes.end(), [&](const DiscoveredNode& node) {
        return node.nodeId == command.nodeId && node.status == "online";
    });
    if (target == nodes.end() || target->advertisedNodeId == 0 || target->ip.empty()) return false;
    Protocol::SetNodeSourceCommand targetCommand = command;
    targetCommand.nodeId = target->advertisedNodeId;
    std::lock_guard<std::mutex> commandLock(m_commandMutex);
    if (!m_broadcaster->sendSetNodeSource(target->ip, targetCommand)) return false;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        if (command.source.type == SignalSourceType::NONE) {
            m_nodeSourceOverrides.erase(command.nodeId);
        } else {
            m_nodeSourceOverrides[command.nodeId] = command.source;
        }
    }
    // A source resolution change invalidates the dimensions in the current
    // window command. Re-send the saved canvas immediately so the slave can
    // rebuild its input caps against the new source.
    if (!getWindowPlacements().empty() && !applyWindowLayoutLocked()) {
        LOG_WARNING("Node source updated but current window layout could not be refreshed: node=%u",
                    command.nodeId);
    }
    return true;
}

void MasterNodeApp::kvmLeaseLoop() {
    while (m_running.load()) {
        KvmSession expired;
        if (m_kvmSessionManager && m_kvmSessionManager->expire(
                Protocol::getCurrentTimestamp(), expired) && m_broadcaster) {
            m_broadcaster->broadcastKvmRelease(expired.sessionId);
        }
        for (int i = 0; i < 10 && m_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void MasterNodeApp::layoutSyncLoop() {
    // Re-send the saved layout when a node restarts so it can leave its idle
    // background without requiring a second client edit.
    while (m_running.load()) {
        std::set<uint32_t> currentNodeIds;
        bool nodeAssignmentsPending = false;
        if (m_nodeDiscovery) {
            for (const auto& node : m_nodeDiscovery->getNodes()) {
                if (node.status != "online") continue;
                currentNodeIds.insert(node.nodeId);
                const std::string identity = node.macAddress.empty() ? node.ip : node.macAddress;
                const uint64_t now = Protocol::getCurrentTimestamp();
                const auto attempt = m_nodeAssignmentAttempts.find(identity);
                const bool needsReassignment = node.advertisedNodeId != node.nodeId;
                nodeAssignmentsPending = nodeAssignmentsPending || needsReassignment;
                const bool shouldSend = attempt == m_nodeAssignmentAttempts.end() ||
                    (needsReassignment && now - attempt->second >= 2000);
                if (!shouldSend || identity.empty() || node.ip.empty() ||
                    node.macAddress.empty()) {
                    continue;
                }
                Protocol::SetNodeIdCommand assignment;
                assignment.currentNodeId = node.advertisedNodeId;
                assignment.newNodeId = node.nodeId;
                assignment.targetMacAddress = node.macAddress;
                std::lock_guard<std::mutex> commandLock(m_commandMutex);
                if (m_broadcaster && m_broadcaster->sendSetNodeId(node.ip, assignment)) {
                    m_nodeAssignmentAttempts[identity] = now;
                    LOG_INFO("Assigned persistent node ID %u to %s (%s)",
                             node.nodeId, node.ip.c_str(), node.macAddress.c_str());
                }
            }
        }
        if (!nodeAssignmentsPending && currentNodeIds != m_layoutSyncNodeIds) {
            std::lock_guard<std::mutex> commandLock(m_commandMutex);
            if (m_running.load() && m_broadcaster && !currentNodeIds.empty() &&
                !replayCurrentLayoutLocked()) {
                LOG_WARNING("Could not replay the saved layout after node discovery changed");
            }
            m_layoutSyncNodeIds = std::move(currentNodeIds);
        }
        for (int i = 0; i < 5 && m_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

bool MasterNodeApp::replayCurrentLayoutLocked() {
    if (!m_broadcaster) {
        return false;
    }

    MasterPlaybackStatus status;
    VideoInfo videoInfo;
    uint64_t delayMs = 0;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        status = m_playbackStatus;
        videoInfo = m_currentVideoInfo;
        delayMs = m_currentPlaybackDelayMs;
    }

    if (status.videoUrl.empty()) {
        return applyWindowLayoutLocked();
    }
    if (videoInfo.width == 0 || videoInfo.height == 0 ||
        (status.state != "scheduled" && status.state != "playing" &&
         status.state != "degraded")) {
        return true;
    }

    // PREPARE_PLAY does not stop nodes removed from the new crop list.
    if (!m_broadcaster->broadcastStop()) {
        LOG_WARNING("Live layout update could not stop the previous playback command");
    }

    try {
        dms::LayoutCalculator calculator;
        calculator.setLayout(getScreenLayout());
        calculator.setVideoInfo(videoInfo);
        calculator.setPlacements(getScreenPlacements());
        return syncPlay(status.videoUrl, videoInfo, calculator.calculateCropRegions(), delayMs);
    } catch (const std::exception& error) {
        LOG_ERROR("Cannot reapply live playback layout: %s", error.what());
        return false;
    }
}

bool MasterNodeApp::applyWindowLayoutLocked() {
    if (!m_broadcaster || !m_nodeDiscovery) return false;

    const ScreenLayout layout = getScreenLayout();
    std::vector<ScreenPlacement> screens = getScreenPlacements();
    const auto windows = getWindowPlacements();
    const auto nodes = m_nodeDiscovery->getNodes();
    if (screens.empty()) {
        screens.reserve(static_cast<size_t>(layout.rows) * layout.cols);
        for (uint32_t row = 0; row < layout.rows; ++row) {
            for (uint32_t col = 0; col < layout.cols; ++col) {
                screens.push_back({row * layout.cols + col + 1,
                                   static_cast<double>(col) / layout.cols,
                                   static_cast<double>(row) / layout.rows,
                                   1.0 / layout.cols, 1.0 / layout.rows});
            }
        }
    }

    const auto nodeById = [&nodes](uint32_t nodeId) -> const DiscoveredNode* {
        const auto found = std::find_if(nodes.begin(), nodes.end(),
            [nodeId](const DiscoveredNode& node) { return node.nodeId == nodeId; });
        return found == nodes.end() ? nullptr : &*found;
    };
    const auto scaleEdge = [](double value, uint32_t extent) -> uint32_t {
        const double bounded = std::clamp(value, 0.0, 1.0);
        return static_cast<uint32_t>(std::llround(bounded * extent));
    };

    bool success = true;
    for (const auto& screen : screens) {
        const DiscoveredNode* outputNode = nodeById(screen.nodeId);
        if (outputNode == nullptr || outputNode->status != "online" ||
            outputNode->ip.empty() || outputNode->advertisedNodeId == 0 ||
            (outputNode->nodeRole != NodeRole::DECODE &&
             outputNode->nodeRole != NodeRole::CODEC)) {
            continue;
        }

        Protocol::SetWindowLayoutCommand command;
        command.targetNodeId = outputNode->advertisedNodeId;
        command.outputWidth = layout.width;
        command.outputHeight = layout.height;
        std::set<uint32_t> includedSourceIds;

        for (const auto& window : windows) {
            const double intersectionLeft = std::max(screen.x, window.x);
            const double intersectionTop = std::max(screen.y, window.y);
            const double intersectionRight =
                std::min(screen.x + screen.width, window.x + window.width);
            const double intersectionBottom =
                std::min(screen.y + screen.height, window.y + window.height);
            if (intersectionRight <= intersectionLeft ||
                intersectionBottom <= intersectionTop) {
                continue;
            }

            const DiscoveredNode* sourceNode = nodeById(window.sourceNodeId);
            SignalSourceConfig sourceOverride;
            bool hasSourceOverride = false;
            {
                std::lock_guard<std::mutex> stateLock(m_stateMutex);
                const auto overrideIt = m_nodeSourceOverrides.find(window.sourceNodeId);
                if (overrideIt != m_nodeSourceOverrides.end()) {
                    sourceOverride = overrideIt->second;
                    hasSourceOverride = true;
                }
            }
            const bool sourceConfigured = hasSourceOverride
                ? sourceOverride.type != SignalSourceType::NONE
                : (sourceNode != nullptr && sourceNode->signalSourceConfigured);
            if (sourceNode == nullptr || sourceNode->status != "online" ||
                sourceNode->ip.empty() || sourceNode->kvmPort == 0 ||
                sourceNode->kvmPort >= 65535 || !sourceConfigured ||
                (sourceNode->nodeRole != NodeRole::ENCODE &&
                 sourceNode->nodeRole != NodeRole::CODEC)) {
                LOG_WARNING("Window %u source node %u is unavailable",
                            window.windowId, window.sourceNodeId);
                success = false;
                continue;
            }
            const uint32_t sourceWidth = hasSourceOverride && sourceOverride.width != 0
                ? sourceOverride.width
                : (sourceNode->signalSourceWidth != 0 ? sourceNode->signalSourceWidth : layout.width);
            const uint32_t sourceHeight = hasSourceOverride && sourceOverride.height != 0
                ? sourceOverride.height
                : (sourceNode->signalSourceHeight != 0 ? sourceNode->signalSourceHeight : layout.height);

            const double localLeft = (intersectionLeft - screen.x) / screen.width;
            const double localTop = (intersectionTop - screen.y) / screen.height;
            const double localRight = (intersectionRight - screen.x) / screen.width;
            const double localBottom = (intersectionBottom - screen.y) / screen.height;
            const uint32_t targetX = scaleEdge(localLeft, command.outputWidth);
            const uint32_t targetY = scaleEdge(localTop, command.outputHeight);
            const uint32_t targetRight = scaleEdge(localRight, command.outputWidth);
            const uint32_t targetBottom = scaleEdge(localBottom, command.outputHeight);
            if (targetRight <= targetX || targetBottom <= targetY ||
                sourceWidth == 0 || sourceHeight == 0) {
                continue;
            }

            command.layers.push_back({window.windowId, window.sourceNodeId,
                                      0, 0, sourceWidth, sourceHeight,
                                      targetX, targetY, targetRight - targetX,
                                      targetBottom - targetY, window.zOrder});
            if (includedSourceIds.insert(window.sourceNodeId).second) {
                const std::string fallbackEndpoint =
                    "http://" + sourceNode->ip + ":" +
                    std::to_string(static_cast<uint32_t>(sourceNode->kvmPort) + 1U) +
                    "/kvm/preview.jpg?stream=1";
                std::string endpoint = fallbackEndpoint;
                if (sourceNode->signalSourceTransport == "rtp-h264") {
                    const std::string rtpEndpoint =
                        Protocol::signalStreamEndpoint(sourceNode->advertisedNodeId);
                    if (!rtpEndpoint.empty()) {
                        endpoint = rtpEndpoint + "|fallback=" + fallbackEndpoint;
                    }
                }
                command.sources.push_back({
                    window.sourceNodeId,
                    endpoint,
                    sourceWidth,
                    sourceHeight});
            }
        }

        std::stable_sort(command.layers.begin(), command.layers.end(),
            [](const Protocol::WindowLayer& left, const Protocol::WindowLayer& right) {
                return left.zOrder < right.zOrder;
            });
        if (!m_broadcaster->sendSetWindowLayout(outputNode->ip, command)) {
            success = false;
        }
    }
    return success;
}

bool MasterNodeApp::setScreenLayout(const ScreenLayout& layout) {
    return setScreenLayout(layout, {});
}

bool MasterNodeApp::setScreenLayout(
    const ScreenLayout& layout,
    const std::vector<ScreenPlacement>& placements) {
    try {
        dms::LayoutCalculator calculator;
        calculator.setLayout(layout);
        calculator.setPlacements(placements);
    } catch (const std::exception& error) {
        LOG_ERROR("Invalid screen layout: %s", error.what());
        return false;
    }

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_screenLayout = layout;
    m_screenPlacements = placements;
    return true;
}

bool MasterNodeApp::updateScreenLayout(
    const ScreenLayout& layout,
    const std::vector<ScreenPlacement>& placements) {
    std::lock_guard<std::mutex> commandLock(m_commandMutex);
    if (!setScreenLayout(layout, placements)) {
        return false;
    }
    return replayCurrentLayoutLocked();
}

ScreenLayout MasterNodeApp::getScreenLayout() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_screenLayout;
}

std::vector<ScreenPlacement> MasterNodeApp::getScreenPlacements() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_screenPlacements;
}

bool MasterNodeApp::updateWindowLayout(
    const std::vector<WindowPlacement>& placements) {
    if (placements.size() > MAX_WINDOW_LAYERS) return false;
    std::set<uint32_t> windowIds;
    std::map<uint32_t, uint32_t> countsBySource;
    for (const auto& placement : placements) {
        if (placement.windowId == 0 || placement.sourceNodeId == 0 ||
            placement.zOrder == 0 || !std::isfinite(placement.x) ||
            !std::isfinite(placement.y) || !std::isfinite(placement.width) ||
            !std::isfinite(placement.height) || placement.x < 0.0 ||
            placement.y < 0.0 || placement.width <= 0.0 ||
            placement.height <= 0.0 || placement.x + placement.width > 1.0000001 ||
            placement.y + placement.height > 1.0000001 ||
            !windowIds.insert(placement.windowId).second ||
            ++countsBySource[placement.sourceNodeId] > MAX_WINDOWS_PER_SOURCE) {
            return false;
        }
    }

    if (!placements.empty()) {
        if (!m_nodeDiscovery) return false;
        const auto nodes = m_nodeDiscovery->getNodes();
        for (const auto& [sourceNodeId, count] : countsBySource) {
            (void)count;
            const auto node = std::find_if(nodes.begin(), nodes.end(),
                [sourceNodeId](const DiscoveredNode& value) {
                    return value.nodeId == sourceNodeId && value.status == "online";
                });
            if (node == nodes.end() || !node->signalSourceConfigured ||
                node->kvmPort == 0 || node->kvmPort >= 65535 ||
                (node->nodeRole != NodeRole::ENCODE && node->nodeRole != NodeRole::CODEC)) {
                return false;
            }
        }
    }

    std::lock_guard<std::mutex> commandLock(m_commandMutex);
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_windowPlacements = placements;
    }
    return applyWindowLayoutLocked();
}

std::vector<WindowPlacement> MasterNodeApp::getWindowPlacements() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_windowPlacements;
}

MasterPlaybackStatus MasterNodeApp::getPlaybackStatus() const {
    MasterPlaybackStatus status;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        status = m_playbackStatus;
    }
    const uint64_t now = Protocol::getCurrentTimestamp();
    if (status.state == "scheduled" && status.syncTimestamp != 0 && now >= status.syncTimestamp) {
        status.state = "playing";
        status.updatedAt = now;
    }
    return aggregateNodeStatus(status);
}

std::string MasterNodeApp::getLocalHttpUrl() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_localHttpUrl;
}

bool MasterNodeApp::setLocalIp(const std::string& localIp) {
    struct in_addr address {};
    if (localIp.empty() || inet_pton(AF_INET, localIp.c_str(), &address) != 1) {
        LOG_ERROR("Invalid local IPv4 address: %s", localIp.c_str());
        return false;
    }
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_localIp = localIp;
    m_localHttpUrl = "http://" + m_localIp + ":" + std::to_string(m_localHttpPort);
    return true;
}

bool MasterNodeApp::preloadVideo(const std::string& videoUrl) {
    return m_cacheManager && m_cacheManager->preloadVideo(videoUrl);
}

std::string MasterNodeApp::convertToAccessibleUrl(const std::string& videoUrl) {
    if (videoUrl.rfind("http://", 0) == 0 || videoUrl.rfind("https://", 0) == 0) {
        return videoUrl;
    }
    if (!m_cacheManager) {
        return {};
    }

    std::string localPath;
    if (!m_cacheManager->getVideo(videoUrl, localPath)) {
        LOG_ERROR("Failed to cache local video: %s", videoUrl.c_str());
        return {};
    }
    const std::string filename = std::filesystem::path(localPath).filename().string();
    return getLocalHttpUrl() + "/media/" + filename;
}

void MasterNodeApp::updatePlaybackStatus(const std::string& state,
                                         const std::string& videoUrl,
                                         uint32_t commandId,
                                         uint64_t syncTimestamp) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_playbackStatus.commandId != commandId) {
        m_observedCommandNodeIds.clear();
    }
    m_playbackStatus.state = state;
    m_playbackStatus.videoUrl = videoUrl;
    m_playbackStatus.commandId = commandId;
    m_playbackStatus.syncTimestamp = syncTimestamp;
    m_playbackStatus.updatedAt = Protocol::getCurrentTimestamp();
    m_playbackStatus.expectedNodeCount = static_cast<uint32_t>(m_expectedNodeIds.size());
    m_playbackStatus.readyNodeCount = 0;
    m_playbackStatus.playingNodeCount = 0;
    m_playbackStatus.errorNodeCount = 0;
    m_playbackStatus.missingNodeCount = 0;
    m_playbackStatus.maxStartSkewMs = 0;
}

void MasterNodeApp::setExpectedNodeIds(const std::vector<Protocol::CropRegion>& layoutConfig) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_expectedNodeIds.clear();
    m_observedCommandNodeIds.clear();
    for (const auto& crop : layoutConfig) {
        m_expectedNodeIds.insert(crop.nodeId);
    }
    m_playbackStatus.expectedNodeCount = static_cast<uint32_t>(m_expectedNodeIds.size());
}

void MasterNodeApp::clearExpectedNodeIds() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_expectedNodeIds.clear();
    m_observedCommandNodeIds.clear();
    m_playbackStatus.expectedNodeCount = 0;
    m_playbackStatus.readyNodeCount = 0;
    m_playbackStatus.playingNodeCount = 0;
    m_playbackStatus.errorNodeCount = 0;
    m_playbackStatus.missingNodeCount = 0;
    m_playbackStatus.maxStartSkewMs = 0;
}

MasterPlaybackStatus MasterNodeApp::aggregateNodeStatus(const MasterPlaybackStatus& status) const {
    if (status.commandId == 0 || !m_nodeDiscovery) {
        return status;
    }

    std::set<uint32_t> expectedNodeIds;
    std::set<uint32_t> observedCommandNodeIds;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        expectedNodeIds = m_expectedNodeIds;
        observedCommandNodeIds = m_observedCommandNodeIds;
    }
    if (expectedNodeIds.empty()) {
        return status;
    }

    const auto nodes = m_nodeDiscovery->getNodes();
    std::map<uint32_t, DiscoveredNode> reports;
    for (const auto& node : nodes) {
        reports[node.nodeId] = node;
    }

    MasterPlaybackStatus aggregated = status;
    aggregated.expectedNodeCount = static_cast<uint32_t>(expectedNodeIds.size());
    uint32_t observedCurrentNodeCount = 0;
    aggregated.readyNodeCount = 0;
    aggregated.playingNodeCount = 0;
    aggregated.errorNodeCount = 0;
    aggregated.missingNodeCount = 0;
    aggregated.maxStartSkewMs = 0;
    bool hasStartTimestamp = false;

    for (const uint32_t nodeId : expectedNodeIds) {
        const auto reportIt = reports.find(nodeId);
        if (reportIt == reports.end() || reportIt->second.commandId != status.commandId) {
            ++aggregated.missingNodeCount;
            continue;
        }
        const DiscoveredNode& report = reportIt->second;
        ++observedCurrentNodeCount;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_observedCommandNodeIds.insert(nodeId);
        }
        if (report.playState == "ready" || report.playState == "paused" ||
            report.playState == "playing") {
            ++aggregated.readyNodeCount;
        }
        if (report.playState == "playing") {
            ++aggregated.playingNodeCount;
        }
        if (report.playState == "error" || !report.lastError.empty()) {
            ++aggregated.errorNodeCount;
        }
        if (report.actualStartTimestamp != 0 && status.syncTimestamp != 0) {
            const int64_t skew = calculateStartSkew(
                report.actualStartTimestamp, status.syncTimestamp);
            if (!hasStartTimestamp || std::llabs(skew) > std::llabs(aggregated.maxStartSkewMs)) {
                aggregated.maxStartSkewMs = skew;
                hasStartTimestamp = true;
            }
        }
    }

    const bool commandHasStarted = status.syncTimestamp != 0 &&
        Protocol::getCurrentTimestamp() >= status.syncTimestamp;
    const bool partialReportAvailable = observedCurrentNodeCount > 0 ||
        !observedCommandNodeIds.empty();
    if (aggregated.errorNodeCount > 0 ||
        (aggregated.missingNodeCount > 0 && commandHasStarted && partialReportAvailable)) {
        if (status.state == "scheduled" || status.state == "playing") {
            aggregated.state = "degraded";
        }
    } else if ((status.state == "scheduled" || status.state == "degraded") &&
               aggregated.playingNodeCount == aggregated.expectedNodeCount &&
               aggregated.expectedNodeCount > 0) {
        aggregated.state = "playing";
    }
    return aggregated;
}

} // namespace dms
