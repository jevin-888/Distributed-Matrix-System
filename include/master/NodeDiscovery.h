#pragma once

#include "kvm/KvmTypes.h"
#include "common/NodeRole.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dms {

struct DiscoveredNode {
    // The ID exposed by the Master. It normally matches the heartbeat's
    // advertised ID; duplicate IDs are reassigned per endpoint during
    // discovery so separate devices remain visible in the node list.
    uint32_t nodeId = 0;
    uint32_t advertisedNodeId = 0;
    NodeRole nodeRole = NodeRole::UNASSIGNED;
    std::string ipMode = "auto";
    std::string ip;
    std::string deviceModel;
    std::string deviceName;
    std::string softwareVersion;
    std::string boardInfo;
    std::string macAddress;
    std::string subnetMask;
    std::string gateway;
    std::string deviceType;
    std::string status = "online";
    std::string playState = "idle";
    std::string resolution;
    double cpu = 0.0;
    double memory = 0.0;
    std::string audioOutputMode = "both";
    bool masterClockSynchronized = false;
    int64_t masterClockOffsetMs = 0;
    uint32_t commandId = 0;
    uint64_t actualStartTimestamp = 0;
    std::string lastError;
    bool kvmEnabled = false;
    KvmRole kvmRole = KvmRole::DISABLED;
    uint16_t kvmPort = 0;
    uint32_t kvmSessionId = 0;
    std::string kvmState = "disabled";
    std::string kvmLastError;
    std::string signalSourceType = "none";
    bool signalSourceConfigured = false;
    bool signalSourceActive = false;
    std::string signalSourceEndpoint;
    uint32_t signalSourceWidth = 0;
    uint32_t signalSourceHeight = 0;
    uint32_t signalSourceFramerateNumerator = 0;
    uint32_t signalSourceFramerateDenominator = 1;
    std::string signalSourcePixelFormat = "auto";
    // "mjpeg" is the interoperable fallback; "rtp-h264" is advertised only
    // after the local RK MPP encoder and RTP factories pass capability checks.
    std::string signalSourceTransport = "mjpeg";
    uint64_t lastSeen = 0;
};

struct DiscoveryScanResult {
    bool success = false;
    bool busy = false;
    std::vector<std::string> networks;
    uint32_t addressesProbed = 0;
    uint32_t nodesFound = 0;
    uint32_t totalNodes = 0;
    uint64_t elapsedMs = 0;
    std::string error;
};

class NodeDiscovery {
public:
    NodeDiscovery();
    ~NodeDiscovery();

    bool initialize(const std::string& multicastAddress,
                    uint16_t port,
                    uint16_t discoveryPort = 9003,
                    const std::vector<std::string>& discoveryNetworks = {});
    bool start();
    void shutdown();
    std::vector<DiscoveredNode> getNodes() const;
    DiscoveryScanResult discover(uint32_t timeoutMs = 1500);

private:
    void listenThread();
    void cleanupThread();
    bool processHeartbeat(const std::string& message,
                          uint32_t* processedNodeId = nullptr,
                          const std::string& sourceIp = {});

    int m_socket;
    std::atomic<bool> m_running;
    std::thread m_listenThread;
    std::thread m_cleanupThread;
    std::map<uint32_t, DiscoveredNode> m_nodes;
    mutable std::mutex m_nodesMutex;
    std::string m_multicastAddress;
    uint16_t m_port;
    uint16_t m_discoveryPort;
    std::vector<std::string> m_discoveryNetworks;
    std::atomic<bool> m_scanning;

    static constexpr uint64_t NODE_TIMEOUT_MS = 10000;
};

} // namespace dms
