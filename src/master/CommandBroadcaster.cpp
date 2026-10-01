#include "master/CommandBroadcaster.h"
#include "common/Logger.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <exception>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace dms {

CommandBroadcaster::CommandBroadcaster()
    : m_socket(-1)
    , m_sequenceId(0)
    , m_sentPacketCount(0)
    , m_failedPacketCount(0)
    , m_initialized(false) {
    std::memset(&m_multicastAddr, 0, sizeof(m_multicastAddr));
}

CommandBroadcaster::~CommandBroadcaster() {
    shutdown();
}

bool CommandBroadcaster::initialize(const char* multicastGroup, uint16_t port) {
    if (m_initialized.load() || m_socket >= 0 || multicastGroup == nullptr ||
        *multicastGroup == '\0' || port == 0) {
        LOG_ERROR("CommandBroadcaster options are invalid or already initialized");
        return false;
    }

    in_addr parsedAddress{};
    if (inet_pton(AF_INET, multicastGroup, &parsedAddress) != 1 ||
        !IN_MULTICAST(ntohl(parsedAddress.s_addr))) {
        LOG_ERROR("Invalid multicast address: %s", multicastGroup);
        return false;
    }

    m_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socket < 0) {
        LOG_ERROR("Failed to create command broadcaster socket: %s", std::strerror(errno));
        return false;
    }

    const int ttl = 64;
    if (setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) != 0) {
        LOG_ERROR("Failed to set multicast TTL: %s", std::strerror(errno));
        close(m_socket);
        m_socket = -1;
        return false;
    }
    const unsigned char loopback = 1;
    if (setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_LOOP,
                   &loopback, sizeof(loopback)) != 0) {
        LOG_ERROR("Failed to enable local multicast loopback: %s", std::strerror(errno));
        close(m_socket);
        m_socket = -1;
        return false;
    }

    m_multicastAddr = {};
    m_multicastAddr.sin_family = AF_INET;
    m_multicastAddr.sin_port = htons(port);
    m_multicastAddr.sin_addr = parsedAddress;
    m_sequenceId.store(0);
    m_sentPacketCount.store(0);
    m_failedPacketCount.store(0);
    m_initialized.store(true);
    LOG_INFO("CommandBroadcaster initialized: %s:%u", multicastGroup, port);
    return true;
}

void CommandBroadcaster::shutdown() {
    if (!m_initialized.exchange(false)) {
        return;
    }
    if (m_socket >= 0) {
        close(m_socket);
        m_socket = -1;
    }
    LOG_INFO("CommandBroadcaster shutdown (sent: %lu, failed: %lu)",
             m_sentPacketCount.load(), m_failedPacketCount.load());
}

uint32_t CommandBroadcaster::nextSequenceId() {
    uint32_t id = m_sequenceId.fetch_add(1) + 1;
    if (id == 0) {
        id = m_sequenceId.fetch_add(1) + 1;
    }
    return id;
}

bool CommandBroadcaster::broadcastPreparePlay(
    const std::string& videoUrl,
    uint32_t videoWidth,
    uint32_t videoHeight,
    uint64_t syncTimestamp,
    const std::vector<Protocol::CropRegion>& crops,
    uint32_t& commandId) {
    if (!m_initialized.load()) {
        LOG_ERROR("CommandBroadcaster not initialized");
        return false;
    }
    commandId = nextSequenceId();
    const Protocol::Packet packet = Protocol::createPreparePlayPacket(
        commandId, videoUrl, videoWidth, videoHeight, syncTimestamp, crops);
    LOG_INFO("Broadcasting PREPARE_PLAY: command=%u, video=%s, size=%ux%u, timestamp=%lu, nodes=%zu",
             commandId, videoUrl.c_str(), videoWidth, videoHeight, syncTimestamp, crops.size());
    if (!sendPacket(packet)) {
        commandId = 0;
        return false;
    }
    return true;
}

bool CommandBroadcaster::broadcastCommitPlay(uint32_t commandId) {
    if (!m_initialized.load() || commandId == 0) {
        LOG_ERROR("Invalid COMMIT_PLAY command");
        return false;
    }
    const Protocol::Packet packet = Protocol::createCommitPlayPacket(nextSequenceId(), commandId);
    LOG_INFO("Broadcasting COMMIT_PLAY: command=%u", commandId);
    return sendPacket(packet);
}

