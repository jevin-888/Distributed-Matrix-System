#include "common/SyncTimestampGenerator.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

namespace dms {
namespace {

uint64_t systemTimeMilliseconds() {
    using namespace std::chrono;
    const auto value = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    return value < 0 ? 0 : static_cast<uint64_t>(value);
}

} // namespace

SyncTimestampGenerator::SyncTimestampGenerator()
    : m_timeOffset(0), m_synchronized(false) {
}

uint64_t SyncTimestampGenerator::generateSyncTimestamp(uint64_t delayMs) const {
    const uint64_t now = getCurrentTimestamp();
    if (delayMs > std::numeric_limits<uint64_t>::max() - now) {
        return std::numeric_limits<uint64_t>::max();
    }
    return now + delayMs;
}

uint64_t SyncTimestampGenerator::getCurrentTimestamp() const {
    const uint64_t local = systemTimeMilliseconds();
    const int64_t offset = m_timeOffset.load();
    if (offset >= 0) {
        const uint64_t positiveOffset = static_cast<uint64_t>(offset);
        return positiveOffset > std::numeric_limits<uint64_t>::max() - local
            ? std::numeric_limits<uint64_t>::max()
            : local + positiveOffset;
    }

    const uint64_t negativeOffset = static_cast<uint64_t>(-(offset + 1)) + 1ULL;
    return negativeOffset > local ? 0 : local - negativeOffset;
}

int64_t SyncTimestampGenerator::getTimeUntilTimestamp(uint64_t targetTimestamp) const {
    const uint64_t now = getCurrentTimestamp();
    if (targetTimestamp <= now) {
        return 0;
    }
    const uint64_t remaining = targetTimestamp - now;
    return remaining > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
        ? std::numeric_limits<int64_t>::max()
        : static_cast<int64_t>(remaining);
}

int64_t SyncTimestampGenerator::waitUntilTimestamp(uint64_t targetTimestamp,
                                                    uint32_t toleranceMs) const {
    const int64_t remaining = getTimeUntilTimestamp(targetTimestamp);
    if (remaining <= 0) {
        return 0;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(remaining);
    const auto spinWindow = std::chrono::milliseconds(std::min<uint32_t>(toleranceMs, 10U));

    if (deadline - started > spinWindow) {
        std::this_thread::sleep_until(deadline - spinWindow);
    }
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
}

bool SyncTimestampGenerator::synchronizeTo(uint64_t referenceTimestamp,
                                            uint64_t localReceiveTimestamp) {
    constexpr uint64_t MAX_SIGNED_TIMESTAMP =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (referenceTimestamp == 0 || localReceiveTimestamp == 0 ||
        referenceTimestamp > MAX_SIGNED_TIMESTAMP || localReceiveTimestamp > MAX_SIGNED_TIMESTAMP) {
        return false;
    }

    const int64_t offset = static_cast<int64_t>(referenceTimestamp) -
                           static_cast<int64_t>(localReceiveTimestamp);
    m_timeOffset.store(offset);
    m_synchronized.store(true);
    return true;
}

void SyncTimestampGenerator::setTimeOffset(int64_t offsetMs) {
    m_timeOffset.store(offsetMs);
    m_synchronized.store(true);
}

void SyncTimestampGenerator::reset() {
    m_timeOffset.store(0);
    m_synchronized.store(false);
}

} // namespace dms
