#include "kvm/KvmSessionManager.h"
#include "common/Protocol.h"

#include <limits>
#include <random>

namespace dms {

uint32_t KvmSessionManager::nextSessionId() {
    uint32_t id = m_nextSessionId.fetch_add(1) + 1;
    if (id == 0) {
        id = m_nextSessionId.fetch_add(1) + 1;
    }
    return id;
}

uint64_t KvmSessionManager::generateToken() {
    std::random_device random;
    const uint64_t high = static_cast<uint64_t>(random()) << 32;
    const uint64_t low = static_cast<uint64_t>(random());
    const uint64_t token = high | low;
    return token == 0 ? 1 : token;
}

bool KvmSessionManager::acquire(uint32_t controllerNodeId,
                                uint32_t targetNodeId,
                                const std::string& targetIp,
                                uint16_t targetPort,
                                uint64_t leaseMs,
                                KvmSession& session) {
    if (targetNodeId == 0 || controllerNodeId == targetNodeId || targetIp.empty() ||
        targetPort == 0 || leaseMs < 1000 || leaseMs > 3600000) {
        return false;
    }

    const uint64_t now = Protocol::getCurrentTimestamp();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_session.sessionId != 0 && now < m_session.expiresAt) {
        return false;
    }
    if (leaseMs > std::numeric_limits<uint64_t>::max() - now) {
        return false;
    }

    m_session = {};
    m_session.sessionId = nextSessionId();
    m_session.controllerNodeId = controllerNodeId;
    m_session.targetNodeId = targetNodeId;
    m_session.targetIp = targetIp;
    m_session.targetPort = targetPort;
    m_session.sessionToken = generateToken();
    m_session.acquiredAt = now;
    m_session.expiresAt = now + leaseMs;
    m_session.state = "acquired";
    session = m_session;
    return true;
}

bool KvmSessionManager::release(uint32_t sessionId, KvmSession& releasedSession) {
    if (sessionId == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_session.sessionId != sessionId) {
        return false;
    }
    releasedSession = m_session;
    m_session = {};
    return true;
}

KvmSession KvmSessionManager::getSession() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_session;
}

bool KvmSessionManager::expire(uint64_t now, KvmSession& expiredSession) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_session.sessionId == 0 || now < m_session.expiresAt) {
        return false;
    }
    expiredSession = m_session;
    m_session = {};
    return true;
}

} // namespace dms
