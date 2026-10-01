#pragma once

#include <cstdint>
#include <string>

namespace dms {

constexpr uint32_t DISCOVERY_PROTOCOL_VERSION = 1;

std::string createDiscoveryRequest(uint64_t requestId);
bool parseDiscoveryRequest(const std::string& message, uint64_t& requestId);

} // namespace dms
