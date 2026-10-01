#include "common/DiscoveryProtocol.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace dms {

std::string createDiscoveryRequest(uint64_t requestId) {
    if (requestId == 0) {
        return {};
    }
    return json{
        {"type", "discovery_request"},
        {"version", DISCOVERY_PROTOCOL_VERSION},
        {"requestId", requestId}
    }.dump();
}

bool parseDiscoveryRequest(const std::string& message, uint64_t& requestId) {
    requestId = 0;
    try {
        const json value = json::parse(message);
        if (!value.is_object() || value.size() != 3 ||
            !value.contains("type") || !value.at("type").is_string() ||
            value.at("type").get<std::string>() != "discovery_request" ||
            !value.contains("version") || !value.at("version").is_number_unsigned() ||
            value.at("version").get<uint64_t>() != DISCOVERY_PROTOCOL_VERSION ||
            !value.contains("requestId") || !value.at("requestId").is_number_unsigned()) {
            return false;
        }
        requestId = value.at("requestId").get<uint64_t>();
        return requestId != 0;
    } catch (...) {
        return false;
    }
}

} // namespace dms
