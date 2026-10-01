#include "node/CommandReceiver.h"
#include "common/Logger.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dms {

CommandReceiver::CommandReceiver()
    : m_socket(-1)
    , m_running(false)
    , m_multicastJoined(false)
    , m_receivedPacketCount(0)
    , m_failedPacketCount(0) {
    std::memset(&m_localAddr, 0, sizeof(m_localAddr));
    std::memset(&m_multicastRequest, 0, sizeof(m_multicastRequest));
}

CommandReceiver::~CommandReceiver() {
    shutdown();
}

bool CommandReceiver::initialize(const char* multicastGroup,
                                 uint16_t port,
                                 CommandCallback callback) {
    if (m_socket >= 0 || m_running.load()) {
        LOG_ERROR("CommandReceiver is already initialized");
        return false;
    }
    if (multicastGroup == nullptr || *multicastGroup == '\0' || port == 0 || !callback) {
        LOG_ERROR("CommandReceiver options are invalid");
        return false;
    }

    in_addr parsedGroup {};
    if (inet_pton(AF_INET, multicastGroup, &parsedGroup) != 1 ||
        !IN_MULTICAST(ntohl(parsedGroup.s_addr))) {
        LOG_ERROR("Invalid multicast address: %s", multicastGroup);
        return false;
    }

    m_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socket < 0) {
        LOG_ERROR("Failed to create command receiver socket: %s", std::strerror(errno));
        return false;
    }

    const int reuse = 1;
    timeval timeout {};
    timeout.tv_usec = 200000;
    if (setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        LOG_ERROR("Failed to configure command receiver socket: %s", std::strerror(errno));
        close(m_socket);
        m_socket = -1;
        return false;
    }

    m_localAddr = {};
    m_localAddr.sin_family = AF_INET;
    m_localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    m_localAddr.sin_port = htons(port);
    if (bind(m_socket, reinterpret_cast<sockaddr*>(&m_localAddr), sizeof(m_localAddr)) != 0) {
        LOG_ERROR("Failed to bind command receiver socket: %s", std::strerror(errno));
        close(m_socket);
        m_socket = -1;
        return false;
    }

    m_multicastRequest = {};
    m_multicastRequest.imr_multiaddr = parsedGroup;
    m_multicastRequest.imr_interface.s_addr = htonl(INADDR_ANY);
    m_multicastJoined = false;
    if (!joinMulticastGroup(true)) {
        LOG_WARNING("Command receiver will retry multicast membership after the network becomes ready");
    }

    m_callback = std::move(callback);
    m_receivedPacketCount.store(0);
    m_failedPacketCount.store(0);
    m_recentPackets.clear();
    LOG_INFO("CommandReceiver initialized: %s:%u", multicastGroup, port);
    return true;
}

void CommandReceiver::shutdown() {
    m_running.store(false);
    if (m_receiveThread.joinable()) {
        m_receiveThread.join();
    }
    if (m_socket >= 0) {
        if (m_multicastJoined) {
            setsockopt(m_socket, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                       &m_multicastRequest, sizeof(m_multicastRequest));
        }
        close(m_socket);
        m_socket = -1;
    }
    m_multicastJoined = false;
    m_callback = {};
    m_recentPackets.clear();
}

bool CommandReceiver::joinMulticastGroup(bool logFailure) {
    if (m_multicastJoined) {
        return true;
    }
    if (m_socket < 0) {
        if (logFailure) {
            LOG_ERROR("Cannot join command multicast group without a valid socket");
        }
        return false;
    }
    if (setsockopt(m_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &m_multicastRequest, sizeof(m_multicastRequest)) != 0) {
        if (logFailure) {
            LOG_WARNING("Failed to join command multicast group: %s", std::strerror(errno));
        }
        return false;
    }
    m_multicastJoined = true;
    LOG_INFO("Command multicast membership is active");
    return true;
}

bool CommandReceiver::start() {
    if (m_socket < 0 || !m_callback || m_running.exchange(true)) {
        LOG_ERROR("CommandReceiver cannot start in its current state");
        return false;
    }
    try {
        m_receiveThread = std::thread(&CommandReceiver::receiveThreadFunc, this);
    } catch (const std::exception& error) {
        m_running.store(false);
        LOG_ERROR("Failed to start command receiver thread: %s", error.what());
        return false;
    }
    return true;
}

void CommandReceiver::receiveThreadFunc() {
    uint8_t buffer[MAX_PACKET_SIZE];
    uint32_t failedJoinAttempts = 0;
    while (m_running.load()) {
        if (!m_multicastJoined) {
            const bool logFailure = (failedJoinAttempts % 30U) == 0U;
            if (!joinMulticastGroup(logFailure)) {
                ++failedJoinAttempts;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            failedJoinAttempts = 0;
        }

        const ssize_t received = recvfrom(m_socket, buffer, sizeof(buffer), 0, nullptr, nullptr);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            if (m_running.load()) {
                LOG_ERROR("Command receive failed: %s", std::strerror(errno));
                m_failedPacketCount.fetch_add(1);
            }
            continue;
        }
        if (received == 0) {
            m_failedPacketCount.fetch_add(1);
            continue;
        }
        processReceivedData(buffer, static_cast<size_t>(received));
    }
}

void CommandReceiver::processReceivedData(const uint8_t* data, size_t length) {
    try {
        Protocol::Packet packet = Protocol::deserialize(data, length);
        if (!Protocol::verifyChecksum(packet)) {
            m_failedPacketCount.fetch_add(1);
            return;
        }

        const PacketIdentity identity{
            packet.header.commandType,
            packet.header.sequenceId,
            packet.header.timestamp};
        if (std::find(m_recentPackets.begin(), m_recentPackets.end(), identity) !=
            m_recentPackets.end()) {
            return;
        }

        constexpr size_t RECENT_PACKET_WINDOW = 128;
        m_recentPackets.push_back(identity);
        if (m_recentPackets.size() > RECENT_PACKET_WINDOW) {
            m_recentPackets.pop_front();
        }
        m_receivedPacketCount.fetch_add(1);
        m_callback(packet);
    } catch (const std::exception& error) {
        LOG_WARNING("Ignoring invalid command packet: %s", error.what());
        m_failedPacketCount.fetch_add(1);
    }
}

} // namespace dms
