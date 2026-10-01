#pragma once

#include "common/NodeRole.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dms {

constexpr uint32_t PROTOCOL_MAGIC = 0x44565346;
constexpr uint32_t PROTOCOL_VERSION = 0x00010100;

constexpr const char* MULTICAST_GROUP = "239.1.1.1";
constexpr uint16_t MULTICAST_PORT = 9001;
constexpr uint16_t HTTP_PORT = 8080;

// Window scenes are sent as one atomic UDP command. Keep the packet below the
// maximum IPv4 UDP datagram size while allowing a practical multi-layer scene.
constexpr uint32_t MAX_PACKET_SIZE = 65507;
constexpr uint32_t MAX_NODES = 64;
constexpr uint32_t MAX_WINDOWS_PER_SOURCE = 16;
constexpr uint32_t MAX_WINDOW_LAYERS = 256;
constexpr uint16_t SIGNAL_RTP_BASE_PORT = 12000;

// CommandType is the single internal UDP command registry. REST endpoints are
// intentionally kept one-to-one and are not used as protocol aliases.
enum class CommandType : uint32_t {
    SYNC_PLAY   = 0x1001, // legacy one-phase playback command
    STOP        = 0x1002,
    PAUSE       = 0x1003,
    RESUME      = 0x1004,
    PREPARE_PLAY = 0x1005,
    COMMIT_PLAY  = 0x1006,
    SET_AUDIO_OUTPUT = 0x1007,
    SET_AUDIO_VOLUME = 0x1008,
    KVM_ROUTE = 0x1009,
    KVM_RELEASE = 0x100A,
    SET_NODE_ROLE = 0x100B,
    SET_NODE_NETWORK = 0x100C,
    SET_DISPLAY_LAYOUT = 0x100D,
    SET_NODE_SOURCE = 0x100E,
    SET_NODE_ID = 0x100F,
    SET_WINDOW_LAYOUT = 0x1010,
};

enum class AudioOutputMode : uint32_t {
    HDMI,
    ANALOG,
    BOTH,
};

const char* audioOutputModeName(AudioOutputMode mode);
bool parseAudioOutputMode(const std::string& value, AudioOutputMode& mode);

enum class SignalSourceType : uint32_t {
    NONE = 0,
    CAPTURE = 1,
    STREAM = 2,
    NETWORK_CAMERA = 3,
};

const char* signalSourceTypeName(SignalSourceType type);
bool parseSignalSourceType(const std::string& value, SignalSourceType& type);

    struct ScreenLayout {
        uint32_t rows;
        uint32_t cols;
        uint32_t width;
        uint32_t height;
    };

    // Coordinates are normalized to the complete display wall: [0, 1].
    // This is the single layout contract shared by REST and playback.
    struct ScreenPlacement {
        uint32_t nodeId = 0;
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
    };

    // A client window is an input source placed on the complete output wall.
    struct WindowPlacement {
        uint32_t windowId = 0;
        uint32_t sourceNodeId = 0;
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
        uint32_t zOrder = 0;
    };

struct VideoInfo {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint64_t duration;
};

struct SignalSourceConfig {
    SignalSourceType type = SignalSourceType::NONE;
    std::string endpoint;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t framerateNumerator = 0;
    uint32_t framerateDenominator = 1;
    std::string pixelFormat = "auto";
};

class Protocol {
public:
    struct PacketHeader {
        uint32_t magic;
        uint32_t version;
        uint32_t commandType;
        uint32_t sequenceId;
        uint32_t payloadLength;
        uint32_t checksum;
        uint64_t timestamp;
        uint32_t reserved;
        uint32_t reserved2;
    };

    struct Packet {
        PacketHeader header;
        std::vector<uint8_t> payload;
    };

    struct CropRegion {
        uint32_t nodeId;
        uint32_t cropX;
        uint32_t cropY;
        uint32_t cropWidth;
        uint32_t cropHeight;
    };

    struct SyncPlayCommand {
        std::string videoUrl;
        uint32_t videoWidth = 0;
        uint32_t videoHeight = 0;
        uint64_t syncTimestamp = 0;
        std::vector<CropRegion> crops;
    };

    struct CommitPlayCommand {
        uint32_t commandId = 0;
    };

    struct SetAudioOutputCommand {
        AudioOutputMode mode = AudioOutputMode::BOTH;
    };

    struct SetAudioVolumeCommand {
        uint32_t volumePercent = 100;
    };

    struct KvmRouteCommand {
        uint32_t sessionId = 0;
        uint32_t controllerNodeId = 0;
        uint32_t targetNodeId = 0;
        std::string targetIp;
        uint16_t targetPort = 0;
        uint64_t sessionToken = 0;
        uint64_t expiresAt = 0;
    };

    struct KvmReleaseCommand {
        uint32_t sessionId = 0;
    };

    struct SetNodeRoleCommand {
        uint32_t nodeId = 0;
        NodeRole role = NodeRole::UNASSIGNED;
    };

    struct SetNodeNetworkCommand {
        uint32_t nodeId = 0;
        std::string mode;
        std::string address;
        uint32_t prefixLength = 0;
        std::string gateway;
        std::vector<std::string> dnsServers;
    };

    struct SetDisplayLayoutCommand {
        uint32_t sourceWidth = 0;
        uint32_t sourceHeight = 0;
        std::vector<CropRegion> crops;
    };

