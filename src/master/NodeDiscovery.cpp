#include "master/NodeDiscovery.h"
#include "common/DiscoveryProtocol.h"
#include "common/Logger.h"
#include "common/Protocol.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cmath>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <limits>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <sys/select.h>
#include <sys/socket.h>
#include <unordered_set>
#include <unistd.h>

using json = nlohmann::json;

namespace dms {

namespace {

constexpr uint32_t MAX_DISCOVERY_ADDRESSES = 65536;

bool hasExactFieldsWithOptional(const json& value,
                                std::initializer_list<const char*> fields,
                                std::initializer_list<const char*> optional) {
    if (!value.is_object()) return false;
    std::set<std::string> allowed;
    for (const char* field : fields) {
        allowed.emplace(field);
        if (!value.contains(field)) return false;
    }
    for (const char* field : optional) allowed.emplace(field);
    for (const auto& item : value.items()) {
        if (allowed.count(item.key()) == 0) return false;
    }
    return true;
}

struct CidrNetwork {
    uint32_t address = 0;
    uint8_t prefix = 0;
};

bool parseCidr(const std::string& value, CidrNetwork& result) {
    const size_t separator = value.find('/');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size()) {
        return false;
    }
    const std::string addressText = value.substr(0, separator);
    const std::string prefixText = value.substr(separator + 1);
    if (prefixText.empty() || prefixText.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    unsigned long prefix = 0;
    try {
        prefix = std::stoul(prefixText);
    } catch (...) {
        return false;
    }
    if (prefix < 1 || prefix > 32) {
        return false;
    }
    in_addr address{};
    if (inet_pton(AF_INET, addressText.c_str(), &address) != 1) {
        return false;
    }
    const uint32_t hostAddress = ntohl(address.s_addr);
    const uint32_t mask = prefix == 32 ? 0xffffffffU : 0xffffffffU << (32U - prefix);
    result.address = hostAddress & mask;
    result.prefix = static_cast<uint8_t>(prefix);
    return true;
}

std::string cidrText(uint32_t address, uint8_t prefix) {
    in_addr value{};
    value.s_addr = htonl(address);
    char buffer[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &value, buffer, sizeof(buffer))) {
        return {};
    }
    return std::string(buffer) + "/" + std::to_string(prefix);
}

void addNetwork(std::vector<std::string>& networks, std::set<std::string>& seen,
                uint32_t address, uint8_t prefix) {
    const std::string value = cidrText(address, prefix);
    if (!value.empty() && seen.insert(value).second) {
        networks.push_back(value);
    }
}

void collectInterfaceNetworks(std::vector<std::string>& networks, std::set<std::string>& seen) {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return;
    }
    for (ifaddrs* interfaceValue = interfaces; interfaceValue != nullptr;
         interfaceValue = interfaceValue->ifa_next) {
        if (!interfaceValue->ifa_addr || !interfaceValue->ifa_netmask ||
            interfaceValue->ifa_addr->sa_family != AF_INET ||
            (interfaceValue->ifa_flags & IFF_UP) == 0 ||
            (interfaceValue->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        const auto* address = reinterpret_cast<const sockaddr_in*>(interfaceValue->ifa_addr);
        const auto* netmask = reinterpret_cast<const sockaddr_in*>(interfaceValue->ifa_netmask);
        const uint32_t hostAddress = ntohl(address->sin_addr.s_addr);
        const uint32_t hostMask = ntohl(netmask->sin_addr.s_addr);
        uint8_t prefix = 0;
        uint32_t mask = hostMask;
        while ((mask & 0x80000000U) != 0) {
            ++prefix;
            mask <<= 1;
        }
        if (mask != 0) {
            continue;
        }
        addNetwork(networks, seen, hostAddress & hostMask, prefix);
    }
    freeifaddrs(interfaces);
}

void collectRouteNetworks(std::vector<std::string>& networks, std::set<std::string>& seen) {
    std::ifstream routes("/proc/net/route");
    std::string line;
    std::getline(routes, line);
    while (std::getline(routes, line)) {
        std::istringstream fields(line);
        std::string interfaceName;
        std::string destinationText;
        std::string gatewayText;
        std::string flagsText;
        if (!(fields >> interfaceName >> destinationText >> gatewayText >> flagsText)) {
            continue;
        }
        std::string refCount;
        std::string useCount;
        std::string metric;
        std::string maskText;
        if (!(fields >> refCount >> useCount >> metric >> maskText)) {
            continue;
        }
        try {
            const uint32_t destination = ntohl(static_cast<uint32_t>(
                std::stoul(destinationText, nullptr, 16)));
            const uint32_t mask = ntohl(static_cast<uint32_t>(
                std::stoul(maskText, nullptr, 16)));
            const unsigned long flags = std::stoul(flagsText, nullptr, 16);
            if ((flags & 0x1U) == 0 || destination == 0 || mask == 0) {
                continue;
            }
            uint32_t hostMask = mask;
            uint8_t prefix = 0;
            while ((hostMask & 0x80000000U) != 0) {
                ++prefix;
                hostMask <<= 1;
            }
            if (hostMask != 0 || prefix == 0) {
                continue;
            }
            addNetwork(networks, seen, destination & mask, prefix);
        } catch (...) {
            continue;
        }
    }
}

bool appendTargets(const std::string& network, std::vector<sockaddr_in>& targets,
                   std::vector<std::string>& normalizedNetworks,
                   std::set<std::string>& normalizedSeen,
                   std::unordered_set<uint32_t>& targetSeen) {
    CidrNetwork parsed;
    if (!parseCidr(network, parsed)) {
        return false;
    }
    const uint64_t addressCount = parsed.prefix == 32
        ? 1
        : parsed.prefix == 31 ? 2 : (1ULL << (32U - parsed.prefix)) - 2;
    if (addressCount == 0 || addressCount > MAX_DISCOVERY_ADDRESSES) {
        return false;
    }
    const std::string normalized = cidrText(parsed.address, parsed.prefix);
    if (!normalizedSeen.insert(normalized).second) {
        return true;
    }
    normalizedNetworks.push_back(normalized);
    const uint32_t first = parsed.prefix >= 31 ? parsed.address : parsed.address + 1;
    for (uint64_t offset = 0; offset < addressCount; ++offset) {
        const uint32_t hostAddress = first + static_cast<uint32_t>(offset);
        if (!targetSeen.insert(hostAddress).second) {
            continue;
        }
        if (targets.size() >= MAX_DISCOVERY_ADDRESSES) {
            return false;
        }
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(0);
        target.sin_addr.s_addr = htonl(hostAddress);
        targets.push_back(target);
    }
    return true;
}

uint64_t nextDiscoveryRequestId() {
    static std::atomic<uint64_t> value{1};
    return value.fetch_add(1);
}

} // namespace

NodeDiscovery::NodeDiscovery()
    : m_socket(-1), m_running(false), m_port(0), m_discoveryPort(0), m_scanning(false) {}
NodeDiscovery::~NodeDiscovery() { shutdown(); }

bool NodeDiscovery::initialize(const std::string& multicastAddress,
                               uint16_t port,
                               uint16_t discoveryPort,
                               const std::vector<std::string>& discoveryNetworks) {
    if (m_socket >= 0 || multicastAddress.empty() || port == 0 || discoveryPort == 0) {
        return false;
    }
    struct in_addr parsedAddress {};
    if (inet_pton(AF_INET, multicastAddress.c_str(), &parsedAddress) != 1 ||
        !IN_MULTICAST(ntohl(parsedAddress.s_addr))) {
        LOG_ERROR("Invalid heartbeat multicast address: %s", multicastAddress.c_str());
        return false;
    }

    m_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socket < 0) {
        LOG_ERROR("Failed to create node discovery socket");
        return false;
    }
    int reuse = 1;
    if (setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        LOG_ERROR("Failed to set SO_REUSEADDR on node discovery socket");
        close(m_socket);
        m_socket = -1;
        return false;
    }
    struct timeval receiveTimeout {};
    receiveTimeout.tv_usec = 200000;
    if (setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout)) < 0) {
        LOG_ERROR("Failed to set heartbeat receive timeout");
        close(m_socket);
        m_socket = -1;
        return false;
    }
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(m_socket, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0) {
        LOG_ERROR("Failed to bind node discovery socket to port %u", port);
        close(m_socket);
        m_socket = -1;
        return false;
    }
    struct ip_mreq membership {};
    membership.imr_multiaddr = parsedAddress;
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(m_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) < 0) {
        LOG_ERROR("Failed to join heartbeat multicast group");
        close(m_socket);
        m_socket = -1;
        return false;
    }
    m_multicastAddress = multicastAddress;
    m_port = port;
    m_discoveryPort = discoveryPort;
    m_discoveryNetworks = discoveryNetworks;
    LOG_INFO("NodeDiscovery initialized on %s:%u", multicastAddress.c_str(), port);
    return true;
}

