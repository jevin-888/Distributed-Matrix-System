#pragma once

#include "common/Protocol.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <netinet/in.h>
#include <thread>

namespace dms {

using CommandCallback = std::function<void(const Protocol::Packet& packet)>;

class CommandReceiver {
public:
    CommandReceiver();
    ~CommandReceiver();

    CommandReceiver(const CommandReceiver&) = delete;
    CommandReceiver& operator=(const CommandReceiver&) = delete;

    bool initialize(const char* multicastGroup, uint16_t port, CommandCallback callback);
    bool start();
    void shutdown();

    uint64_t getReceivedPacketCount() const { return m_receivedPacketCount.load(); }
    uint64_t getFailedPacketCount() const { return m_failedPacketCount.load(); }
    bool isRunning() const { return m_running.load(); }

private:
    struct PacketIdentity {
        uint32_t commandType = 0;
        uint32_t sequenceId = 0;
        uint64_t timestamp = 0;

        bool operator==(const PacketIdentity& other) const {
            return commandType == other.commandType && sequenceId == other.sequenceId &&
                   timestamp == other.timestamp;
        }
    };

    bool joinMulticastGroup(bool logFailure);
    void receiveThreadFunc();
    void processReceivedData(const uint8_t* data, size_t length);

    int m_socket;
    struct sockaddr_in m_localAddr;
    struct ip_mreq m_multicastRequest;
    CommandCallback m_callback;
    std::thread m_receiveThread;
    std::atomic<bool> m_running;
    bool m_multicastJoined;
    std::atomic<uint64_t> m_receivedPacketCount;
    std::atomic<uint64_t> m_failedPacketCount;
    std::deque<PacketIdentity> m_recentPackets;
};

} // namespace dms