bool CommandBroadcaster::broadcastSyncPlay(
    const std::string& videoUrl,
    uint32_t videoWidth,
    uint32_t videoHeight,
    uint64_t syncTimestamp,
    const std::vector<Protocol::CropRegion>& crops) {
    uint32_t commandId = 0;
    return broadcastPreparePlay(videoUrl, videoWidth, videoHeight, syncTimestamp, crops, commandId) &&
           broadcastCommitPlay(commandId);
}

bool CommandBroadcaster::broadcastStop() {
    if (!m_initialized.load()) {
        LOG_ERROR("CommandBroadcaster not initialized");
        return false;
    }
    const uint32_t sequenceId = nextSequenceId();
    LOG_INFO("Broadcasting STOP: seq=%u", sequenceId);
    return sendPacket(Protocol::createStopPacket(sequenceId));
}

bool CommandBroadcaster::broadcastPause() {
    if (!m_initialized.load()) {
        LOG_ERROR("CommandBroadcaster not initialized");
        return false;
    }
    const uint32_t sequenceId = nextSequenceId();
    LOG_INFO("Broadcasting PAUSE: seq=%u", sequenceId);
    return sendPacket(Protocol::createPausePacket(sequenceId));
}

bool CommandBroadcaster::broadcastResume() {
    if (!m_initialized.load()) {
        LOG_ERROR("CommandBroadcaster not initialized");
        return false;
    }
    const uint32_t sequenceId = nextSequenceId();
    LOG_INFO("Broadcasting RESUME: seq=%u", sequenceId);
    return sendPacket(Protocol::createResumePacket(sequenceId));
}

bool CommandBroadcaster::broadcastSetAudioOutput(AudioOutputMode mode) {
    if (!m_initialized.load()) {
        LOG_ERROR("CommandBroadcaster not initialized");
        return false;
    }
    const uint32_t sequenceId = nextSequenceId();
    LOG_INFO("Broadcasting SET_AUDIO_OUTPUT: seq=%u, mode=%s",
             sequenceId, audioOutputModeName(mode));
    return sendPacket(Protocol::createSetAudioOutputPacket(sequenceId, mode));
}

bool CommandBroadcaster::broadcastSetAudioVolume(uint32_t volumePercent) {
    if (!m_initialized.load() || volumePercent > 100) {
        LOG_ERROR("Invalid audio volume percent: %u", volumePercent);
        return false;
    }
    const uint32_t sequenceId = nextSequenceId();
    LOG_INFO("Broadcasting SET_AUDIO_VOLUME: seq=%u, volume=%u%%",
             sequenceId, volumePercent);
    return sendPacket(Protocol::createSetAudioVolumePacket(sequenceId, volumePercent));
}

bool CommandBroadcaster::broadcastKvmRoute(const KvmSession& session) {
    if (!m_initialized.load()) return false;
    Protocol::KvmRouteCommand command;
    command.sessionId = session.sessionId;
    command.controllerNodeId = session.controllerNodeId;
    command.targetNodeId = session.targetNodeId;
    command.targetIp = session.targetIp;
    command.targetPort = session.targetPort;
    command.sessionToken = session.sessionToken;
    command.expiresAt = session.expiresAt;
    const uint32_t sequenceId = nextSequenceId();
    LOG_INFO("Broadcasting KVM_ROUTE: seq=%u session=%u controller=%u target=%u",
             sequenceId, session.sessionId, session.controllerNodeId, session.targetNodeId);
    return sendPacket(Protocol::createKvmRoutePacket(sequenceId, command));
}

bool CommandBroadcaster::broadcastKvmRelease(uint32_t sessionId) {
    if (!m_initialized.load() || sessionId == 0) return false;
    const uint32_t sequenceId = nextSequenceId();
    LOG_INFO("Broadcasting KVM_RELEASE: seq=%u session=%u", sequenceId, sessionId);
    return sendPacket(Protocol::createKvmReleasePacket(sequenceId, sessionId));
}

bool CommandBroadcaster::broadcastSetDisplayLayout(
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    const std::vector<Protocol::CropRegion>& crops) {
    if (!m_initialized.load()) return false;
    try {
        LOG_INFO("Broadcasting SET_DISPLAY_LAYOUT: source=%ux%u crops=%zu",
                 sourceWidth, sourceHeight, crops.size());
        return sendPacket(Protocol::createSetDisplayLayoutPacket(
            nextSequenceId(), sourceWidth, sourceHeight, crops));
    } catch (const std::exception& error) {
        LOG_ERROR("Cannot broadcast display layout: %s", error.what());
        return false;
    }
}