void NodeDiscovery::shutdown() {
    m_running.store(false);
    if (m_listenThread.joinable()) m_listenThread.join();
    if (m_cleanupThread.joinable()) m_cleanupThread.join();
    if (m_socket >= 0) { close(m_socket); m_socket = -1; }
}

bool NodeDiscovery::start() {
    if (m_socket < 0 || m_running.exchange(true)) {
        return false;
    }
    try {
        m_listenThread = std::thread(&NodeDiscovery::listenThread, this);
        m_cleanupThread = std::thread(&NodeDiscovery::cleanupThread, this);
    } catch (const std::exception& error) {
        m_running.store(false);
        if (m_listenThread.joinable()) {
            m_listenThread.join();
        }
        LOG_ERROR("Failed to start node discovery threads: %s", error.what());
        return false;
    }
    return true;
}


void NodeDiscovery::listenThread() {
    char buffer[4096];
    while (m_running.load()) {
        sockaddr_in sender{};
        socklen_t senderLength = sizeof(sender);
        const ssize_t length = recvfrom(m_socket, buffer, sizeof(buffer), 0,
                                        reinterpret_cast<sockaddr*>(&sender), &senderLength);
        if (length > 0) processHeartbeat(std::string(buffer, static_cast<size_t>(length)));
        else if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && m_running.load())
            LOG_WARNING("Heartbeat receive failed: %s", std::strerror(errno));
    }
}

