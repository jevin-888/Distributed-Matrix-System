#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace dms {

enum class KvmRole {
    DISABLED,
    CONTROLLER,
    TARGET,
    BOTH,
};

inline const char* kvmRoleName(KvmRole role) {
    switch (role) {
        case KvmRole::DISABLED: return "disabled";
        case KvmRole::CONTROLLER: return "controller";
        case KvmRole::TARGET: return "target";
        case KvmRole::BOTH: return "both";
    }
    return "disabled";
}

inline bool parseKvmRole(const std::string& value, KvmRole& role) {
    if (value == "disabled") role = KvmRole::DISABLED;
    else if (value == "controller") role = KvmRole::CONTROLLER;
    else if (value == "target") role = KvmRole::TARGET;
    else if (value == "both") role = KvmRole::BOTH;
    else return false;
    return true;
}

inline bool kvmCanControl(KvmRole role) {
    return role == KvmRole::CONTROLLER || role == KvmRole::BOTH;
}

inline bool kvmCanTarget(KvmRole role) {
    return role == KvmRole::TARGET || role == KvmRole::BOTH;
}

inline std::string formatNodeId(uint32_t nodeId) {
    std::ostringstream output;
    output << std::setw(3) << std::setfill('0') << nodeId;
    return output.str();
}

struct KvmOptions {
    bool enabled = false;
    KvmRole role = KvmRole::DISABLED;
    uint16_t listenPort = 9101;
    std::vector<std::string> inputDevices;
    std::string keyboardHidDevice = "/dev/hidg0";
    std::string mouseHidDevice = "/dev/hidg1";
};

struct KvmAgentStatus {
    bool enabled = false;
    KvmRole role = KvmRole::DISABLED;
    uint16_t port = 0;
    uint32_t sessionId = 0;
    std::string state = "disabled";
    std::string lastError;
};

struct KvmSession {
    uint32_t sessionId = 0;
    uint32_t controllerNodeId = 0;
    uint32_t targetNodeId = 0;
    std::string targetIp;
    uint16_t targetPort = 0;
    uint64_t sessionToken = 0;
    uint64_t acquiredAt = 0;
    uint64_t expiresAt = 0;
    std::string state = "idle";
};

} // namespace dms
