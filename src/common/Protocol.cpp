#include "common/Protocol.h"
#include "common/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

using json = nlohmann::json;

namespace dms {

bool hasControlCharacters(const std::string& value);

namespace {

constexpr size_t WIRE_HEADER_SIZE = 40;

void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendU64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

uint32_t readU32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

uint64_t readU64(const uint8_t* data) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | data[i];
    }
    return value;
}

bool isKnownCommand(uint32_t rawType) {
    switch (static_cast<CommandType>(rawType)) {
        case CommandType::SYNC_PLAY:
        case CommandType::STOP:
        case CommandType::PAUSE:
        case CommandType::RESUME:
        case CommandType::PREPARE_PLAY:
        case CommandType::COMMIT_PLAY:
        case CommandType::SET_AUDIO_OUTPUT:
        case CommandType::SET_AUDIO_VOLUME:
        case CommandType::KVM_ROUTE:
        case CommandType::KVM_RELEASE:
        case CommandType::SET_NODE_ROLE:
        case CommandType::SET_NODE_NETWORK:
        case CommandType::SET_DISPLAY_LAYOUT:
        case CommandType::SET_NODE_SOURCE:
        case CommandType::SET_NODE_ID:
        case CommandType::SET_WINDOW_LAYOUT:
            return true;
    }
    return false;
}

void appendHeader(std::vector<uint8_t>& out,
                  const Protocol::PacketHeader& header,
                  uint32_t checksum) {
    appendU32(out, header.magic);
    appendU32(out, header.version);
    appendU32(out, header.commandType);
    appendU32(out, header.sequenceId);
    appendU32(out, header.payloadLength);
    appendU32(out, checksum);
    appendU64(out, header.timestamp);
    appendU32(out, header.reserved);
    appendU32(out, header.reserved2);
}

uint32_t calculatePacketChecksum(const Protocol::Packet& packet) {
    std::vector<uint8_t> bytes;
    bytes.reserve(WIRE_HEADER_SIZE + packet.payload.size());
    appendHeader(bytes, packet.header, 0);
    bytes.insert(bytes.end(), packet.payload.begin(), packet.payload.end());
    return Protocol::calculateCRC32(bytes.data(), bytes.size());
}

Protocol::Packet makeCommandPacket(uint32_t sequenceId,
                                   CommandType type,
                                   const json& payload) {
    if (sequenceId == 0) {
        throw std::invalid_argument("sequenceId must not be zero");
    }
    Protocol::Packet packet{};
    const std::string text = payload.dump();
    if (WIRE_HEADER_SIZE + text.size() > MAX_PACKET_SIZE) {
        throw std::invalid_argument("Command payload exceeds MAX_PACKET_SIZE");
    }
    packet.payload.assign(text.begin(), text.end());
    packet.header.magic = PROTOCOL_MAGIC;
    packet.header.version = PROTOCOL_VERSION;
    packet.header.commandType = static_cast<uint32_t>(type);
    packet.header.sequenceId = sequenceId;
    packet.header.payloadLength = static_cast<uint32_t>(packet.payload.size());
    packet.header.timestamp = Protocol::getCurrentTimestamp();
    packet.header.reserved = 0;
    packet.header.reserved2 = 0;
    packet.header.checksum = calculatePacketChecksum(packet);
    return packet;
}

uint32_t requiredU32(const json& object, const char* key, bool allowZero = true) {
    if (!object.contains(key) || !object.at(key).is_number_unsigned()) {
        throw std::invalid_argument(std::string("Invalid unsigned field: ") + key);
    }
    const uint64_t value = object.at(key).get<uint64_t>();
    if (value > std::numeric_limits<uint32_t>::max() || (!allowZero && value == 0)) {
        throw std::invalid_argument(std::string("Unsigned field out of range: ") + key);
    }
    return static_cast<uint32_t>(value);
}

bool hasExactFields(const json& object, std::initializer_list<const char*> fields) {
    if (!object.is_object() || object.size() != fields.size()) {
        return false;
    }
    for (const char* field : fields) {
        if (!object.contains(field)) {
            return false;
        }
    }
    return true;
}

bool parseCropRegions(const json& cropsJson,
                      uint32_t videoWidth,
                      uint32_t videoHeight,
                      std::vector<Protocol::CropRegion>& crops) {
    if (!cropsJson.is_array() || cropsJson.empty() || cropsJson.size() > MAX_NODES) {
        return false;
    }
    std::set<uint32_t> nodeIds;
    for (const auto& cropJson : cropsJson) {
        if (!hasExactFields(cropJson, {"node_id", "x", "y", "width", "height"})) {
            return false;
        }
        Protocol::CropRegion crop{};
        crop.nodeId = requiredU32(cropJson, "node_id", false);
        crop.cropX = requiredU32(cropJson, "x");
        crop.cropY = requiredU32(cropJson, "y");
        crop.cropWidth = requiredU32(cropJson, "width", false);
        crop.cropHeight = requiredU32(cropJson, "height", false);
        if (crop.cropX > std::numeric_limits<uint32_t>::max() - crop.cropWidth ||
            crop.cropY > std::numeric_limits<uint32_t>::max() - crop.cropHeight ||
            crop.cropX + crop.cropWidth > videoWidth ||
            crop.cropY + crop.cropHeight > videoHeight ||
            !nodeIds.insert(crop.nodeId).second) {
            return false;
        }
        crops.push_back(crop);
    }
    return true;
}