void NodeDiscovery::cleanupThread() {
    while (m_running.load()) {
        for (int i = 0; i < 10 && m_running.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const uint64_t now = Protocol::getCurrentTimestamp();
        std::lock_guard<std::mutex> lock(m_nodesMutex);
        for (auto iterator = m_nodes.begin(); iterator != m_nodes.end();) {
            if (now >= iterator->second.lastSeen && now - iterator->second.lastSeen > NODE_TIMEOUT_MS)
                iterator = m_nodes.erase(iterator);
            else ++iterator;
        }
    }
}

bool NodeDiscovery::processHeartbeat(const std::string& message,
                                     uint32_t* processedNodeId,
                                     const std::string& sourceIp) {
    try {
        const json value = json::parse(message);
        const bool hasIpMode = value.contains("ipMode");
        const bool hasAnyDeviceInfo = value.contains("deviceModel") ||
                                      value.contains("deviceName") ||
                                      value.contains("softwareVersion") ||
                                      value.contains("boardInfo") ||
                                      value.contains("macAddress") ||
                                      value.contains("subnetMask") ||
                                      value.contains("gateway") ||
                                      value.contains("deviceType");
        const bool hasCompleteDeviceInfo = value.contains("deviceModel") &&
                                           value.contains("deviceName") &&
                                           value.contains("softwareVersion") &&
                                           value.contains("boardInfo") &&
                                           value.contains("macAddress") &&
                                           value.contains("subnetMask") &&
                                           value.contains("gateway") &&
                                           value.contains("deviceType");
        const bool hasAnySourceInfo = value.contains("signalSourceType") ||
                                      value.contains("signalSourceConfigured") ||
                                      value.contains("signalSourceEndpoint") ||
                                      value.contains("signalSourceWidth") ||
                                      value.contains("signalSourceHeight") ||
                                      value.contains("signalSourceFramerateNumerator") ||
                                      value.contains("signalSourceFramerateDenominator") ||
                                      value.contains("signalSourcePixelFormat");
        const bool hasCompleteSourceInfo = value.contains("signalSourceType") &&
                                           value.contains("signalSourceConfigured") &&
                                           value.contains("signalSourceEndpoint") &&
                                           value.contains("signalSourceWidth") &&
                                           value.contains("signalSourceHeight") &&
                                           value.contains("signalSourceFramerateNumerator") &&
                                           value.contains("signalSourceFramerateDenominator") &&
                                           value.contains("signalSourcePixelFormat");
        const bool hasSignalSourceActive = value.contains("signalSourceActive");
        const std::initializer_list<const char*> sourceFields = {
            "signalSourceType", "signalSourceConfigured", "signalSourceEndpoint",
            "signalSourceWidth", "signalSourceHeight", "signalSourceFramerateNumerator",
            "signalSourceFramerateDenominator", "signalSourcePixelFormat",
            "signalSourceActive", "signalSourceTransport"};
        const bool hasValidBaseFields = hasIpMode
            ? hasExactFieldsWithOptional(value, {"type", "nodeId", "nodeRole", "ip", "ipMode", "status",
                     "playState", "resolution", "cpu", "memory", "audioOutputMode",
                     "masterClockSynchronized", "masterClockOffsetMs", "commandId",
                     "actualStartTimestamp", "lastError", "kvmEnabled", "kvmRole", "kvmPort",
                     "kvmSessionId", "kvmState", "kvmLastError", "timestamp"}, sourceFields)
            : hasExactFieldsWithOptional(value, {"type", "nodeId", "nodeRole", "ip", "status", "playState",
                     "resolution", "cpu", "memory", "audioOutputMode", "masterClockSynchronized",
                     "masterClockOffsetMs", "commandId", "actualStartTimestamp", "lastError",
                     "kvmEnabled", "kvmRole", "kvmPort", "kvmSessionId", "kvmState",
                     "kvmLastError", "timestamp"}, sourceFields);
        const bool hasValidExtendedFields = hasIpMode
            ? hasExactFieldsWithOptional(value, {"type", "nodeId", "nodeRole", "ip", "ipMode", "deviceModel",
                     "deviceName", "softwareVersion", "boardInfo", "macAddress", "subnetMask",
                     "gateway", "deviceType", "status", "playState", "resolution", "cpu",
                     "memory", "audioOutputMode", "masterClockSynchronized", "masterClockOffsetMs",
                     "commandId", "actualStartTimestamp", "lastError", "kvmEnabled", "kvmRole",
                     "kvmPort", "kvmSessionId", "kvmState", "kvmLastError", "timestamp"}, sourceFields)
            : hasExactFieldsWithOptional(value, {"type", "nodeId", "nodeRole", "ip", "deviceModel", "deviceName",
                     "softwareVersion", "boardInfo", "macAddress", "subnetMask", "gateway", "deviceType",
                     "status", "playState", "resolution", "cpu", "memory", "audioOutputMode",
                     "masterClockSynchronized", "masterClockOffsetMs", "commandId", "actualStartTimestamp",
                     "lastError", "kvmEnabled", "kvmRole", "kvmPort", "kvmSessionId", "kvmState",
                     "kvmLastError", "timestamp"}, sourceFields);
        if (hasAnyDeviceInfo != hasCompleteDeviceInfo ||
            (hasAnyDeviceInfo ? !hasValidExtendedFields : !hasValidBaseFields) ||
            (hasAnySourceInfo != hasCompleteSourceInfo) ||
            !value.contains("type") || !value.at("type").is_string() ||
            value.at("type").get<std::string>() != "heartbeat" ||
            !value.contains("nodeId") || !value.at("nodeId").is_number_unsigned() ||
            !value.contains("nodeRole") || !value.at("nodeRole").is_string() ||
            (hasIpMode && (!value.at("ipMode").is_string() ||
                (value.at("ipMode").get<std::string>() != "auto" &&
                 value.at("ipMode").get<std::string>() != "manual"))) ||
            !value.contains("ip") || !value.at("ip").is_string() ||
            !value.contains("status") || !value.at("status").is_string() ||
            !value.contains("playState") || !value.at("playState").is_string() ||
            !value.contains("resolution") || !value.at("resolution").is_string() ||
            !value.contains("cpu") || !value.at("cpu").is_number() ||
            !value.contains("memory") || !value.at("memory").is_number() ||
            !value.contains("audioOutputMode") || !value.at("audioOutputMode").is_string() ||
            !value.contains("commandId") || !value.at("commandId").is_number_unsigned() ||
            !value.contains("actualStartTimestamp") ||
                !value.at("actualStartTimestamp").is_number_unsigned() ||
            !value.contains("lastError") || !value.at("lastError").is_string() ||
            !value.contains("masterClockSynchronized") ||
                !value.at("masterClockSynchronized").is_boolean() ||
            !value.contains("masterClockOffsetMs") ||
                (!value.at("masterClockOffsetMs").is_number_integer() &&
                 !value.at("masterClockOffsetMs").is_number_unsigned()) ||
            !value.contains("kvmEnabled") || !value.at("kvmEnabled").is_boolean() ||
            !value.contains("kvmRole") || !value.at("kvmRole").is_string() ||
            !value.contains("kvmPort") || !value.at("kvmPort").is_number_unsigned() ||
            !value.contains("kvmSessionId") || !value.at("kvmSessionId").is_number_unsigned() ||
            !value.contains("kvmState") || !value.at("kvmState").is_string() ||
            !value.contains("kvmLastError") || !value.at("kvmLastError").is_string() ||
            (hasSignalSourceActive && !value.at("signalSourceActive").is_boolean()) ||
            !value.contains("timestamp") || !value.at("timestamp").is_number_unsigned()) {
            return false;
        }

        if (hasAnyDeviceInfo &&
            (!value.at("deviceModel").is_string() || !value.at("deviceName").is_string() ||
             !value.at("softwareVersion").is_string() || !value.at("boardInfo").is_string() ||
             !value.at("macAddress").is_string() || !value.at("subnetMask").is_string() ||
             !value.at("gateway").is_string() || !value.at("deviceType").is_string())) {
            return false;
        }

        const uint64_t rawNodeId = value.at("nodeId").get<uint64_t>();
        const uint64_t sourceTimestamp = value.at("timestamp").get<uint64_t>();
        const uint64_t rawCommandId = value.at("commandId").get<uint64_t>();
        const uint64_t actualStartTimestamp = value.at("actualStartTimestamp").get<uint64_t>();
        const uint64_t rawKvmPort = value.at("kvmPort").get<uint64_t>();
        const uint64_t rawKvmSessionId = value.at("kvmSessionId").get<uint64_t>();
        const json& offsetValue = value.at("masterClockOffsetMs");
        if (rawNodeId == 0 || rawNodeId > std::numeric_limits<uint32_t>::max() ||
            rawCommandId > std::numeric_limits<uint32_t>::max() || rawKvmPort > 65535 ||
            rawKvmSessionId > std::numeric_limits<uint32_t>::max() || sourceTimestamp == 0 ||
            (offsetValue.is_number_unsigned() &&
             offsetValue.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))) {
            return false;
        }

        DiscoveredNode node;
        const uint32_t advertisedNodeId = static_cast<uint32_t>(rawNodeId);
        node.nodeId = advertisedNodeId;
        node.advertisedNodeId = advertisedNodeId;
        if (!parseNodeRole(value.at("nodeRole").get<std::string>(), node.nodeRole)) return false;
        node.ipMode = hasIpMode ? value.at("ipMode").get<std::string>() : "auto";
        node.ip = sourceIp.empty() ? value.at("ip").get<std::string>() : sourceIp;
        if (hasAnyDeviceInfo) {
            node.deviceModel = value.at("deviceModel").get<std::string>();
            node.deviceName = value.at("deviceName").get<std::string>();
            node.softwareVersion = value.at("softwareVersion").get<std::string>();
            node.boardInfo = value.at("boardInfo").get<std::string>();
            node.macAddress = value.at("macAddress").get<std::string>();
            node.subnetMask = value.at("subnetMask").get<std::string>();
            node.gateway = value.at("gateway").get<std::string>();
            node.deviceType = value.at("deviceType").get<std::string>();
        }
        node.status = value.at("status").get<std::string>();
        node.playState = value.at("playState").get<std::string>();
        node.resolution = value.at("resolution").get<std::string>();
        node.cpu = value.at("cpu").get<double>();
        node.memory = value.at("memory").get<double>();
        node.audioOutputMode = value.at("audioOutputMode").get<std::string>();
        node.masterClockSynchronized = value.at("masterClockSynchronized").get<bool>();
        node.masterClockOffsetMs = offsetValue.is_number_unsigned()
            ? static_cast<int64_t>(offsetValue.get<uint64_t>())
            : offsetValue.get<int64_t>();
        node.commandId = static_cast<uint32_t>(rawCommandId);
        node.actualStartTimestamp = actualStartTimestamp;
        node.lastError = value.at("lastError").get<std::string>();
        node.kvmEnabled = value.at("kvmEnabled").get<bool>();
        node.kvmPort = static_cast<uint16_t>(rawKvmPort);
        node.kvmSessionId = static_cast<uint32_t>(rawKvmSessionId);
        node.kvmState = value.at("kvmState").get<std::string>();
        node.kvmLastError = value.at("kvmLastError").get<std::string>();
        if (!parseKvmRole(value.at("kvmRole").get<std::string>(), node.kvmRole)) return false;
        AudioOutputMode parsedAudioOutputMode;
        in_addr parsedNodeIp{};
        if (node.ip.empty() || node.status.empty() || node.playState.empty() || node.resolution.empty() ||
            inet_pton(AF_INET, node.ip.c_str(), &parsedNodeIp) != 1 ||
            !parseAudioOutputMode(node.audioOutputMode, parsedAudioOutputMode) ||
            !std::isfinite(node.cpu) || !std::isfinite(node.memory) ||
            node.cpu < 0.0 || node.cpu > 100.0 || node.memory < 0.0 || node.memory > 100.0 ||
            node.kvmState.empty() || (node.kvmEnabled &&
                (node.kvmRole == KvmRole::DISABLED || node.kvmPort == 0)) ||
             (!node.kvmEnabled && node.kvmRole != KvmRole::DISABLED)) {
            return false;
        }
        if (hasCompleteSourceInfo) {
            if (!value.at("signalSourceType").is_string() ||
                !value.at("signalSourceConfigured").is_boolean() ||
                !value.at("signalSourceEndpoint").is_string() ||
                !value.at("signalSourceWidth").is_number_unsigned() ||
                !value.at("signalSourceHeight").is_number_unsigned() ||
                !value.at("signalSourceFramerateNumerator").is_number_unsigned() ||
                !value.at("signalSourceFramerateDenominator").is_number_unsigned() ||
                !value.at("signalSourcePixelFormat").is_string()) {
                return false;
            }
            SignalSourceConfig source;
            if (!parseSignalSourceType(value.at("signalSourceType").get<std::string>(), source.type)) {
                return false;
            }
            source.endpoint = value.at("signalSourceEndpoint").get<std::string>();
            try {
                source.width = value.at("signalSourceWidth").get<uint32_t>();
                source.height = value.at("signalSourceHeight").get<uint32_t>();
                source.framerateNumerator = value.at("signalSourceFramerateNumerator").get<uint32_t>();
                source.framerateDenominator = value.at("signalSourceFramerateDenominator").get<uint32_t>();
            } catch (const std::exception&) {
                return false;
            }
            source.pixelFormat = value.at("signalSourcePixelFormat").get<std::string>();
            if (value.at("signalSourceConfigured").get<bool>() !=
                    (source.type != SignalSourceType::NONE) ||
                !Protocol::validateSignalSourceConfig(source)) {
                return false;
            }
            node.signalSourceType = value.at("signalSourceType").get<std::string>();
            node.signalSourceConfigured = source.type != SignalSourceType::NONE;
            node.signalSourceEndpoint = source.endpoint;
            node.signalSourceWidth = source.width;
            node.signalSourceHeight = source.height;
            node.signalSourceFramerateNumerator = source.framerateNumerator;
            node.signalSourceFramerateDenominator = source.framerateDenominator;
            node.signalSourcePixelFormat = source.pixelFormat;
        }
        node.signalSourceActive = hasSignalSourceActive &&
                                  value.at("signalSourceActive").get<bool>();
        if (value.contains("signalSourceTransport")) {
            if (!value.at("signalSourceTransport").is_string()) return false;
            node.signalSourceTransport = value.at("signalSourceTransport").get<std::string>();
            if (node.signalSourceTransport != "mjpeg" &&
                node.signalSourceTransport != "rtp-h264") {
                return false;
            }
        }
        if (node.signalSourceActive && !node.signalSourceConfigured) {
            // Older nodes used MediaPlayer::State::Capture for both a local
            // input pipeline and an output-only hardware window pipeline.
            // Do not make the whole output node disappear because that
            // legacy heartbeat mislabeled window playback as an active input.
            LOG_WARNING("Node %u (%s) advertised an active source without a configured "
                        "source; treating the source as inactive",
                        node.advertisedNodeId, node.ip.c_str());
            node.signalSourceActive = false;
        }
        node.lastSeen = Protocol::getCurrentTimestamp();
        std::lock_guard<std::mutex> lock(m_nodesMutex);

        const auto sameDevice = [](const DiscoveredNode& left, const DiscoveredNode& right) {
            if (!left.macAddress.empty() && !right.macAddress.empty()) {
                return left.macAddress == right.macAddress;
            }
            return !left.ip.empty() && left.ip == right.ip;
        };
        uint32_t previousNodeId = 0;
        std::vector<DiscoveredNode> candidates;
        candidates.reserve(m_nodes.size() + 1);
        for (const auto& entry : m_nodes) {
            if (sameDevice(entry.second, node)) {
                previousNodeId = entry.second.nodeId;
            } else {
                candidates.push_back(entry.second);
            }
        }

        std::set<uint32_t> assignedIds;
        for (const auto& candidate : candidates) {
            if (candidate.nodeId != 0) assignedIds.insert(candidate.nodeId);
        }
        uint32_t assignedNodeId = 0;
        if (previousNodeId != 0 && assignedIds.find(previousNodeId) == assignedIds.end()) {
            assignedNodeId = previousNodeId;
        } else if (node.advertisedNodeId != 0 &&
                   node.advertisedNodeId <= MAX_NODES &&
                   assignedIds.find(node.advertisedNodeId) == assignedIds.end()) {
            assignedNodeId = node.advertisedNodeId;
        } else {
            assignedNodeId = 1;
            while (assignedNodeId < MAX_NODES &&
                   assignedIds.find(assignedNodeId) != assignedIds.end()) {
                ++assignedNodeId;
            }
            if (assignedIds.find(assignedNodeId) != assignedIds.end()) {
                LOG_ERROR("Unable to assign an automatic node ID for endpoint %s",
                          node.ip.c_str());
                return false;
            }
        }
        node.nodeId = assignedNodeId;
        candidates.push_back(std::move(node));
        m_nodes.clear();
        for (auto& candidate : candidates) {
            m_nodes[candidate.nodeId] = std::move(candidate);
        }
        const auto assigned = m_nodes.find(assignedNodeId);
        if (assigned != m_nodes.end() &&
            assigned->second.advertisedNodeId != assigned->second.nodeId) {
            LOG_WARNING("Duplicate node ID %u from endpoint %s; assigned node ID %u",
                        assigned->second.advertisedNodeId, assigned->second.ip.c_str(),
                        assigned->second.nodeId);
        }
        if (processedNodeId) *processedNodeId = assignedNodeId;
        return true;
    } catch (const std::exception& error) {
        LOG_WARNING("Ignoring invalid heartbeat: %s", error.what());
        return false;
    }
}