    struct WindowSource {
        uint32_t sourceNodeId = 0;
        std::string endpoint;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct WindowLayer {
        uint32_t windowId = 0;
        uint32_t sourceNodeId = 0;
        uint32_t sourceX = 0;
        uint32_t sourceY = 0;
        uint32_t sourceWidth = 0;
        uint32_t sourceHeight = 0;
        uint32_t targetX = 0;
        uint32_t targetY = 0;
        uint32_t targetWidth = 0;
        uint32_t targetHeight = 0;
        uint32_t zOrder = 0;
    };

    struct SetWindowLayoutCommand {
        uint32_t targetNodeId = 0;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        std::vector<WindowSource> sources;
        std::vector<WindowLayer> layers;
    };

    struct SetNodeSourceCommand {
        uint32_t nodeId = 0;
        SignalSourceConfig source;
    };

    struct SetNodeIdCommand {
        uint32_t currentNodeId = 0;
        uint32_t newNodeId = 0;
        std::string targetMacAddress;
    };

    static_assert(sizeof(PacketHeader) == 40, "PacketHeader must be 40 bytes");

    static std::vector<uint8_t> serialize(const Packet& packet);
    static Packet deserialize(const uint8_t* data, size_t length);

    static Packet createSyncPlayPacket(
        uint32_t sequenceId,
        const std::string& videoUrl,
        uint32_t videoWidth,
        uint32_t videoHeight,
        uint64_t syncTimestamp,
        const std::vector<CropRegion>& crops);

    static Packet createPreparePlayPacket(
        uint32_t sequenceId,
        const std::string& videoUrl,
        uint32_t videoWidth,
        uint32_t videoHeight,
        uint64_t syncTimestamp,
        const std::vector<CropRegion>& crops);

    static Packet createCommitPlayPacket(uint32_t sequenceId, uint32_t commandId);
    static Packet createStopPacket(uint32_t sequenceId);
    static Packet createPausePacket(uint32_t sequenceId);
    static Packet createResumePacket(uint32_t sequenceId);
    static Packet createSetAudioOutputPacket(uint32_t sequenceId, AudioOutputMode mode);
    static Packet createSetAudioVolumePacket(uint32_t sequenceId, uint32_t volumePercent);
    static Packet createKvmRoutePacket(uint32_t sequenceId, const KvmRouteCommand& command);
    static Packet createKvmReleasePacket(uint32_t sequenceId, uint32_t sessionId);
    static Packet createSetNodeRolePacket(uint32_t sequenceId, uint32_t nodeId, NodeRole role);
    static Packet createSetNodeNetworkPacket(uint32_t sequenceId,
                                             const SetNodeNetworkCommand& command);
    static Packet createSetDisplayLayoutPacket(uint32_t sequenceId,
                                               uint32_t sourceWidth,
                                               uint32_t sourceHeight,
                                               const std::vector<CropRegion>& crops);
    static Packet createSetNodeSourcePacket(uint32_t sequenceId,
                                            const SetNodeSourceCommand& command);
    static Packet createSetNodeIdPacket(uint32_t sequenceId,
                                        const SetNodeIdCommand& command);
    static Packet createSetWindowLayoutPacket(uint32_t sequenceId,
                                              const SetWindowLayoutCommand& command);

    static bool parseJsonCommand(const std::string& json, CommandType& type, SyncPlayCommand& cmd);
    static bool parsePreparePlayCommand(const std::string& json, SyncPlayCommand& cmd);
    static bool parseCommitPlayCommand(const std::string& json, CommitPlayCommand& cmd);
    static bool parseSetAudioOutputCommand(const std::string& json, SetAudioOutputCommand& cmd);
    static bool parseSetAudioVolumeCommand(const std::string& json, SetAudioVolumeCommand& cmd);
    static bool parseKvmRouteCommand(const std::string& json, KvmRouteCommand& cmd);
    static bool parseKvmReleaseCommand(const std::string& json, KvmReleaseCommand& cmd);
    static bool parseSetNodeRoleCommand(const std::string& json, SetNodeRoleCommand& cmd);
    static bool parseSetNodeNetworkCommand(const std::string& json, SetNodeNetworkCommand& cmd);
    static bool parseSetDisplayLayoutCommand(const std::string& json,
                                             SetDisplayLayoutCommand& cmd);
    static bool parseSetNodeSourceCommand(const std::string& json,
                                          SetNodeSourceCommand& cmd);
    static bool parseSetNodeIdCommand(const std::string& json,
                                      SetNodeIdCommand& cmd);
    static bool parseSetWindowLayoutCommand(const std::string& json,
                                            SetWindowLayoutCommand& cmd);
    static bool validateSignalSourceConfig(const SignalSourceConfig& source,
                                           std::string* error = nullptr);
    // Returns the canonical per-source multicast endpoint used by the optional
    // RK MPP H.264 transport. The mapping is deterministic on every node.
    static std::string signalStreamEndpoint(uint32_t sourceNodeId);
    static bool validateSetNodeNetworkCommand(const SetNodeNetworkCommand& cmd,
                                              std::string* error = nullptr);

    static uint32_t calculateCRC32(const uint8_t* data, size_t length);
    static bool verifyChecksum(const Packet& packet);
    static uint64_t getCurrentTimestamp();
};

} // namespace dms