Protocol::Packet createPlayPacket(uint32_t sequenceId,
                                  CommandType type,
                                  const std::string& videoUrl,
                                  uint32_t videoWidth,
                                  uint32_t videoHeight,
                                  uint64_t syncTimestamp,
                                  const std::vector<Protocol::CropRegion>& crops) {
    if (sequenceId == 0) {
        throw std::invalid_argument("sequenceId must not be zero");
    }
    if (videoUrl.empty()) {
        throw std::invalid_argument("videoUrl must not be empty");
    }
    if (videoWidth == 0 || videoHeight == 0 || videoWidth > 65535 || videoHeight > 65535) {
        throw std::invalid_argument("video dimensions are invalid");
    }
    if (syncTimestamp == 0) {
        throw std::invalid_argument("syncTimestamp must not be zero");
    }
    if (crops.empty() || crops.size() > MAX_NODES) {
        throw std::invalid_argument("crops count is invalid");
    }

    const char* command = type == CommandType::PREPARE_PLAY ? "prepare_play" : "sync_play";
    json payload = {{"cmd", command}, {"video_url", videoUrl},
                    {"video_width", videoWidth}, {"video_height", videoHeight},
                    {"sync_timestamp", syncTimestamp}, {"crops", json::array()}};
    std::set<uint32_t> nodeIds;
    for (const auto& crop : crops) {
        if (crop.nodeId == 0 || crop.cropWidth == 0 || crop.cropHeight == 0 ||
            crop.cropX > std::numeric_limits<uint32_t>::max() - crop.cropWidth ||
            crop.cropY > std::numeric_limits<uint32_t>::max() - crop.cropHeight ||
            crop.cropX + crop.cropWidth > videoWidth ||
            crop.cropY + crop.cropHeight > videoHeight ||
            !nodeIds.insert(crop.nodeId).second) {
            throw std::invalid_argument("Invalid crop region");
        }
        payload["crops"].push_back({{"node_id", crop.nodeId}, {"x", crop.cropX},
                                    {"y", crop.cropY}, {"width", crop.cropWidth},
                                    {"height", crop.cropHeight}});
    }
    return makeCommandPacket(sequenceId, type, payload);
}

Protocol::Packet createDisplayLayoutPacket(
    uint32_t sequenceId,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    const std::vector<Protocol::CropRegion>& crops) {
    if (sourceWidth == 0 || sourceHeight == 0 || sourceWidth > 65535 || sourceHeight > 65535) {
        throw std::invalid_argument("display layout source dimensions are invalid");
    }
    if (crops.size() > MAX_NODES) {
        throw std::invalid_argument("display layout crop count is invalid");
    }

    json payload = { {"cmd", "set_display_layout"},
                     {"source_width", sourceWidth},
                     {"source_height", sourceHeight},
                     {"crops", json::array()} };
    std::set<uint32_t> nodeIds;
    for (const auto& crop : crops) {
        if (crop.nodeId == 0 || crop.cropWidth == 0 || crop.cropHeight == 0 ||
            crop.cropX > std::numeric_limits<uint32_t>::max() - crop.cropWidth ||
            crop.cropY > std::numeric_limits<uint32_t>::max() - crop.cropHeight ||
            crop.cropX + crop.cropWidth > sourceWidth ||
            crop.cropY + crop.cropHeight > sourceHeight ||
            !nodeIds.insert(crop.nodeId).second) {
            throw std::invalid_argument("Invalid display layout crop region");
        }
        payload["crops"].push_back({{"node_id", crop.nodeId}, {"x", crop.cropX},
                                     {"y", crop.cropY}, {"width", crop.cropWidth},
                                     {"height", crop.cropHeight}});
    }
    return makeCommandPacket(sequenceId, CommandType::SET_DISPLAY_LAYOUT, payload);
}

Protocol::Packet createNodeSourcePacket(
    uint32_t sequenceId,
    const Protocol::SetNodeSourceCommand& command) {
    std::string error;
    if (command.nodeId == 0 || !Protocol::validateSignalSourceConfig(command.source, &error)) {
        throw std::invalid_argument(error.empty() ? "Invalid node signal source command" : error);
    }
    return makeCommandPacket(
        sequenceId, CommandType::SET_NODE_SOURCE,
        {{"cmd", "set_node_source"},
         {"node_id", command.nodeId},
         {"source_type", signalSourceTypeName(command.source.type)},
         {"endpoint", command.source.endpoint},
         {"width", command.source.width},
         {"height", command.source.height},
         {"framerate_numerator", command.source.framerateNumerator},
         {"framerate_denominator", command.source.framerateDenominator},
         {"pixel_format", command.source.pixelFormat}});
}

Protocol::Packet createNodeIdPacket(
    uint32_t sequenceId,
    const Protocol::SetNodeIdCommand& command) {
    if (command.currentNodeId == 0 || command.newNodeId == 0 ||
        command.newNodeId > MAX_NODES || command.targetMacAddress.empty() ||
        command.targetMacAddress.size() > 32 ||
        hasControlCharacters(command.targetMacAddress)) {
        throw std::invalid_argument("Invalid node ID assignment command");
    }
    return makeCommandPacket(
        sequenceId, CommandType::SET_NODE_ID,
        {{"cmd", "set_node_id"},
         {"current_node_id", command.currentNodeId},
         {"new_node_id", command.newNodeId},
         {"target_mac_address", command.targetMacAddress}});
}

bool parseSourceConfig(const json& payload, SignalSourceConfig& source) {
    if (!hasExactFields(payload, {"source_type", "endpoint", "width", "height",
                                  "framerate_numerator", "framerate_denominator",
                                  "pixel_format"}) ||
        !payload.at("source_type").is_string() || !payload.at("endpoint").is_string() ||
        !payload.at("pixel_format").is_string()) {
        return false;
    }
    SignalSourceType type;
    if (!parseSignalSourceType(payload.at("source_type").get<std::string>(), type)) {
        return false;
    }
    SignalSourceConfig parsed;
    parsed.type = type;
    parsed.endpoint = payload.at("endpoint").get<std::string>();
    parsed.width = requiredU32(payload, "width");
    parsed.height = requiredU32(payload, "height");
    parsed.framerateNumerator = requiredU32(payload, "framerate_numerator");
    parsed.framerateDenominator = requiredU32(payload, "framerate_denominator", false);
    parsed.pixelFormat = payload.at("pixel_format").get<std::string>();
    if (!Protocol::validateSignalSourceConfig(parsed)) return false;
    source = std::move(parsed);
    return true;
}

