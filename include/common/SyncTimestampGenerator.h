#pragma once

#include <atomic>
#include <cstdint>

namespace dms {

class SyncTimestampGenerator {
public:
    SyncTimestampGenerator();
    ~SyncTimestampGenerator() = default;

    uint64_t generateSyncTimestamp(uint64_t delayMs) const;
    uint64_t getCurrentTimestamp() const;
    int64_t getTimeUntilTimestamp(uint64_t targetTimestamp) const;
    int64_t waitUntilTimestamp(uint64_t targetTimestamp, uint32_t toleranceMs) const;

    bool synchronizeTo(uint64_t referenceTimestamp, uint64_t localReceiveTimestamp);
    void setTimeOffset(int64_t offsetMs);
    void reset();

    int64_t getTimeOffset() const { return m_timeOffset.load(); }
    bool isSynchronized() const { return m_synchronized.load(); }

private:
    std::atomic<int64_t> m_timeOffset;
    std::atomic<bool> m_synchronized;
};

} // namespace dms
