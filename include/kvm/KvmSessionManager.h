#pragma once

#include "kvm/KvmTypes.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace dms {

class KvmSessionManager {
public:
    bool acquire(uint32_t controllerNodeId,
                 uint32_t targetNodeId,
                 const std::string& targetIp,
                 uint16_t targetPort,
                 uint64_t leaseMs,
                 KvmSession& session);
    bool release(uint32_t sessionId, KvmSession& releasedSession);
    KvmSession getSession() const;
    bool expire(uint64_t now, KvmSession& expiredSession);

private:
    uint32_t nextSessionId();
    static uint64_t generateToken();

    mutable std::mutex m_mutex;
    std::atomic<uint32_t> m_nextSessionId{0};
    KvmSession m_session;
};

} // namespace dms
