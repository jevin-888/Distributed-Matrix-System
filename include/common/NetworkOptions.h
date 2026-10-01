#pragma once

#include "common/Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dms {

enum class IpAssignmentMode {
    AUTO,
    MANUAL
};

inline const char* ipAssignmentModeName(IpAssignmentMode mode) {
    return mode == IpAssignmentMode::MANUAL ? "manual" : "auto";
}

inline bool parseIpAssignmentMode(const std::string& value, IpAssignmentMode& mode) {
    if (value == "auto") {
        mode = IpAssignmentMode::AUTO;
        return true;
    }
    if (value == "manual") {
        mode = IpAssignmentMode::MANUAL;
        return true;
    }
    return false;
}

struct NetworkOptions {
    IpAssignmentMode ipMode = IpAssignmentMode::AUTO;
    std::string localIp;
    std::string commandMulticastAddress = MULTICAST_GROUP;
    uint16_t commandPort = MULTICAST_PORT;
    std::string heartbeatMulticastAddress = "239.1.1.2";
    uint16_t heartbeatPort = 9002;
    uint16_t discoveryPort = 9003;
    std::vector<std::string> discoveryNetworks;
};

} // namespace dms