bool parsePlayPayload(const json& payload,
                      const char* command,
                      Protocol::SyncPlayCommand& cmd) {
    if (!hasExactFields(payload, {"cmd", "video_url", "video_width", "video_height",
                                  "sync_timestamp", "crops"}) ||
        payload.at("cmd").get<std::string>() != command ||
        !payload.at("video_url").is_string() ||
        !payload.at("video_width").is_number_unsigned() ||
        !payload.at("video_height").is_number_unsigned() ||
        !payload.at("sync_timestamp").is_number_unsigned()) {
        return false;
    }
    cmd = Protocol::SyncPlayCommand{};
    cmd.videoUrl = payload.at("video_url").get<std::string>();
    cmd.videoWidth = requiredU32(payload, "video_width", false);
    cmd.videoHeight = requiredU32(payload, "video_height", false);
    cmd.syncTimestamp = payload.at("sync_timestamp").get<uint64_t>();
    if (cmd.videoUrl.empty() || cmd.videoWidth > 65535 || cmd.videoHeight > 65535 ||
        cmd.syncTimestamp == 0 || !parseCropRegions(payload.at("crops"), cmd.videoWidth,
                                                     cmd.videoHeight, cmd.crops)) {
        return false;
    }
    return true;
}

} // namespace

const char* audioOutputModeName(AudioOutputMode mode) {
    switch (mode) {
        case AudioOutputMode::HDMI: return "hdmi";
        case AudioOutputMode::ANALOG: return "analog";
        case AudioOutputMode::BOTH: return "both";
    }
    return "both";
}

bool parseAudioOutputMode(const std::string& value, AudioOutputMode& mode) {
    if (value == "hdmi") {
        mode = AudioOutputMode::HDMI;
        return true;
    }
    if (value == "analog") {
        mode = AudioOutputMode::ANALOG;
        return true;
    }
    if (value == "both") {
        mode = AudioOutputMode::BOTH;
        return true;
    }
    return false;
}

const char* signalSourceTypeName(SignalSourceType type) {
    switch (type) {
        case SignalSourceType::NONE: return "none";
        case SignalSourceType::CAPTURE: return "capture";
        case SignalSourceType::STREAM: return "stream";
        case SignalSourceType::NETWORK_CAMERA: return "network_camera";
    }
    return "none";
}

bool parseSignalSourceType(const std::string& value, SignalSourceType& type) {
    if (value == "none") {
        type = SignalSourceType::NONE;
        return true;
    }
    if (value == "capture") {
        type = SignalSourceType::CAPTURE;
        return true;
    }
    if (value == "stream") {
        type = SignalSourceType::STREAM;
        return true;
    }
    if (value == "network_camera") {
        type = SignalSourceType::NETWORK_CAMERA;
        return true;
    }
    return false;
}

bool hasNetworkUriScheme(const std::string& value);
bool isWindowTransportEndpoint(const std::string& value);

std::string Protocol::signalStreamEndpoint(uint32_t sourceNodeId) {
    if (sourceNodeId == 0 || sourceNodeId > 65535U ||
        SIGNAL_RTP_BASE_PORT > 65535U - sourceNodeId) {
        return {};
    }
    const uint32_t high = (sourceNodeId >> 8U) & 0xffU;
    const uint32_t low = sourceNodeId & 0xffU;
    return "rtp://239.192." + std::to_string(high) + "." +
           std::to_string(low) + ":" +
           std::to_string(static_cast<uint32_t>(SIGNAL_RTP_BASE_PORT) + sourceNodeId);
}

