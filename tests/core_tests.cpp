#include "common/DiscoveryProtocol.h"
#include "common/NetworkOptions.h"
#include "common/Protocol.h"
#include "master/LayoutCalculator.h"
#include "kvm/KvmSessionManager.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template <typename Function>
void requireThrows(Function&& function, const char* message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void writeU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes.at(offset) = static_cast<uint8_t>(value >> 24);
    bytes.at(offset + 1) = static_cast<uint8_t>(value >> 16);
    bytes.at(offset + 2) = static_cast<uint8_t>(value >> 8);
    bytes.at(offset + 3) = static_cast<uint8_t>(value);
}
}

int main() {
    using namespace dms;
    IpAssignmentMode ipMode = IpAssignmentMode::AUTO;
    require(parseIpAssignmentMode("manual", ipMode) &&
                ipMode == IpAssignmentMode::MANUAL &&
                std::string(ipAssignmentModeName(ipMode)) == "manual",
            "manual IP mode parsing");
    require(parseIpAssignmentMode("auto", ipMode) &&
                ipMode == IpAssignmentMode::AUTO &&
                std::string(ipAssignmentModeName(ipMode)) == "auto",
            "automatic IP mode parsing");
    require(!parseIpAssignmentMode("dhcp", ipMode), "unknown IP mode rejection");
    const std::string discoveryRequest = createDiscoveryRequest(123456);
    uint64_t discoveryRequestId = 0;
    require(parseDiscoveryRequest(discoveryRequest, discoveryRequestId) &&
                discoveryRequestId == 123456,
            "discovery request round-trip");
    require(!parseDiscoveryRequest("not-json", discoveryRequestId),
            "malformed discovery request rejection");
    require(!parseDiscoveryRequest(
                R"({"type":"discovery_request","version":2,"requestId":1})",
                discoveryRequestId),
            "discovery version rejection");
    require(!parseDiscoveryRequest(
                R"({"type":"discovery_request","version":1,"requestId":1,"extra":true})",
                discoveryRequestId),
            "discovery unknown field rejection");
    require(!parseDiscoveryRequest(
                R"({"type":"discovery_request","version":1,"requestId":0})",
                discoveryRequestId),
            "zero discovery request ID rejection");
    require(createDiscoveryRequest(0).empty(), "zero discovery request creation rejection");

    const std::vector<Protocol::CropRegion> crops{{1, 0, 0, 1920, 1080},
                                                   {2, 1920, 0, 1920, 1080}};
    const auto packet = Protocol::createSyncPlayPacket(7, "http://host/media/test.mp4", 3840, 1080, 123456, crops);
    const auto bytes = Protocol::serialize(packet);
    const auto decoded = Protocol::deserialize(bytes.data(), bytes.size());
    require(decoded.header.sequenceId == 7, "protocol sequence round-trip");
    require(Protocol::verifyChecksum(decoded), "protocol checksum");
    require(bytes.size() >= 40 && bytes[0] == 0x44 && bytes[1] == 0x56 && bytes[2] == 0x53 && bytes[3] == 0x46,
            "wire magic must use network byte order");
    require(bytes[4] == 0x00 && bytes[5] == 0x01 && bytes[6] == 0x01 && bytes[7] == 0x00,
            "wire version must use network byte order");
    require(bytes[8] == 0x00 && bytes[9] == 0x00 && bytes[10] == 0x10 && bytes[11] == 0x01,
            "wire command must use network byte order");

    CommandType type{};
    Protocol::SyncPlayCommand command;
    require(Protocol::parseJsonCommand(std::string(decoded.payload.begin(), decoded.payload.end()), type, command),
            "protocol JSON parse");
    require(type == CommandType::SYNC_PLAY && command.videoWidth == 3840 &&
            command.videoHeight == 1080 && command.crops.size() == 2 &&
            command.crops[1].nodeId == 2,
            "protocol video dimensions and crop round-trip");

    const auto preparePacket = Protocol::createPreparePlayPacket(
        42, "http://host/media/prepare.mp4", 3840, 1080, 654321, crops);
    const auto prepareBytes = Protocol::serialize(preparePacket);
    const auto decodedPrepare = Protocol::deserialize(prepareBytes.data(), prepareBytes.size());
    require(decodedPrepare.header.commandType == static_cast<uint32_t>(CommandType::PREPARE_PLAY),
            "prepare command wire type");
    Protocol::SyncPlayCommand prepareCommand;
    require(Protocol::parsePreparePlayCommand(
                std::string(decodedPrepare.payload.begin(), decodedPrepare.payload.end()), prepareCommand) &&
                prepareCommand.videoUrl == "http://host/media/prepare.mp4" &&
                prepareCommand.videoWidth == 3840 && prepareCommand.crops.size() == 2,
            "prepare command round-trip");
    require(!Protocol::parsePreparePlayCommand(
                R"({"cmd":"prepare_play","video_url":"x","video_width":1,"video_height":1,"sync_timestamp":1,"crops":[],"extra":true})",
                prepareCommand), "prepare unknown field rejection");

    const auto commitPacket = Protocol::createCommitPlayPacket(43, 42);
    const auto commitBytes = Protocol::serialize(commitPacket);
    const auto decodedCommit = Protocol::deserialize(commitBytes.data(), commitBytes.size());
    Protocol::CommitPlayCommand commitCommand;
    require(Protocol::parseCommitPlayCommand(
                std::string(decodedCommit.payload.begin(), decodedCommit.payload.end()), commitCommand) &&
                commitCommand.commandId == 42,
            "commit command round-trip");
    require(!Protocol::parseCommitPlayCommand(R"({"cmd":"commit_play","command_id":0})", commitCommand),
            "zero commit command rejection");
    require(!Protocol::parseCommitPlayCommand(
                R"({"cmd":"commit_play","command_id":42,"extra":true})", commitCommand),
            "commit unknown field rejection");

    const auto audioPacket = Protocol::createSetAudioOutputPacket(44, AudioOutputMode::BOTH);
    const auto audioBytes = Protocol::serialize(audioPacket);
    const auto decodedAudio = Protocol::deserialize(audioBytes.data(), audioBytes.size());
    require(decodedAudio.header.commandType == static_cast<uint32_t>(CommandType::SET_AUDIO_OUTPUT),
            "set audio output wire type");
    Protocol::SetAudioOutputCommand audioCommand;
    require(Protocol::parseSetAudioOutputCommand(
                std::string(decodedAudio.payload.begin(), decodedAudio.payload.end()), audioCommand) &&
                audioCommand.mode == AudioOutputMode::BOTH,
            "set audio output round-trip");
    require(Protocol::parseSetAudioOutputCommand(
                R"({"cmd":"set_audio_output","mode":"hdmi"})", audioCommand) &&
                audioCommand.mode == AudioOutputMode::HDMI,
            "HDMI audio output parse");
    require(Protocol::parseSetAudioOutputCommand(
                R"({"cmd":"set_audio_output","mode":"analog"})", audioCommand) &&
                audioCommand.mode == AudioOutputMode::ANALOG,
            "analog audio output parse");
    require(!Protocol::parseSetAudioOutputCommand(
                R"({"cmd":"set_audio_output","mode":"invalid"})", audioCommand),
            "invalid audio output rejection");
    require(!Protocol::parseSetAudioOutputCommand(
                R"({"cmd":"set_audio_output","mode":"both","extra":true})", audioCommand),
            "set audio output unknown field rejection");

    const auto volumePacket = Protocol::createSetAudioVolumePacket(45, 72);
    const auto volumeBytes = Protocol::serialize(volumePacket);
    const auto decodedVolume = Protocol::deserialize(volumeBytes.data(), volumeBytes.size());
    require(decodedVolume.header.commandType == static_cast<uint32_t>(CommandType::SET_AUDIO_VOLUME),
            "set audio volume wire type");
    Protocol::SetAudioVolumeCommand volumeCommand;
    require(Protocol::parseSetAudioVolumeCommand(
                std::string(decodedVolume.payload.begin(), decodedVolume.payload.end()), volumeCommand) &&
                volumeCommand.volumePercent == 72,
            "set audio volume round-trip");
    require(!Protocol::parseSetAudioVolumeCommand(
                R"({"cmd":"set_audio_volume","volume_percent":101})", volumeCommand),
            "audio volume upper bound rejection");
    requireThrows([] { Protocol::createSetAudioVolumePacket(46, 101); },
                  "audio volume packet upper bound rejection");

    Protocol::KvmRouteCommand kvmRoute{7, 0, 2, "10.0.0.2", 9101, 123456789, 9999999999999ULL};
    const auto kvmPacket = Protocol::createKvmRoutePacket(47, kvmRoute);
    const auto kvmBytes = Protocol::serialize(kvmPacket);
    const auto decodedKvm = Protocol::deserialize(kvmBytes.data(), kvmBytes.size());
    require(decodedKvm.header.commandType == static_cast<uint32_t>(CommandType::KVM_ROUTE),
            "KVM route wire type");
    Protocol::KvmRouteCommand parsedKvm;
    require(Protocol::parseKvmRouteCommand(
                std::string(decodedKvm.payload.begin(), decodedKvm.payload.end()), parsedKvm) &&
                parsedKvm.sessionId == 7 && parsedKvm.controllerNodeId == 0 &&
                parsedKvm.targetNodeId == 2 && parsedKvm.targetPort == 9101 &&
                parsedKvm.sessionToken == 123456789,
            "KVM route round-trip");
    require(!Protocol::parseKvmRouteCommand(
                R"({"cmd":"kvm_route","session_id":7,"controller_node_id":1,"target_node_id":2,"target_ip":"bad","target_port":9101,"session_token":1,"expires_at":2})",
                parsedKvm), "KVM invalid target IP rejection");
    require(!Protocol::parseKvmRouteCommand(
                R"({"cmd":"kvm_route","session_id":7,"controller_node_id":1,"target_node_id":2,"target_ip":"10.0.0.2","target_port":9101,"session_token":1,"expires_at":2,"extra":true})",
                parsedKvm), "KVM route unknown field rejection");
    Protocol::KvmReleaseCommand kvmRelease;
    const auto releasePacket = Protocol::createKvmReleasePacket(48, 7);
    require(Protocol::parseKvmReleaseCommand(
                std::string(releasePacket.payload.begin(), releasePacket.payload.end()), kvmRelease) &&
                kvmRelease.sessionId == 7, "KVM release round-trip");
    require(!Protocol::parseKvmReleaseCommand(
                R"({"cmd":"kvm_release","session_id":0})", kvmRelease),
            "KVM zero release session rejection");

    const auto nodeRolePacket = Protocol::createSetNodeRolePacket(49, 7, NodeRole::DECODE);
    require(nodeRolePacket.header.commandType == static_cast<uint32_t>(CommandType::SET_NODE_ROLE),
            "node role wire type");
    Protocol::SetNodeRoleCommand nodeRoleCommand;
    require(Protocol::parseSetNodeRoleCommand(
                std::string(nodeRolePacket.payload.begin(), nodeRolePacket.payload.end()),
                nodeRoleCommand) && nodeRoleCommand.nodeId == 7 &&
                nodeRoleCommand.role == NodeRole::DECODE,
            "node role round-trip");
    require(!Protocol::parseSetNodeRoleCommand(
                R"({"cmd":"set_node_role","node_id":7,"role":"invalid"})", nodeRoleCommand),
            "invalid node role rejection");
    const auto unassignedRolePacket = Protocol::createSetNodeRolePacket(50, 8, NodeRole::UNASSIGNED);
    require(Protocol::parseSetNodeRoleCommand(
                std::string(unassignedRolePacket.payload.begin(), unassignedRolePacket.payload.end()),
                nodeRoleCommand) && nodeRoleCommand.nodeId == 8 &&
                nodeRoleCommand.role == NodeRole::UNASSIGNED,
            "unassigned node role round-trip");

    Protocol::SetNodeNetworkCommand manualNetwork{7, "manual", "192.168.2.103", 24,
                                                   "192.168.2.1", {"223.5.5.5", "114.114.114.114"}};
    const auto networkPacket = Protocol::createSetNodeNetworkPacket(50, manualNetwork);
    require(networkPacket.header.commandType == static_cast<uint32_t>(CommandType::SET_NODE_NETWORK),
            "node network wire type");
    Protocol::SetNodeNetworkCommand parsedNetwork;
    require(Protocol::parseSetNodeNetworkCommand(
                std::string(networkPacket.payload.begin(), networkPacket.payload.end()), parsedNetwork) &&
                parsedNetwork.nodeId == 7 && parsedNetwork.mode == "manual" &&
                parsedNetwork.address == "192.168.2.103" && parsedNetwork.prefixLength == 24 &&
                parsedNetwork.gateway == "192.168.2.1" && parsedNetwork.dnsServers.size() == 2,
            "node network manual round-trip");
    const Protocol::SetNodeNetworkCommand automaticNetwork{7, "auto", "", 0, "", {}};
    require(Protocol::validateSetNodeNetworkCommand(automaticNetwork),
            "node network automatic validation");
    require(!Protocol::validateSetNodeNetworkCommand(
                Protocol::SetNodeNetworkCommand{7, "auto", "192.168.2.103", 0, "", {}}),
            "automatic network address rejection");
    require(!Protocol::validateSetNodeNetworkCommand(
                Protocol::SetNodeNetworkCommand{7, "manual", "192.168.2.103", 33,
                                                "192.168.2.1", {"223.5.5.5"}}),
            "network prefix upper bound rejection");
    require(!Protocol::parseSetNodeNetworkCommand(
                R"({"cmd":"set_node_network","node_id":7,"mode":"manual","address":"192.168.2.103","prefix_length":24,"gateway":"192.168.2.1","dns_servers":["223.5.5.5"],"extra":true})",
                parsedNetwork), "node network unknown field rejection");

    Protocol::SetNodeIdCommand nodeIdAssignment{1, 2, "96:42:17:95:59:AA"};
    const auto nodeIdPacket = Protocol::createSetNodeIdPacket(51, nodeIdAssignment);
    require(nodeIdPacket.header.commandType == static_cast<uint32_t>(CommandType::SET_NODE_ID),
            "node ID assignment wire type");
    Protocol::SetNodeIdCommand parsedNodeIdAssignment;
    require(Protocol::parseSetNodeIdCommand(
                std::string(nodeIdPacket.payload.begin(), nodeIdPacket.payload.end()),
                parsedNodeIdAssignment) && parsedNodeIdAssignment.currentNodeId == 1 &&
                parsedNodeIdAssignment.newNodeId == 2 &&
                parsedNodeIdAssignment.targetMacAddress == "96:42:17:95:59:AA",
            "node ID assignment round-trip");
    require(!Protocol::parseSetNodeIdCommand(
                R"({"cmd":"set_node_id","current_node_id":1,"new_node_id":65,"target_mac_address":"96:42:17:95:59:AA"})",
                parsedNodeIdAssignment), "node ID assignment upper bound rejection");
    require(!Protocol::parseSetNodeIdCommand(
                R"({"cmd":"set_node_id","current_node_id":1,"new_node_id":2,"target_mac_address":"96:42:17:95:59:AA","extra":true})",
                parsedNodeIdAssignment), "node ID assignment unknown field rejection");

    KvmSessionManager kvmManager;
    KvmSession session;
    require(kvmManager.acquire(0, 2, "10.0.0.2", 9101, 1000, session) &&
                session.sessionId != 0 && session.sessionToken != 0,
            "KVM session acquisition");
    KvmSession other;
    require(!kvmManager.acquire(3, 4, "10.0.0.4", 9101, 1000, other),
            "KVM exclusive session enforcement");
    require(!kvmManager.release(session.sessionId + 1, other), "KVM wrong session release rejection");
    require(kvmManager.expire(session.expiresAt, other) && other.sessionId == session.sessionId,
            "KVM lease expiration");
    require(!kvmManager.acquire(1, 1, "10.0.0.1", 9101, 1000, other),
            "KVM self-route rejection");
    require(!kvmManager.acquire(1, 2, "10.0.0.2", 9101, 999, other),
            "KVM short lease rejection");
    require(formatNodeId(1) == "001" && formatNodeId(12) == "012",
            "three-digit node ID formatting");
    require(Protocol::parseJsonCommand(R"({"cmd":"stop"})", type, command) && type == CommandType::STOP,
            "stop JSON parse");
    require(Protocol::parseJsonCommand(R"({"cmd":"pause"})", type, command) && type == CommandType::PAUSE,
            "pause JSON parse");
    require(Protocol::parseJsonCommand(R"({"cmd":"resume"})", type, command) && type == CommandType::RESUME,
            "resume JSON parse");
    require(!Protocol::parseJsonCommand(R"({"cmd":"stop","extra":true})", type, command),
            "simple command unknown field rejection");
    require(!Protocol::parseJsonCommand(
        R"({"cmd":"sync_play","video_url":"x","video_width":2,"video_height":1,"sync_timestamp":1,"crops":[{"node_id":1,"x":0,"y":0,"width":1,"height":1}],"extra":true})",
        type, command), "sync command unknown field rejection");
    require(!Protocol::parseJsonCommand(
        R"({"cmd":"sync_play","video_url":"x","video_width":2,"video_height":1,"sync_timestamp":1,"crops":[{"node_id":1,"x":0,"y":0,"width":1,"height":1,"extra":true}]})",
        type, command), "crop unknown field rejection");
    require(!Protocol::parseJsonCommand(R"({"cmd":"unknown"})", type, command), "unknown JSON command rejection");
    require(!Protocol::parseJsonCommand(
        R"({"cmd":"sync_play","video_url":"x","video_width":2,"video_height":1,"sync_timestamp":1,"crops":[{"node_id":1,"x":0,"y":0,"width":1,"height":1},{"node_id":1,"x":1,"y":0,"width":1,"height":1}]})",
        type, command), "duplicate crop node rejection");

    auto corrupted = bytes;
    corrupted.back() ^= 0x01;
    requireThrows([&] { Protocol::deserialize(corrupted.data(), corrupted.size()); }, "checksum corruption rejection");
    auto truncated = bytes;
    truncated.pop_back();
    requireThrows([&] { Protocol::deserialize(truncated.data(), truncated.size()); }, "truncated packet rejection");
    auto trailing = bytes;
    trailing.push_back(0);
    requireThrows([&] { Protocol::deserialize(trailing.data(), trailing.size()); }, "trailing byte rejection");
    auto wrongMagic = bytes;
    wrongMagic[0] ^= 0x01;
    requireThrows([&] { Protocol::deserialize(wrongMagic.data(), wrongMagic.size()); }, "wrong magic rejection");
    auto wrongVersion = bytes;
    wrongVersion[7] ^= 0x01;
    requireThrows([&] { Protocol::deserialize(wrongVersion.data(), wrongVersion.size()); }, "wrong version rejection");
    auto unknownCommand = bytes;
    writeU32(unknownCommand, 8, 0xFFFFFFFFu);
    requireThrows([&] { Protocol::deserialize(unknownCommand.data(), unknownCommand.size()); }, "unknown command rejection");
    auto wrongLength = bytes;
    writeU32(wrongLength, 16, static_cast<uint32_t>(packet.payload.size() + 1));
    requireThrows([&] { Protocol::deserialize(wrongLength.data(), wrongLength.size()); }, "payload length mismatch rejection");
    std::vector<uint8_t> oversize(MAX_PACKET_SIZE + 1, 0);
    requireThrows([&] { Protocol::deserialize(oversize.data(), oversize.size()); }, "oversize packet rejection");
    requireThrows([] { Protocol::deserialize(nullptr, 40); }, "null packet rejection");
    requireThrows([] { Protocol::calculateCRC32(nullptr, 1); }, "null CRC input rejection");
    require(Protocol::calculateCRC32(nullptr, 0) == 0, "empty CRC value");
    requireThrows([] {
        Protocol::createSyncPlayPacket(0, "x", 1, 1, 1, {{1, 0, 0, 1, 1}});
    }, "zero sequence rejection");
    requireThrows([] {
        Protocol::createSyncPlayPacket(1, "x", 1, 1, 1, {{0, 0, 0, 1, 1}});
    }, "zero crop node rejection");
    requireThrows([] {
        Protocol::createSyncPlayPacket(1, "x", 1, 1, 1,
            {{1, std::numeric_limits<uint32_t>::max(), 0, 2, 1}});
    }, "crop overflow rejection");
    requireThrows([] {
        Protocol::createSyncPlayPacket(1, "x", 2, 1, 1, {{1, 0, 0, 1, 1}, {1, 1, 0, 1, 1}});
    }, "duplicate crop node creation rejection");
    requireThrows([] {
        Protocol::createSyncPlayPacket(1, "x", 0, 1, 1, {{1, 0, 0, 1, 1}});
    }, "zero source width rejection");
    requireThrows([] {
        Protocol::createSyncPlayPacket(1, "x", 1, 1, 1, {{1, 1, 0, 1, 1}});
    }, "crop outside source bounds rejection");

    const auto displayLayoutPacket = Protocol::createSetDisplayLayoutPacket(
        7, 1920, 1080, {{1, 960, 0, 960, 540}});
    Protocol::SetDisplayLayoutCommand displayLayoutCommand;
    require(Protocol::parseSetDisplayLayoutCommand(
                std::string(displayLayoutPacket.payload.begin(), displayLayoutPacket.payload.end()),
                displayLayoutCommand) &&
                displayLayoutCommand.sourceWidth == 1920 &&
                displayLayoutCommand.sourceHeight == 1080 &&
                displayLayoutCommand.crops.size() == 1 &&
                displayLayoutCommand.crops.front().nodeId == 1 &&
                displayLayoutCommand.crops.front().cropX == 960,
            "display layout command carries live crop coordinates");

    Protocol::SetWindowLayoutCommand windowLayout;
    windowLayout.targetNodeId = 2;
    windowLayout.outputWidth = 1920;
    windowLayout.outputHeight = 1080;
    windowLayout.sources.push_back({1, "http://192.168.2.101:9102/kvm/preview.jpg?stream=1",
                                    1920, 1080});
    windowLayout.layers.push_back({7, 1, 0, 0, 1920, 1080, 0, 0, 960, 540, 1});
    windowLayout.layers.push_back({8, 1, 0, 0, 1920, 1080, 960, 540, 960, 540, 2});
    const auto windowPacket = Protocol::createSetWindowLayoutPacket(8, windowLayout);
    require(windowPacket.header.commandType == static_cast<uint32_t>(CommandType::SET_WINDOW_LAYOUT),
            "window layout wire type");
    require(Protocol::parseSetWindowLayoutCommand(
                std::string(windowPacket.payload.begin(), windowPacket.payload.end()),
                windowLayout) && windowLayout.targetNodeId == 2 &&
                windowLayout.sources.size() == 1 && windowLayout.layers.size() == 2,
            "window layout command round-trip");
    require(Protocol::signalStreamEndpoint(2) == "rtp://239.192.0.2:12002",
            "canonical RTP endpoint");
    windowLayout.sources.front().endpoint =
        "rtp://239.192.0.2:12002|fallback=http://127.0.0.1:9102/kvm/preview.jpg?stream=1";
    const auto rtpWindowPacket = Protocol::createSetWindowLayoutPacket(9, windowLayout);
    Protocol::SetWindowLayoutCommand rtpWindowLayout;
    require(Protocol::parseSetWindowLayoutCommand(
                std::string(rtpWindowPacket.payload.begin(), rtpWindowPacket.payload.end()),
                rtpWindowLayout) && rtpWindowLayout.sources.front().endpoint ==
                windowLayout.sources.front().endpoint,
            "RTP window endpoint packet round-trip");
    require(!Protocol::parseSetWindowLayoutCommand(
                R"({"cmd":"set_window_layout","target_node_id":2,"output_width":1920,"output_height":1080,"sources":[],"layers":[],"extra":true})",
                windowLayout), "window layout unknown field rejection");
    require(!Protocol::parseSetWindowLayoutCommand(
                R"({"cmd":"set_window_layout","target_node_id":2,"output_width":1920,"output_height":1080,"sources":[{"source_node_id":1,"endpoint":"http://127.0.0.1:9102/kvm/preview.jpg?stream=1","width":1920,"height":1080}],"layers":[{"window_id":1,"source_node_id":1,"source_x":0,"source_y":0,"source_width":1920,"source_height":1080,"target_x":0,"target_y":0,"target_width":960,"target_height":540,"z_order":1},{"window_id":1,"source_node_id":1,"source_x":0,"source_y":0,"source_width":1920,"source_height":1080,"target_x":960,"target_y":0,"target_width":960,"target_height":540,"z_order":2}]})",
                windowLayout), "window layout duplicate ID rejection");

    dms::LayoutCalculator calculator;
    calculator.setLayout(ScreenLayout{2, 2, 1920, 1080});
    calculator.setVideoInfo(VideoInfo{3840, 2160, 60, 0});
    const auto regions = calculator.calculateCropRegions();
    require(regions.size() == 4, "2x2 crop count");
    require(regions.front().nodeId == 1 && regions.back().nodeId == 4, "layout node IDs must be 1-based");
    require(regions[0].cropWidth == 1920 && regions[0].cropHeight == 1080, "2x2 crop size");
    calculator.setVideoInfo(VideoInfo{4096, 2160, 60, 0});
    const auto wide = calculator.calculateCropRegions();
    require(wide.front().cropX == 128 && wide.back().cropX == 2048, "wide video center crop");
    calculator.setVideoInfo(VideoInfo{3840, 2160, 60, 0});
    calculator.setPlacements({
        {7, 0.5, 0.0, 0.5, 0.5},
        {3, 0.0, 0.5, 0.5, 0.5},
    });
    const auto custom = calculator.calculateCropRegions();
    require(custom.size() == 2 && custom[0].nodeId == 7 && custom[0].cropX == 1920 &&
                custom[0].cropY == 0 && custom[0].cropWidth == 1920 &&
                custom[0].cropHeight == 1080,
            "custom placement maps wall coordinates to source crop");
    require(calculator.getCropRegion(3).nodeId == 3 &&
                calculator.getCropRegion(3).cropY == 1080,
            "custom placement lookup uses node ID");
    calculator.setPlacements({});
    requireThrows([&] { calculator.setLayout(ScreenLayout{0, 2, 1920, 1080}); }, "invalid layout rejection");
    requireThrows([&] { calculator.setLayout(ScreenLayout{8, 9, 1920, 1080}); }, "too many layout nodes rejection");
    requireThrows([&] { calculator.setVideoInfo(VideoInfo{1920, 1080, 241, 0}); }, "excessive FPS rejection");
    calculator.setLayout(ScreenLayout{8, 8, 1920, 1080});
    calculator.setVideoInfo(VideoInfo{1, 1, 60, 0});
    requireThrows([&] { calculator.calculateCropRegions(); }, "video too small for layout rejection");

    std::cout << "core_tests passed\n";
}
