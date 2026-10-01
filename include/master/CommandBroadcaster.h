#pragma once

#include "common/Protocol.h"
#include "kvm/KvmTypes.h"

#include <atomic>
#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <vector>

namespace dms {

class CommandBroadcaster {
public:
    CommandBroadcaster();
    ~CommandBroadcaster();

    CommandBroadcaster(const CommandBroadcaster&) = delete;
    CommandBroadcaster& operator=(const CommandBroadcaster&) = delete;

    bool initialize(const char* multicastGroup, uint16_t port);
    void shutdown();

    bool broadcastSyncPlay(const std::string& videoUrl,
                           uint32_t videoWidth,
                           uint32_t videoHeight,
                           uint64_t syncTimestamp,
                           const std::vector<Protocol::CropRegion>& crops);
    bool broadcastPreparePlay(const std::string& videoUrl,
                              uint32_t videoWidth,
                              uint32_t videoHeight,
                              uint64_t syncTimestamp,
                              const std::vector<Protocol::CropRegion>& crops,
                              uint32_t& commandId);
    bool broadcastCommitPlay(uint32_t commandId);
    bool broadcastStop();
    bool broadcastPause();
    bool broadcastResume();
    bool broadcastSetAudioOutput(AudioOutputMode mode);
    bool broadcastSetAudioVolume(uint32_t volumePercent);
    bool broadcastKvmRoute(const KvmSession& session);
    bool broadcastKvmRelease(uint32_t sessionId);
    bool broadcastSetDisplayLayout(uint32_t sourceWidth,
                                   uint32_t sourceHeight,
                                   const std::vector<Protocol::CropRegion>& crops);
    bool sendSetNodeRole(const std::string& targetIp, uint32_t nodeId, NodeRole role);
    bool sendSetNodeNetwork(const std::string& targetIp,
                            const Protocol::SetNodeNetworkCommand& command);
    bool sendSetNodeSource(const std::string& targetIp,
                           const Protocol::SetNodeSourceCommand& command);
    bool sendSetNodeId(const std::string& targetIp,
                       const Protocol::SetNodeIdCommand& command);
    bool sendSetWindowLayout(const std::string& targetIp,
                             const Protocol::SetWindowLayoutCommand& command);

    uint64_t getSentPacketCount() const { return m_sentPacketCount.load(); }
    uint64_t getFailedPacketCount() const { return m_failedPacketCount.load(); }
    bool isInitialized() const { return m_initialized.load(); }

private:
    bool sendPacket(const Protocol::Packet& packet);
    bool sendPacket(const Protocol::Packet& packet, const struct sockaddr_in& destination);
    bool sendPacketTo(const Protocol::Packet& packet, const std::string& targetIp);
    bool sendData(const uint8_t* data, size_t length,
                  const struct sockaddr_in& destination);
    uint32_t nextSequenceId();

    int m_socket;
    struct sockaddr_in m_multicastAddr;
    std::atomic<uint32_t> m_sequenceId;
    std::atomic<uint64_t> m_sentPacketCount;
    std::atomic<uint64_t> m_failedPacketCount;
    std::atomic<bool> m_initialized;
};

} // namespace dms