std::vector<DiscoveredNode> NodeDiscovery::getNodes() const {
    std::lock_guard<std::mutex> lock(m_nodesMutex);
    std::vector<DiscoveredNode> nodes;
    nodes.reserve(m_nodes.size());
    for (const auto& entry : m_nodes) nodes.push_back(entry.second);
    return nodes;
}

DiscoveryScanResult NodeDiscovery::discover(uint32_t timeoutMs) {
    DiscoveryScanResult result;
    if (m_socket < 0 || !m_running.load()) {
        result.error = "Node discovery is not running";
        return result;
    }
    if (timeoutMs == 0 || timeoutMs > 10000) {
        result.error = "Discovery timeout is out of range";
        return result;
    }
    if (m_scanning.exchange(true)) {
        result.busy = true;
        result.error = "A device discovery scan is already running";
        return result;
    }
    struct ScanGuard {
        std::atomic<bool>& scanning;
        ~ScanGuard() { scanning.store(false); }
    } scanGuard{m_scanning};

    const auto started = std::chrono::steady_clock::now();
    auto finish = [&]() {
        result.elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
        result.totalNodes = static_cast<uint32_t>(getNodes().size());
        return result;
    };

    std::vector<std::string> candidateNetworks = m_discoveryNetworks;
    std::set<std::string> candidateSeen(candidateNetworks.begin(), candidateNetworks.end());
    if (candidateNetworks.empty()) {
        collectInterfaceNetworks(candidateNetworks, candidateSeen);
        collectRouteNetworks(candidateNetworks, candidateSeen);
    }
    if (candidateNetworks.empty()) {
        result.error = "No routable IPv4 networks were found";
        return finish();
    }

    std::vector<sockaddr_in> targets;
    std::set<std::string> normalizedSeen;
    std::unordered_set<uint32_t> targetSeen;
    for (const auto& network : candidateNetworks) {
        if (!appendTargets(network, targets, result.networks, normalizedSeen, targetSeen)) {
            result.error = "Invalid or oversized discovery network: " + network;
            return finish();
        }
    }
    if (targets.empty()) {
        result.error = "No usable IPv4 addresses were found in discovery networks";
        return finish();
    }

    const int scanSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (scanSocket < 0) {
        result.error = std::string("Failed to create discovery socket: ") + std::strerror(errno);
        return finish();
    }
    const std::string request = createDiscoveryRequest(nextDiscoveryRequestId());
    int broadcast = 1;
    setsockopt(scanSocket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    for (const auto& network : result.networks) {
        CidrNetwork parsed;
        if (!parseCidr(network, parsed) || parsed.prefix >= 31) continue;
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(m_discoveryPort);
        const uint32_t broadcastAddress = parsed.address | (0xffffffffU >> parsed.prefix);
        target.sin_addr.s_addr = htonl(broadcastAddress);
        sendto(scanSocket, request.data(), request.size(), MSG_DONTWAIT,
               reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }
    for (auto target : targets) {
        target.sin_port = htons(m_discoveryPort);
        ++result.addressesProbed;
        const ssize_t sent = sendto(scanSocket, request.data(), request.size(), MSG_DONTWAIT,
                                    reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        (void)sent;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::set<uint32_t> respondingNodeIds;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        timeval wait{};
        wait.tv_sec = static_cast<long>(remaining.count() / 1000);
        wait.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(scanSocket, &readable);
        const int ready = select(scanSocket + 1, &readable, nullptr, nullptr, &wait);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) break;
        char buffer[4096];
        sockaddr_in sender{};
        socklen_t senderLength = sizeof(sender);
        const ssize_t length = recvfrom(scanSocket, buffer, sizeof(buffer), 0,
                                        reinterpret_cast<sockaddr*>(&sender), &senderLength);
        if (length > 0) {
            char senderIp[INET_ADDRSTRLEN] = {};
            uint32_t nodeId = 0;
            if (inet_ntop(AF_INET, &sender.sin_addr, senderIp, sizeof(senderIp)) &&
                processHeartbeat(std::string(buffer, static_cast<size_t>(length)),
                                 &nodeId, senderIp)) {
                respondingNodeIds.insert(nodeId);
            }
        }
    }
    close(scanSocket);

    result.nodesFound = static_cast<uint32_t>(respondingNodeIds.size());
    result.success = true;
    return finish();
}

} // namespace dms