bool Protocol::validateSignalSourceConfig(const SignalSourceConfig& source,
                                          std::string* error) {
    const auto fail = [error](const char* message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (source.width > 65535 || source.height > 65535 ||
        (source.width == 0) != (source.height == 0) ||
        source.framerateNumerator > 1000 || source.framerateDenominator == 0 ||
        source.framerateDenominator > 1000 || source.endpoint.size() > 2048 ||
        source.pixelFormat.size() > 32 || hasControlCharacters(source.endpoint) ||
        hasControlCharacters(source.pixelFormat)) {
        return fail("signal source parameters are out of range");
    }

    switch (source.type) {
        case SignalSourceType::NONE:
            if (!source.endpoint.empty() || source.width != 0 || source.height != 0 ||
                source.framerateNumerator != 0 || source.framerateDenominator != 1 ||
                source.pixelFormat != "auto") {
                return fail("none signal source must not have endpoint parameters");
            }
            return true;
        case SignalSourceType::CAPTURE:
            if (source.endpoint.empty() || source.pixelFormat.empty()) {
                return fail("capture signal source requires a device and pixel format");
            }
            return true;
        case SignalSourceType::STREAM:
        case SignalSourceType::NETWORK_CAMERA:
            if (source.endpoint.empty() || !hasNetworkUriScheme(source.endpoint)) {
                return fail("network signal source requires an rtsp/http/udp/tcp URI");
            }
            if (source.pixelFormat != "auto") {
                return fail("network signal source pixel format must be auto");
            }
            return true;
    }
    return fail("unknown signal source type");
}

uint32_t Protocol::calculateCRC32(const uint8_t* data, size_t length) {
    if (data == nullptr && length != 0) {
        throw std::invalid_argument("CRC32 data is null");
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

bool Protocol::verifyChecksum(const Protocol::Packet& packet) {
    return packet.header.payloadLength == packet.payload.size() &&
           calculatePacketChecksum(packet) == packet.header.checksum;
}

uint64_t Protocol::getCurrentTimestamp() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count());
}

std::vector<uint8_t> Protocol::serialize(const Protocol::Packet& packet) {
    if (packet.header.magic != PROTOCOL_MAGIC || packet.header.version != PROTOCOL_VERSION) {
        throw std::invalid_argument("Packet header magic/version is invalid");
    }
    if (!isKnownCommand(packet.header.commandType)) {
        throw std::invalid_argument("Packet command type is invalid");
    }
    if (packet.header.sequenceId == 0) {
        throw std::invalid_argument("Packet sequenceId must not be zero");
    }
    if (packet.header.payloadLength != packet.payload.size()) {
        throw std::invalid_argument("Packet payload length does not match payload");
    }
    if (WIRE_HEADER_SIZE + packet.payload.size() > MAX_PACKET_SIZE) {
        throw std::invalid_argument("Packet exceeds MAX_PACKET_SIZE");
    }
    if (!verifyChecksum(packet)) {
        throw std::invalid_argument("Packet checksum is invalid");
    }

    std::vector<uint8_t> buffer;
    buffer.reserve(WIRE_HEADER_SIZE + packet.payload.size());
    appendHeader(buffer, packet.header, packet.header.checksum);
    buffer.insert(buffer.end(), packet.payload.begin(), packet.payload.end());
    return buffer;
}

Protocol::Packet Protocol::deserialize(const uint8_t* data, size_t length) {
    if (data == nullptr || length < WIRE_HEADER_SIZE || length > MAX_PACKET_SIZE) {
        throw std::runtime_error("Invalid packet size");
    }

    Protocol::Packet packet{};
    packet.header.magic = readU32(data + 0);
    packet.header.version = readU32(data + 4);
    packet.header.commandType = readU32(data + 8);
    packet.header.sequenceId = readU32(data + 12);
    packet.header.payloadLength = readU32(data + 16);
    packet.header.checksum = readU32(data + 20);
    packet.header.timestamp = readU64(data + 24);
    packet.header.reserved = readU32(data + 32);
    packet.header.reserved2 = readU32(data + 36);

    if (packet.header.magic != PROTOCOL_MAGIC || packet.header.version != PROTOCOL_VERSION ||
        !isKnownCommand(packet.header.commandType) || packet.header.sequenceId == 0) {
        throw std::runtime_error("Invalid packet header");
    }
    if (packet.header.payloadLength > MAX_PACKET_SIZE - WIRE_HEADER_SIZE ||
        length != WIRE_HEADER_SIZE + packet.header.payloadLength) {
        throw std::runtime_error("Invalid payload length");
    }

    packet.payload.assign(data + WIRE_HEADER_SIZE, data + length);
    if (!verifyChecksum(packet)) {
        throw std::runtime_error("Invalid checksum");
    }
    return packet;
}

Protocol::Packet Protocol::createSyncPlayPacket(
    uint32_t sequenceId,
    const std::string& videoUrl,
    uint32_t videoWidth,
    uint32_t videoHeight,
    uint64_t syncTimestamp,
    const std::vector<CropRegion>& crops) {
    return createPlayPacket(sequenceId, CommandType::SYNC_PLAY, videoUrl, videoWidth,
                            videoHeight, syncTimestamp, crops);
}

Protocol::Packet Protocol::createPreparePlayPacket(
    uint32_t sequenceId,
    const std::string& videoUrl,
    uint32_t videoWidth,
    uint32_t videoHeight,
    uint64_t syncTimestamp,
    const std::vector<CropRegion>& crops) {
    return createPlayPacket(sequenceId, CommandType::PREPARE_PLAY, videoUrl, videoWidth,
                            videoHeight, syncTimestamp, crops);
}

Protocol::Packet Protocol::createCommitPlayPacket(uint32_t sequenceId, uint32_t commandId) {
    if (commandId == 0) {
        throw std::invalid_argument("commandId must not be zero");
    }
    return makeCommandPacket(sequenceId, CommandType::COMMIT_PLAY,
                             {{"cmd", "commit_play"}, {"command_id", commandId}});
}

Protocol::Packet Protocol::createStopPacket(uint32_t sequenceId) {
    return makeCommandPacket(sequenceId, CommandType::STOP, {{"cmd", "stop"}});
}

Protocol::Packet Protocol::createPausePacket(uint32_t sequenceId) {
    return makeCommandPacket(sequenceId, CommandType::PAUSE, {{"cmd", "pause"}});
}

Protocol::Packet Protocol::createResumePacket(uint32_t sequenceId) {
    return makeCommandPacket(sequenceId, CommandType::RESUME, {{"cmd", "resume"}});
}

Protocol::Packet Protocol::createSetAudioOutputPacket(uint32_t sequenceId,
                                                      AudioOutputMode mode) {
    return makeCommandPacket(sequenceId, CommandType::SET_AUDIO_OUTPUT,
                             {{"cmd", "set_audio_output"},
                              {"mode", audioOutputModeName(mode)}});
}

Protocol::Packet Protocol::createSetAudioVolumePacket(uint32_t sequenceId,
                                                       uint32_t volumePercent) {
    if (volumePercent > 100) {
        throw std::invalid_argument("audio volume percent must be between 0 and 100");
    }
    return makeCommandPacket(sequenceId, CommandType::SET_AUDIO_VOLUME,
                             {{"cmd", "set_audio_volume"},
                              {"volume_percent", volumePercent}});
}

Protocol::Packet Protocol::createKvmRoutePacket(uint32_t sequenceId,
                                                 const KvmRouteCommand& command) {
    struct in_addr address {};
    if (command.sessionId == 0 || command.targetNodeId == 0 ||
        command.controllerNodeId == command.targetNodeId ||
        command.targetIp.empty() || command.targetPort == 0 ||
        command.sessionToken == 0 || command.expiresAt == 0 ||
        inet_pton(AF_INET, command.targetIp.c_str(), &address) != 1) {
        throw std::invalid_argument("Invalid KVM route command");
    }
    return makeCommandPacket(sequenceId, CommandType::KVM_ROUTE,
        {{"cmd", "kvm_route"}, {"session_id", command.sessionId},
         {"controller_node_id", command.controllerNodeId},
         {"target_node_id", command.targetNodeId}, {"target_ip", command.targetIp},
         {"target_port", command.targetPort}, {"session_token", command.sessionToken},
         {"expires_at", command.expiresAt}});
}

Protocol::Packet Protocol::createKvmReleasePacket(uint32_t sequenceId, uint32_t sessionId) {
    if (sessionId == 0) throw std::invalid_argument("KVM sessionId must not be zero");
    return makeCommandPacket(sequenceId, CommandType::KVM_RELEASE,
                             {{"cmd", "kvm_release"}, {"session_id", sessionId}});
}

Protocol::Packet Protocol::createSetNodeRolePacket(uint32_t sequenceId,
                                                   uint32_t nodeId,
                                                   NodeRole role) {
    if (nodeId == 0) throw std::invalid_argument("nodeId must not be zero");
    return makeCommandPacket(sequenceId, CommandType::SET_NODE_ROLE,
                             {{"cmd", "set_node_role"}, {"node_id", nodeId},
                              {"role", nodeRoleName(role)}});
}

bool Protocol::validateSetNodeNetworkCommand(const SetNodeNetworkCommand& cmd,
                                             std::string* error) {
    const auto reject = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    const auto validIpv4 = [](const std::string& value) {
        in_addr address{};
        return !value.empty() && inet_pton(AF_INET, value.c_str(), &address) == 1;
    };

    if (cmd.nodeId == 0) return reject("nodeId must be a positive integer");
    if (cmd.mode != "auto" && cmd.mode != "manual") {
        return reject("mode must be auto or manual");
    }
    if (cmd.dnsServers.size() > 2) return reject("dnsServers accepts at most two addresses");
    if (cmd.mode == "auto") {
        if (!cmd.address.empty() || cmd.prefixLength != 0 || !cmd.gateway.empty() ||
            !cmd.dnsServers.empty()) {
            return reject("automatic mode requires empty address, gateway, and DNS values");
        }
        return true;
    }
    if (!validIpv4(cmd.address)) return reject("address must be a valid IPv4 address");
    if (cmd.prefixLength < 1 || cmd.prefixLength > 32) {
        return reject("prefixLength must be between 1 and 32");
    }
    if (!validIpv4(cmd.gateway)) return reject("gateway must be a valid IPv4 address");
    if (cmd.dnsServers.empty()) return reject("manual mode requires at least one DNS server");
    for (const auto& dns : cmd.dnsServers) {
        if (!validIpv4(dns)) return reject("dnsServers must contain valid IPv4 addresses");
    }
    return true;
}

bool hasControlCharacters(const std::string& value) {
    for (const unsigned char character : value) {
        if (character < 0x20U || character == 0x7FU) return true;
    }
    return false;
}

bool hasNetworkUriScheme(const std::string& value) {
    return value.rfind("rtsp://", 0) == 0 || value.rfind("http://", 0) == 0 ||
           value.rfind("https://", 0) == 0 || value.rfind("udp://", 0) == 0 ||
           value.rfind("tcp://", 0) == 0;
}

bool isWindowTransportEndpoint(const std::string& value) {
    constexpr const char* fallbackMarker = "|fallback=";
    const size_t separator = value.find(fallbackMarker);
    const std::string primary = separator == std::string::npos
        ? value : value.substr(0, separator);
    const std::string fallback = separator == std::string::npos
        ? std::string{} : value.substr(separator + std::char_traits<char>::length(fallbackMarker));
    const bool primaryHttp = primary.rfind("http://", 0) == 0 ||
                             primary.rfind("https://", 0) == 0;
    const bool primaryRtp = primary.rfind("rtp://", 0) == 0;
    if (!primaryHttp && !primaryRtp) return false;
    if (separator == std::string::npos) return primaryHttp;
    return primaryRtp && (fallback.rfind("http://", 0) == 0 ||
                          fallback.rfind("https://", 0) == 0);
}

Protocol::Packet Protocol::createSetNodeNetworkPacket(
    uint32_t sequenceId,
    const SetNodeNetworkCommand& command) {
    std::string error;
    if (!validateSetNodeNetworkCommand(command, &error)) {
        throw std::invalid_argument(error);
    }
    return makeCommandPacket(sequenceId, CommandType::SET_NODE_NETWORK,
                             {{"cmd", "set_node_network"},
                              {"node_id", command.nodeId},
                              {"mode", command.mode},
                              {"address", command.address},
                              {"prefix_length", command.prefixLength},
                               {"gateway", command.gateway},
                               {"dns_servers", command.dnsServers}});
}

Protocol::Packet Protocol::createSetDisplayLayoutPacket(
    uint32_t sequenceId,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    const std::vector<CropRegion>& crops) {
    return createDisplayLayoutPacket(sequenceId, sourceWidth, sourceHeight, crops);
}

Protocol::Packet Protocol::createSetNodeSourcePacket(
    uint32_t sequenceId,
    const SetNodeSourceCommand& command) {
    return createNodeSourcePacket(sequenceId, command);
}

Protocol::Packet Protocol::createSetNodeIdPacket(
    uint32_t sequenceId,
    const SetNodeIdCommand& command) {
    return createNodeIdPacket(sequenceId, command);
}

Protocol::Packet Protocol::createSetWindowLayoutPacket(
    uint32_t sequenceId,
    const SetWindowLayoutCommand& command) {
    if (command.targetNodeId == 0 || command.outputWidth == 0 ||
        command.outputHeight == 0 || command.outputWidth > 16384 ||
        command.outputHeight > 16384 || command.sources.size() > MAX_NODES ||
        command.layers.size() > MAX_WINDOW_LAYERS) {
        throw std::invalid_argument("window layout dimensions or counts are invalid");
    }
    if (command.layers.empty() != command.sources.empty()) {
        throw std::invalid_argument("window layout sources and layers must both be empty or populated");
    }

    json payload = {{"cmd", "set_window_layout"},
                    {"target_node_id", command.targetNodeId},
                    {"output_width", command.outputWidth},
                    {"output_height", command.outputHeight},
                    {"sources", json::array()},
                    {"layers", json::array()}};
    std::set<uint32_t> sourceNodeIds;
    for (const auto& source : command.sources) {
        if (source.sourceNodeId == 0 || source.endpoint.empty() || source.endpoint.size() > 2048 ||
            hasControlCharacters(source.endpoint) ||
            !isWindowTransportEndpoint(source.endpoint) ||
            source.width == 0 || source.height == 0 ||
            source.width > 16384 || source.height > 16384 ||
            !sourceNodeIds.insert(source.sourceNodeId).second) {
            throw std::invalid_argument("window layout source is invalid");
        }
        payload["sources"].push_back({{"source_node_id", source.sourceNodeId},
                                      {"endpoint", source.endpoint},
                                      {"width", source.width},
                                      {"height", source.height}});
    }

    std::set<uint32_t> windowIds;
    std::map<uint32_t, uint32_t> windowCountBySource;
    std::set<uint32_t> usedSourceNodeIds;
    for (const auto& layer : command.layers) {
        const auto source = std::find_if(command.sources.begin(), command.sources.end(),
            [&](const WindowSource& value) {
                return value.sourceNodeId == layer.sourceNodeId;
            });
        if (layer.windowId == 0 || layer.sourceNodeId == 0 || layer.zOrder == 0 ||
            layer.sourceWidth == 0 || layer.sourceHeight == 0 ||
            layer.targetWidth == 0 || layer.targetHeight == 0 ||
            source == command.sources.end() ||
            layer.sourceX > source->width - std::min(source->width, layer.sourceWidth) ||
            layer.sourceY > source->height - std::min(source->height, layer.sourceHeight) ||
            layer.sourceX + layer.sourceWidth > source->width ||
            layer.sourceY + layer.sourceHeight > source->height ||
            layer.targetX > command.outputWidth - std::min(command.outputWidth, layer.targetWidth) ||
            layer.targetY > command.outputHeight - std::min(command.outputHeight, layer.targetHeight) ||
            layer.targetX + layer.targetWidth > command.outputWidth ||
            layer.targetY + layer.targetHeight > command.outputHeight ||
            !windowIds.insert(layer.windowId).second ||
            ++windowCountBySource[layer.sourceNodeId] > MAX_WINDOWS_PER_SOURCE) {
            throw std::invalid_argument("window layout layer is invalid");
        }
        usedSourceNodeIds.insert(layer.sourceNodeId);
        payload["layers"].push_back({{"window_id", layer.windowId},
                                     {"source_node_id", layer.sourceNodeId},
                                     {"source_x", layer.sourceX},
                                     {"source_y", layer.sourceY},
                                     {"source_width", layer.sourceWidth},
                                     {"source_height", layer.sourceHeight},
                                     {"target_x", layer.targetX},
                                     {"target_y", layer.targetY},
                                     {"target_width", layer.targetWidth},
                                     {"target_height", layer.targetHeight},
                                     {"z_order", layer.zOrder}});
    }
    if (usedSourceNodeIds != sourceNodeIds) {
        throw std::invalid_argument("window layout contains an unused source");
    }
    return makeCommandPacket(sequenceId, CommandType::SET_WINDOW_LAYOUT, payload);
}

bool Protocol::parsePreparePlayCommand(const std::string& jsonText, SyncPlayCommand& cmd) {
    try {
        return parsePlayPayload(json::parse(jsonText), "prepare_play", cmd);
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to parse prepare command: %s", error.what());
        return false;
    }
}

bool Protocol::parseCommitPlayCommand(const std::string& jsonText, CommitPlayCommand& cmd) {
    try {
        const json payload = json::parse(jsonText);
        if (!hasExactFields(payload, {"cmd", "command_id"}) ||
            payload.at("cmd").get<std::string>() != "commit_play") {
            return false;
        }
        cmd = CommitPlayCommand{};
        cmd.commandId = requiredU32(payload, "command_id", false);
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to parse commit command: %s", error.what());
        return false;
    }
}

bool Protocol::parseSetAudioOutputCommand(const std::string& jsonText,
                                          SetAudioOutputCommand& cmd) {
    try {
        const json payload = json::parse(jsonText);
        if (!hasExactFields(payload, {"cmd", "mode"}) ||
            !payload.at("cmd").is_string() ||
            payload.at("cmd").get<std::string>() != "set_audio_output" ||
            !payload.at("mode").is_string()) {
            return false;
        }
        cmd = SetAudioOutputCommand{};
        return parseAudioOutputMode(payload.at("mode").get<std::string>(), cmd.mode);
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to parse set audio output command: %s", error.what());
        return false;
    }
}

bool Protocol::parseSetAudioVolumeCommand(const std::string& jsonText,
                                          SetAudioVolumeCommand& cmd) {
    try {
        const json payload = json::parse(jsonText);
        if (!hasExactFields(payload, {"cmd", "volume_percent"}) ||
            payload.at("cmd").get<std::string>() != "set_audio_volume" ||
            !payload.at("volume_percent").is_number_unsigned()) {
            return false;
        }
        cmd = SetAudioVolumeCommand{};
        cmd.volumePercent = requiredU32(payload, "volume_percent");
        return cmd.volumePercent <= 100;
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to parse set audio volume command: %s", error.what());
        return false;
    }
}

bool Protocol::parseKvmRouteCommand(const std::string& jsonText, KvmRouteCommand& cmd) {
    try {
        const json payload = json::parse(jsonText);
        if (!hasExactFields(payload, {"cmd", "session_id", "controller_node_id",
                                     "target_node_id", "target_ip", "target_port",
                                     "session_token", "expires_at"}) ||
            !payload.at("cmd").is_string() || payload.at("cmd") != "kvm_route" ||
            !payload.at("target_ip").is_string() ||
            !payload.at("session_token").is_number_unsigned() ||
            !payload.at("expires_at").is_number_unsigned()) {
            return false;
        }
        cmd = KvmRouteCommand{};
        cmd.sessionId = requiredU32(payload, "session_id", false);
        cmd.controllerNodeId = requiredU32(payload, "controller_node_id");
        cmd.targetNodeId = requiredU32(payload, "target_node_id", false);
        cmd.targetIp = payload.at("target_ip").get<std::string>();
        const uint32_t port = requiredU32(payload, "target_port", false);
        cmd.sessionToken = payload.at("session_token").get<uint64_t>();
        cmd.expiresAt = payload.at("expires_at").get<uint64_t>();
        struct in_addr address {};
        if (cmd.controllerNodeId == cmd.targetNodeId || cmd.targetIp.empty() || port > 65535 ||
            inet_pton(AF_INET, cmd.targetIp.c_str(), &address) != 1 ||
            cmd.sessionToken == 0 || cmd.expiresAt == 0) return false;
        cmd.targetPort = static_cast<uint16_t>(port);
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to parse KVM route command: %s", error.what());
        return false;
    }
}

bool Protocol::parseKvmReleaseCommand(const std::string& jsonText, KvmReleaseCommand& cmd) {
    try {
        const json payload = json::parse(jsonText);
        if (!hasExactFields(payload, {"cmd", "session_id"}) ||
            !payload.at("cmd").is_string() || payload.at("cmd") != "kvm_release") return false;
        cmd = KvmReleaseCommand{};
        cmd.sessionId = requiredU32(payload, "session_id", false);
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to parse KVM release command: %s", error.what());
        return false;
    }
}

bool Protocol::parseSetNodeRoleCommand(const std::string& jsonText, SetNodeRoleCommand& cmd) {
    try {
        const json payload = json::parse(jsonText);
        if (!hasExactFields(payload, {"cmd", "node_id", "role"}) ||
            !payload.at("cmd").is_string() || payload.at("cmd") != "set_node_role" ||
            !payload.at("role").is_string()) return false;
        cmd = SetNodeRoleCommand{};
        cmd.nodeId = requiredU32(payload, "node_id", false);
        return parseNodeRole(payload.at("role").get<std::string>(), cmd.role);
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to parse node role command: %s", error.what());
        return false;
    }
}

bool Protocol::parseSetNodeNetworkCommand(const std::string& jsonText,
                                          SetNodeNetworkCommand& cmd) {
    try {
        const json value = json::parse(jsonText);
        if (!hasExactFields(value, {"cmd", "node_id", "mode", "address",
                                    "prefix_length", "gateway", "dns_servers"}) ||
            !value.at("cmd").is_string() ||
            value.at("cmd").get<std::string>() != "set_node_network" ||
            !value.at("mode").is_string() || !value.at("address").is_string() ||
            !value.at("gateway").is_string() || !value.at("dns_servers").is_array()) {
            return false;
        }
        SetNodeNetworkCommand parsed;
        parsed.nodeId = requiredU32(value, "node_id", false);
        parsed.mode = value.at("mode").get<std::string>();
        parsed.address = value.at("address").get<std::string>();
        parsed.prefixLength = requiredU32(value, "prefix_length");
        parsed.gateway = value.at("gateway").get<std::string>();
        for (const auto& dns : value.at("dns_servers")) {
            if (!dns.is_string()) return false;
            parsed.dnsServers.push_back(dns.get<std::string>());
        }
        if (!validateSetNodeNetworkCommand(parsed)) return false;
        cmd = std::move(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool Protocol::parseSetDisplayLayoutCommand(const std::string& jsonText,
                                            SetDisplayLayoutCommand& cmd) {
    try {
        const json payload = json::parse(jsonText);
        if (!hasExactFields(payload, {"cmd", "source_width", "source_height", "crops"}) ||
            !payload.at("cmd").is_string() ||
            payload.at("cmd").get<std::string>() != "set_display_layout" ||
            !payload.at("crops").is_array()) {
            return false;
        }
        cmd = SetDisplayLayoutCommand{};
        cmd.sourceWidth = requiredU32(payload, "source_width", false);
        cmd.sourceHeight = requiredU32(payload, "source_height", false);
        if (cmd.sourceWidth > 65535 || cmd.sourceHeight > 65535 ||
            payload.at("crops").size() > MAX_NODES) {
            return false;
        }
        if (!payload.at("crops").empty() &&
            !parseCropRegions(payload.at("crops"), cmd.sourceWidth, cmd.sourceHeight,
                              cmd.crops)) {
            return false;
        }
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to parse display layout command: %s", error.what());
        return false;
    }
}

bool Protocol::parseSetWindowLayoutCommand(const std::string& jsonText,
                                           SetWindowLayoutCommand& cmd) {
    try {
        const json payload = json::parse(jsonText);
        if (!hasExactFields(payload, {"cmd", "target_node_id", "output_width",
                                      "output_height", "sources", "layers"}) ||
            !payload.at("cmd").is_string() ||
            payload.at("cmd").get<std::string>() != "set_window_layout" ||
            !payload.at("sources").is_array() || !payload.at("layers").is_array() ||
            payload.at("sources").size() > MAX_NODES ||
            payload.at("layers").size() > MAX_WINDOW_LAYERS) {
            return false;
        }

        SetWindowLayoutCommand parsed;
        parsed.targetNodeId = requiredU32(payload, "target_node_id", false);
        parsed.outputWidth = requiredU32(payload, "output_width", false);
        parsed.outputHeight = requiredU32(payload, "output_height", false);
        if (parsed.outputWidth > 16384 || parsed.outputHeight > 16384 ||
            payload.at("sources").empty() != payload.at("layers").empty()) {
            return false;
        }

        std::set<uint32_t> sourceNodeIds;
        for (const auto& value : payload.at("sources")) {
            if (!hasExactFields(value, {"source_node_id", "endpoint", "width", "height"}) ||
                !value.at("endpoint").is_string()) {
                return false;
            }
            WindowSource source;
            source.sourceNodeId = requiredU32(value, "source_node_id", false);
            source.endpoint = value.at("endpoint").get<std::string>();
            source.width = requiredU32(value, "width", false);
            source.height = requiredU32(value, "height", false);
            if (source.endpoint.empty() || source.endpoint.size() > 2048 ||
                hasControlCharacters(source.endpoint) ||
                !isWindowTransportEndpoint(source.endpoint) ||
                source.width > 16384 || source.height > 16384 ||
                !sourceNodeIds.insert(source.sourceNodeId).second) {
                return false;
            }
            parsed.sources.push_back(std::move(source));
        }

        std::set<uint32_t> windowIds;
        std::map<uint32_t, uint32_t> windowCountBySource;
        std::set<uint32_t> usedSourceNodeIds;
        for (const auto& value : payload.at("layers")) {
            if (!hasExactFields(value, {"window_id", "source_node_id", "source_x",
                                        "source_y", "source_width", "source_height",
                                        "target_x", "target_y", "target_width",
                                        "target_height", "z_order"})) {
                return false;
            }
            WindowLayer layer;
            layer.windowId = requiredU32(value, "window_id", false);
            layer.sourceNodeId = requiredU32(value, "source_node_id", false);
            layer.sourceX = requiredU32(value, "source_x");
            layer.sourceY = requiredU32(value, "source_y");
            layer.sourceWidth = requiredU32(value, "source_width", false);
            layer.sourceHeight = requiredU32(value, "source_height", false);
            layer.targetX = requiredU32(value, "target_x");
            layer.targetY = requiredU32(value, "target_y");
            layer.targetWidth = requiredU32(value, "target_width", false);
            layer.targetHeight = requiredU32(value, "target_height", false);
            layer.zOrder = requiredU32(value, "z_order", false);
            const auto source = std::find_if(parsed.sources.begin(), parsed.sources.end(),
                [&](const WindowSource& item) {
                    return item.sourceNodeId == layer.sourceNodeId;
                });
            if (source == parsed.sources.end() ||
                layer.sourceX > source->width - std::min(source->width, layer.sourceWidth) ||
                layer.sourceY > source->height - std::min(source->height, layer.sourceHeight) ||
                layer.sourceX + layer.sourceWidth > source->width ||
                layer.sourceY + layer.sourceHeight > source->height ||
                layer.targetX > parsed.outputWidth -
                    std::min(parsed.outputWidth, layer.targetWidth) ||
                layer.targetY > parsed.outputHeight -
                    std::min(parsed.outputHeight, layer.targetHeight) ||
                layer.targetX + layer.targetWidth > parsed.outputWidth ||
                layer.targetY + layer.targetHeight > parsed.outputHeight ||
                !windowIds.insert(layer.windowId).second ||
                ++windowCountBySource[layer.sourceNodeId] > MAX_WINDOWS_PER_SOURCE) {
                return false;
            }
            usedSourceNodeIds.insert(layer.sourceNodeId);
            parsed.layers.push_back(layer);
        }
        if (usedSourceNodeIds != sourceNodeIds) return false;
        cmd = std::move(parsed);
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to parse window layout command: %s", error.what());
        return false;
    }
}

bool Protocol::parseSetNodeSourceCommand(const std::string& jsonText,
                                         SetNodeSourceCommand& cmd) {
    try {
        const json payload = json::parse(jsonText);
        if (!payload.is_object() || payload.size() != 9 ||
            !payload.contains("cmd") || !payload.at("cmd").is_string() ||
            payload.at("cmd").get<std::string>() != "set_node_source" ||
            !payload.contains("node_id") || !payload.at("node_id").is_number_unsigned()) {
            return false;
        }
        SetNodeSourceCommand parsed;
        parsed.nodeId = requiredU32(payload, "node_id", false);
        json source = payload;
        source.erase("cmd");
        source.erase("node_id");
        if (!parseSourceConfig(source, parsed.source)) return false;
        cmd = std::move(parsed);
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to parse node source command: %s", error.what());
        return false;
    }
}

bool Protocol::parseSetNodeIdCommand(const std::string& jsonText,
                                     SetNodeIdCommand& cmd) {
    try {
        const json payload = json::parse(jsonText);
        if (!hasExactFields(payload, {"cmd", "current_node_id", "new_node_id",
                                      "target_mac_address"}) ||
            !payload.at("cmd").is_string() || payload.at("cmd") != "set_node_id" ||
            !payload.at("target_mac_address").is_string()) {
            return false;
        }
        SetNodeIdCommand parsed;
        parsed.currentNodeId = requiredU32(payload, "current_node_id", false);
        parsed.newNodeId = requiredU32(payload, "new_node_id", false);
        parsed.targetMacAddress = payload.at("target_mac_address").get<std::string>();
        if (parsed.newNodeId > MAX_NODES || parsed.targetMacAddress.empty() ||
            parsed.targetMacAddress.size() > 32 ||
            hasControlCharacters(parsed.targetMacAddress)) {
            return false;
        }
        cmd = std::move(parsed);
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to parse node ID command: %s", error.what());
        return false;
    }
}

bool Protocol::parseJsonCommand(const std::string& jsonText,
                                CommandType& type,
                                SyncPlayCommand& cmd) {
    try {
        const json payload = json::parse(jsonText);
        if (!payload.is_object() || !payload.contains("cmd") || !payload.at("cmd").is_string()) {
            return false;
        }
        const std::string name = payload.at("cmd").get<std::string>();
        if (name == "stop" || name == "pause" || name == "resume") {
            if (!hasExactFields(payload, {"cmd"})) {
                return false;
            }
            type = name == "stop" ? CommandType::STOP
                 : name == "pause" ? CommandType::PAUSE : CommandType::RESUME;
            return true;
        }
        if (name == "sync_play" && parsePlayPayload(payload, "sync_play", cmd)) {
            type = CommandType::SYNC_PLAY;
            return true;
        }
        return false;
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to parse JSON command: %s", error.what());
        return false;
    }
}

} // namespace dms