bool CommandBroadcaster::sendSetNodeRole(
    const std::string& targetIp, uint32_t nodeId, NodeRole role) {
    if (!m_initialized.load() || nodeId == 0) return false;
    return sendPacketTo(
        Protocol::createSetNodeRolePacket(nextSequenceId(), nodeId, role), targetIp);
}

bool CommandBroadcaster::sendSetNodeNetwork(
    const std::string& targetIp,
    const Protocol::SetNodeNetworkCommand& command) {
    if (!m_initialized.load() || !Protocol::validateSetNodeNetworkCommand(command)) return false;
    return sendPacketTo(Protocol::createSetNodeNetworkPacket(nextSequenceId(), command), targetIp);
}

bool CommandBroadcaster::sendSetNodeSource(
    const std::string& targetIp,
    const Protocol::SetNodeSourceCommand& command) {
    if (!m_initialized.load()) return false;
    try {
        return sendPacketTo(Protocol::createSetNodeSourcePacket(nextSequenceId(), command), targetIp);
    } catch (const std::exception& error) {
        LOG_ERROR("Cannot send node signal source: %s", error.what());
        return false;
    }
}

bool CommandBroadcaster::sendSetNodeId(
    const std::string& targetIp,
    const Protocol::SetNodeIdCommand& command) {
    if (!m_initialized.load()) return false;
    try {
        return sendPacketTo(Protocol::createSetNodeIdPacket(nextSequenceId(), command), targetIp);
    } catch (const std::exception& error) {
        LOG_ERROR("Cannot send node ID assignment: %s", error.what());
        return false;
    }
}

bool CommandBroadcaster::sendSetWindowLayout(
    const std::string& targetIp,
    const Protocol::SetWindowLayoutCommand& command) {
    if (!m_initialized.load() || targetIp.empty()) return false;
    try {
        LOG_INFO("Sending SET_WINDOW_LAYOUT: target=%u ip=%s sources=%zu layers=%zu",
                 command.targetNodeId, targetIp.c_str(), command.sources.size(),
                 command.layers.size());
        return sendPacketTo(
            Protocol::createSetWindowLayoutPacket(nextSequenceId(), command), targetIp);
    } catch (const std::exception& error) {
        LOG_ERROR("Cannot send window layout: %s", error.what());
        return false;
    }
}

bool CommandBroadcaster::sendPacket(const Protocol::Packet& packet) {
    return sendPacket(packet, m_multicastAddr);
}

bool CommandBroadcaster::sendPacketTo(
    const Protocol::Packet& packet,
    const std::string& targetIp) {
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = m_multicastAddr.sin_port;
    if (inet_pton(AF_INET, targetIp.c_str(), &destination.sin_addr) != 1) {
        LOG_ERROR("Invalid target IPv4 address: %s", targetIp.c_str());
        return false;
    }
    return sendPacket(packet, destination);
}

bool CommandBroadcaster::sendPacket(
    const Protocol::Packet& packet,
    const sockaddr_in& destination) {
    if (!m_initialized.load()) {
        return false;
    }
    const std::vector<uint8_t> buffer = Protocol::serialize(packet);
    constexpr unsigned SEND_ATTEMPTS = 3;
    constexpr auto RETRY_INTERVAL = std::chrono::milliseconds(5);
    bool sentAny = false;
    for (unsigned attempt = 0; attempt < SEND_ATTEMPTS; ++attempt) {
        sentAny = sendData(buffer.data(), buffer.size(), destination) || sentAny;
        if (attempt + 1 < SEND_ATTEMPTS) {
            std::this_thread::sleep_for(RETRY_INTERVAL);
        }
    }
    if (sentAny) {
        m_sentPacketCount.fetch_add(1);
    } else {
        m_failedPacketCount.fetch_add(1);
    }
    return sentAny;
}

bool CommandBroadcaster::sendData(
    const uint8_t* data,
    size_t length,
    const sockaddr_in& destination) {
    if (data == nullptr || length == 0 || length > MAX_PACKET_SIZE || m_socket < 0) {
        return false;
    }
    const ssize_t sent = sendto(m_socket, data, length, 0,
                                reinterpret_cast<const struct sockaddr*>(&destination),
                                sizeof(destination));
    if (sent != static_cast<ssize_t>(length)) {
        LOG_ERROR("Failed to send complete UDP packet: %s", std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace dms
