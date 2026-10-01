#include "common/DiscoveryProtocol.h"
#include "common/Protocol.h"
#include "master/CommandBroadcaster.h"
#include "master/MasterNodeApp.h"
#include "master/NodeDiscovery.h"
#include "common/SyncTimestampGenerator.h"
#include "kvm/KvmAgent.h"
#include "node/CommandReceiver.h"
#include "node/MediaPlayer.h"
#include "node/SlaveNodeApp.h"

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <nlohmann/json.hpp>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

bool waitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

bool sendMulticast(const std::string& group, uint16_t port, const std::string& data) {
    const int socketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd < 0) {
        return false;
    }
    const unsigned char loop = 1;
    setsockopt(socketFd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, group.c_str(), &address.sin_addr) != 1) {
        close(socketFd);
        return false;
    }
    const ssize_t sent = sendto(socketFd, data.data(), data.size(), 0,
                                reinterpret_cast<sockaddr*>(&address), sizeof(address));
    close(socketFd);
    return sent == static_cast<ssize_t>(data.size());
}

bool sendPacket(const std::string& group, uint16_t port, const dms::Protocol::Packet& packet) {
    const auto bytes = dms::Protocol::serialize(packet);
    return sendMulticast(group, port, std::string(
        reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::string requestPreview(uint16_t port, const std::string& target) {
    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0) return {};
    timeval timeout{};
    timeout.tv_sec = 2;
    setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(socketFd);
        return {};
    }
    const std::string request =
        "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t written = send(socketFd, request.data() + sent,
                                     request.size() - sent, 0);
        if (written <= 0) {
            close(socketFd);
            return {};
        }
        sent += static_cast<size_t>(written);
    }
    std::string response;
    char buffer[4096];
    while (true) {
        const ssize_t received = recv(socketFd, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        response.append(buffer, static_cast<size_t>(received));
    }
    close(socketFd);
    return response;
}

void testTimestampGenerator() {
    dms::SyncTimestampGenerator generator;
    require(!generator.isSynchronized(), "new timestamp generator must be unsynchronized");

    const uint64_t local = dms::Protocol::getCurrentTimestamp();
    const uint64_t generated = generator.getCurrentTimestamp();
    require(generated >= local - 50 && generated <= local + 50, "local timestamp range");

    require(generator.synchronizeTo(2000, 1000), "reference clock synchronization");
    require(generator.getTimeOffset() == 1000, "reference clock offset");
    require(generator.isSynchronized(), "reference clock synchronization state");
    generator.reset();
    require(!generator.isSynchronized() && generator.getTimeOffset() == 0,
            "timestamp generator reset");

    generator.setTimeOffset(1000);
    require(generator.getCurrentTimestamp() >= local + 900, "positive time offset");
    generator.setTimeOffset(-1000);
    require(generator.getCurrentTimestamp() + 900 <= dms::Protocol::getCurrentTimestamp(), "negative time offset");

    generator.setTimeOffset(0);
    const uint64_t target = generator.generateSyncTimestamp(60);
    const int64_t waited = generator.waitUntilTimestamp(target, 5);
    require(waited >= 45 && waited < 500, "timestamp wait duration");
    require(generator.getTimeUntilTimestamp(target) == 0, "timestamp reached");

    generator.setTimeOffset(std::numeric_limits<int64_t>::min());
    require(generator.getCurrentTimestamp() == 0, "minimum signed offset must clamp to zero");
}

void testSharedLivePreview() {
    dms::KvmOptions options;
    options.enabled = true;
    options.role = dms::KvmRole::TARGET;
    options.listenPort = static_cast<uint16_t>(42000 + (getpid() % 10000));
    dms::KvmAgent agent;
    require(agent.initialize(7, options), "preview agent initialization");
    const auto jpeg = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0xff, 0xd8, 0xff, 0xd9});
    agent.setPreviewFrameProvider([jpeg] { return jpeg; });
    require(agent.start(), "preview agent start");

    const uint16_t previewPort = static_cast<uint16_t>(options.listenPort + 1);
    const std::string canvasResponse = requestPreview(
        previewPort, "/kvm/preview.jpg?v=1");
    require(canvasResponse.find("HTTP/1.1 200 OK") == 0,
            "canvas preview response status");
    require(canvasResponse.find("Content-Type: image/jpeg") != std::string::npos,
            "canvas preview content type");
    require(canvasResponse.size() >= jpeg->size() &&
                canvasResponse.compare(
                    canvasResponse.size() - jpeg->size(), jpeg->size(),
                    reinterpret_cast<const char*>(jpeg->data()), jpeg->size()) == 0,
            "canvas preview JPEG body");

    const std::string streamResponse = requestPreview(
        previewPort, "/kvm/preview.jpg?stream=1");
    require(streamResponse.find("HTTP/1.1 200 OK") == 0,
            "MJPEG preview response status");
    require(streamResponse.find(
                "Content-Type: multipart/x-mixed-replace; boundary=dmsframe") !=
                std::string::npos,
            "MJPEG preview content type");
    require(streamResponse.find("--dmsframe\r\nContent-Type: image/jpeg") !=
                std::string::npos,
            "MJPEG preview frame boundary");

    const std::string partialAuthResponse = requestPreview(
        previewPort, "/kvm/preview.jpg?sessionId=1&v=2");
    require(partialAuthResponse.find("HTTP/1.1 403 Forbidden") == 0,
            "partial KVM preview credentials rejection");
    agent.shutdown();
}

void testCommandTransport() {
    const std::string group = "239.255.77.77";
    const uint16_t port = static_cast<uint16_t>(22000 + (getpid() % 10000));

    dms::CommandReceiver invalidReceiver;
    require(!invalidReceiver.initialize(nullptr, port, [](const auto&) {}), "null multicast rejection");
    require(!invalidReceiver.initialize("127.0.0.1", port, [](const auto&) {}), "unicast rejection");
    require(!invalidReceiver.initialize(group.c_str(), 0, [](const auto&) {}), "zero port rejection");
    require(!invalidReceiver.initialize(group.c_str(), port, {}), "empty callback rejection");

    std::atomic<uint32_t> receivedType{0};
    std::atomic<uint32_t> callbackCount{0};
    dms::CommandReceiver receiver;
    require(receiver.initialize(group.c_str(), port, [&](const dms::Protocol::Packet& packet) {
        receivedType.store(packet.header.commandType);
        callbackCount.fetch_add(1);
    }), "command receiver initialization");
    require(!receiver.initialize(group.c_str(), port, [](const auto&) {}), "receiver duplicate initialization rejection");
    require(receiver.start(), "command receiver start");
    require(!receiver.start(), "receiver duplicate start rejection");

    dms::CommandBroadcaster broadcaster;
    require(!broadcaster.initialize(nullptr, port), "broadcaster null multicast rejection");
    require(!broadcaster.initialize("127.0.0.1", port), "broadcaster unicast rejection");
    require(!broadcaster.initialize(group.c_str(), 0), "broadcaster zero port rejection");
    require(broadcaster.initialize(group.c_str(), port), "command broadcaster initialization");
    require(!broadcaster.initialize(group.c_str(), port), "broadcaster duplicate initialization rejection");
    const std::vector<dms::Protocol::CropRegion> crops{{1, 0, 0, 1, 1}};
    uint32_t commandId = 0;
    require(broadcaster.broadcastPreparePlay("http://host/video.mp4", 1, 1,
                                             dms::Protocol::getCurrentTimestamp() + 500,
                                             crops, commandId), "prepare command broadcast");
    require(commandId != 0, "prepare command id");
    require(waitFor([&] { return receivedType.load() == static_cast<uint32_t>(dms::CommandType::PREPARE_PLAY); },
                    std::chrono::milliseconds(1500)), "prepare multicast command reception");
    require(broadcaster.broadcastCommitPlay(commandId), "commit command broadcast");
    require(waitFor([&] { return receivedType.load() == static_cast<uint32_t>(dms::CommandType::COMMIT_PLAY); },
                    std::chrono::milliseconds(1500)), "commit multicast command reception");
    require(broadcaster.broadcastStop(), "stop command broadcast");
    require(waitFor([&] { return receivedType.load() == static_cast<uint32_t>(dms::CommandType::STOP); },
                    std::chrono::milliseconds(1500)), "stop multicast command reception");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(receiver.getReceivedPacketCount() == 3, "redundant datagrams must be deduplicated");
    require(callbackCount.load() == 3, "deduplicated command callback count");

    // The identity includes command type, so a different command type with the
    // same sequence number must not be suppressed as a duplicate.
    const auto sameSequencePrepare = dms::Protocol::createPreparePlayPacket(
        777, "http://host/video-2.mp4", 1, 1,
        dms::Protocol::getCurrentTimestamp() + 500, crops);
    const auto sameSequenceStop = dms::Protocol::createStopPacket(777);
    const auto sameSequencePrepareBytes = dms::Protocol::serialize(sameSequencePrepare);
    const auto sameSequenceStopBytes = dms::Protocol::serialize(sameSequenceStop);
    require(sendMulticast(group, port, std::string(
        reinterpret_cast<const char*>(sameSequencePrepareBytes.data()),
        sameSequencePrepareBytes.size())), "same-sequence prepare send");
    require(sendMulticast(group, port, std::string(
        reinterpret_cast<const char*>(sameSequenceStopBytes.data()),
        sameSequenceStopBytes.size())), "same-sequence stop send");
    require(waitFor([&] { return receiver.getReceivedPacketCount() == 5; },
                    std::chrono::milliseconds(1500)), "command type participates in identity");

    const auto windowAnchor = dms::Protocol::createStopPacket(1000);
    require(sendPacket(group, port, windowAnchor), "dedupe window anchor send");
    require(waitFor([&] { return receiver.getReceivedPacketCount() == 6; },
                    std::chrono::milliseconds(1500)), "dedupe window anchor reception");
    for (uint32_t sequence = 2000; sequence < 2128; ++sequence) {
        require(sendPacket(group, port, dms::Protocol::createPausePacket(sequence)),
                "dedupe window distinct packet send");
    }
    require(waitFor([&] { return receiver.getReceivedPacketCount() == 134; },
                    std::chrono::milliseconds(2000)), "dedupe window population");
    require(sendPacket(group, port, windowAnchor), "dedupe window anchor replay send");
    require(waitFor([&] { return receiver.getReceivedPacketCount() == 135; },
                    std::chrono::milliseconds(1500)), "dedupe window eviction behavior");

    const std::string invalidPacket = "not-a-protocol-packet";
    require(sendMulticast(group, port, invalidPacket), "invalid multicast packet send");
    require(waitFor([&] { return receiver.getFailedPacketCount() >= 1; },
                    std::chrono::milliseconds(1500)), "invalid command packet count");

    const auto stopStarted = std::chrono::steady_clock::now();
    receiver.shutdown();
    require(std::chrono::steady_clock::now() - stopStarted < std::chrono::seconds(1),
            "command receiver shutdown latency");
    broadcaster.shutdown();
}


std::string makeHeartbeat(uint32_t nodeId,
                          const std::string& playState,
                          uint32_t commandId,
                          uint64_t actualStartTimestamp,
                          const std::string& lastError = {}) {
    return nlohmann::json{
        {"type", "heartbeat"},
        {"nodeId", nodeId},
        {"nodeRole", "decode"},
        {"ip", "10.0.0." + std::to_string(nodeId)},
        {"deviceModel", "test-device"},
        {"deviceName", "test-node-" + std::to_string(nodeId)},
        {"softwareVersion", "1.0.0"},
        {"boardInfo", "test-board"},
        {"macAddress", "00:11:22:33:44:" +
            std::to_string((nodeId / 10) % 10) + std::to_string(nodeId % 10)},
        {"subnetMask", "255.255.255.0"},
        {"gateway", "10.0.0.1"},
        {"deviceType", "output"},
        {"status", "online"},
        {"playState", playState},
        {"resolution", "1920x1080"},
        {"cpu", 10.0},
        {"memory", 20.0},
        {"audioOutputMode", "both"},
        {"masterClockSynchronized", true},
        {"masterClockOffsetMs", 0},
        {"commandId", commandId},
        {"actualStartTimestamp", actualStartTimestamp},
        {"lastError", lastError},
        {"kvmEnabled", false},
        {"kvmRole", "disabled"},
        {"kvmPort", 0},
        {"kvmSessionId", 0},
        {"kvmState", "disabled"},
        {"kvmLastError", ""},
        {"timestamp", dms::Protocol::getCurrentTimestamp()}
    }.dump();
}

void testMasterPlaybackAggregation() {
    const uint16_t httpPort = static_cast<uint16_t>(40000 + (getpid() % 1000));
    const uint16_t commandPort = static_cast<uint16_t>(41000 + (getpid() % 1000));
    const uint16_t heartbeatPort = static_cast<uint16_t>(42000 + (getpid() % 1000));
    const std::string cacheDir = "/tmp/dms-master-test-" + std::to_string(getpid());
    std::filesystem::remove_all(cacheDir);

    dms::MasterNodeOptions options;
    options.httpPort = httpPort;
    options.cacheDir = cacheDir;
    options.network.ipMode = dms::IpAssignmentMode::MANUAL;
    options.network.localIp = "127.0.0.1";
    options.webRoot = "web";
    options.defaultSyncDelayMs = 1000;
    options.layout = dms::ScreenLayout{1, 2, 1920, 1080};
    options.network.commandMulticastAddress = "239.255.77.79";
    options.network.commandPort = commandPort;
    options.network.heartbeatMulticastAddress = "239.255.77.80";
    options.network.heartbeatPort = heartbeatPort;

    dms::MasterNodeApp app;
    require(app.initialize(options), "master aggregation initialization");
    require(app.play("http://127.0.0.1/video.mp4", dms::VideoInfo{1920, 1080, 30, 0}, 1000),
            "master aggregation playback command");
    const auto scheduled = app.getPlaybackStatus();
    require(scheduled.commandId != 0 && scheduled.expectedNodeCount == 2 &&
                scheduled.missingNodeCount == 2,
            "master aggregation transaction baseline");

    require(sendMulticast(options.network.heartbeatMulticastAddress, heartbeatPort,
                          makeHeartbeat(1, "ready", scheduled.commandId, 0)),
            "ready heartbeat node 1");
    require(sendMulticast(options.network.heartbeatMulticastAddress, heartbeatPort,
                          makeHeartbeat(2, "ready", scheduled.commandId, 0)),
            "ready heartbeat node 2");
    require(waitFor([&] {
        const auto status = app.getPlaybackStatus();
        return status.readyNodeCount == 2 && status.missingNodeCount == 0;
    }, std::chrono::milliseconds(1500)), "master ready aggregation");

    require(sendMulticast(options.network.heartbeatMulticastAddress, heartbeatPort,
                          makeHeartbeat(1, "playing", scheduled.commandId,
                                        scheduled.syncTimestamp)),
            "playing heartbeat node 1");
    require(sendMulticast(options.network.heartbeatMulticastAddress, heartbeatPort,
                          makeHeartbeat(2, "playing", scheduled.commandId,
                                        scheduled.syncTimestamp)),
            "playing heartbeat node 2");
    require(waitFor([&] {
        const auto status = app.getPlaybackStatus();
        return status.playingNodeCount == 2 && status.maxStartSkewMs == 0;
    }, std::chrono::milliseconds(1500)), "master playing aggregation");

    require(sendMulticast(options.network.heartbeatMulticastAddress, heartbeatPort,
                          makeHeartbeat(1, "error", scheduled.commandId, 0,
                                        "prepare failed")),
            "error heartbeat node 1");
    require(waitFor([&] {
        const auto status = app.getPlaybackStatus();
        return status.state == "degraded" && status.errorNodeCount == 1;
    }, std::chrono::milliseconds(1500)), "master degraded aggregation");

    app.shutdown();
    std::filesystem::remove_all(cacheDir);
}

void testNodeDiscovery() {
    const std::string group = "239.255.77.78";
    const uint16_t port = static_cast<uint16_t>(32000 + (getpid() % 10000));
    const uint16_t activeDiscoveryPort = static_cast<uint16_t>(port + 1);
    dms::NodeDiscovery discovery;
    require(!discovery.initialize("127.0.0.1", port), "discovery unicast rejection");
    require(!discovery.initialize(group, 0), "discovery zero port rejection");
    require(discovery.initialize(group, port, activeDiscoveryPort, {"127.0.0.1/32"}),
            "node discovery initialization");
    require(discovery.start(), "node discovery start");
    require(!discovery.start(), "node discovery duplicate start rejection");

    require(sendMulticast(group, port, "not-json"), "malformed heartbeat send");
    require(sendMulticast(group, port, R"({"type":"heartbeat","nodeId":0})"), "zero node heartbeat send");
    require(sendMulticast(group, port,
        R"({"type":"heartbeat","nodeId":6,"ip":"10.0.0.6","status":"online","playState":"idle","resolution":"1920x1080","cpu":1.0,"memory":2.0,"timestamp":1})"),
        "heartbeat missing extended fields send");
    require(sendMulticast(group, port,
        R"({"type":"heartbeat","nodeId":8,"ip":"10.0.0.8","status":"online","playState":"idle","resolution":"1920x1080","cpu":1.0,"memory":2.0,"audioOutputMode":"both","masterClockSynchronized":false,"masterClockOffsetMs":0,"commandId":0,"actualStartTimestamp":0,"lastError":"","timestamp":1,"extra":true})"),
        "heartbeat unknown field send");
    require(sendMulticast(group, port,
        R"({"type":"heartbeat","nodeId":9,"ip":"10.0.0.9","status":"online","playState":"idle","resolution":"1920x1080","cpu":1.0,"memory":2.0,"audioOutputMode":"both","masterClockSynchronized":true,"masterClockOffsetMs":1.5,"commandId":0,"actualStartTimestamp":0,"lastError":"","timestamp":1})"),
        "heartbeat non-integer clock offset send");
    auto legacyHeartbeat = nlohmann::json::parse(makeHeartbeat(5, "idle", 0, 0));
    for (const char* field : {"deviceModel", "deviceName", "softwareVersion", "boardInfo",
                              "macAddress", "subnetMask", "gateway", "deviceType"}) {
        legacyHeartbeat.erase(field);
    }
    require(sendMulticast(group, port, legacyHeartbeat.dump()), "legacy heartbeat send");
    require(sendMulticast(group, port,
        R"({"type":"heartbeat","nodeId":7,"nodeRole":"decode","ip":"10.0.0.7","deviceModel":"rk3566-test","deviceName":"node-7","softwareVersion":"1.0.0","boardInfo":"test-board","macAddress":"AA:BB:CC:DD:EE:FF","subnetMask":"255.255.255.0","gateway":"10.0.0.1","deviceType":"output","status":"online","playState":"paused","resolution":"1920x1080","cpu":12.5,"memory":25.0,"audioOutputMode":"analog","masterClockSynchronized":true,"masterClockOffsetMs":-7,"commandId":123,"actualStartTimestamp":456,"lastError":"","kvmEnabled":true,"kvmRole":"both","kvmPort":9101,"kvmSessionId":0,"kvmState":"idle","kvmLastError":"","timestamp":1})"),
        "valid heartbeat send");
    auto activeSourceHeartbeat = nlohmann::json::parse(makeHeartbeat(12, "idle", 0, 0));
    activeSourceHeartbeat["signalSourceType"] = "capture";
    activeSourceHeartbeat["signalSourceConfigured"] = true;
    activeSourceHeartbeat["signalSourceActive"] = true;
    activeSourceHeartbeat["signalSourceEndpoint"] = "/dev/video0";
    activeSourceHeartbeat["signalSourceWidth"] = 1920;
    activeSourceHeartbeat["signalSourceHeight"] = 1080;
    activeSourceHeartbeat["signalSourceFramerateNumerator"] = 60;
    activeSourceHeartbeat["signalSourceFramerateDenominator"] = 1;
    activeSourceHeartbeat["signalSourcePixelFormat"] = "auto";
    require(sendMulticast(group, port, activeSourceHeartbeat.dump()),
            "active signal source heartbeat send");
    auto legacyOutputHeartbeat = nlohmann::json::parse(makeHeartbeat(13, "playing", 0, 0));
    legacyOutputHeartbeat["signalSourceType"] = "none";
    legacyOutputHeartbeat["signalSourceConfigured"] = false;
    legacyOutputHeartbeat["signalSourceActive"] = true;
    legacyOutputHeartbeat["signalSourceEndpoint"] = "";
    legacyOutputHeartbeat["signalSourceWidth"] = 0;
    legacyOutputHeartbeat["signalSourceHeight"] = 0;
    legacyOutputHeartbeat["signalSourceFramerateNumerator"] = 0;
    legacyOutputHeartbeat["signalSourceFramerateDenominator"] = 1;
    legacyOutputHeartbeat["signalSourcePixelFormat"] = "auto";
    require(sendMulticast(group, port, legacyOutputHeartbeat.dump()),
            "legacy output-only active-source heartbeat send");
    require(waitFor([&] {
        const auto nodes = discovery.getNodes();
        const auto current = std::find_if(nodes.begin(), nodes.end(), [](const dms::DiscoveredNode& node) {
            return node.nodeId == 7;
        });
        const auto legacy = std::find_if(nodes.begin(), nodes.end(), [](const dms::DiscoveredNode& node) {
            return node.nodeId == 5;
        });
        const auto active = std::find_if(nodes.begin(), nodes.end(), [](const dms::DiscoveredNode& node) {
            return node.nodeId == 12;
        });
        const auto legacyOutput = std::find_if(nodes.begin(), nodes.end(), [](const dms::DiscoveredNode& node) {
            return node.nodeId == 13;
        });
        return nodes.size() == 4 && current != nodes.end() && legacy != nodes.end() &&
               active != nodes.end() && legacyOutput != nodes.end() &&
               active->signalSourceConfigured && active->signalSourceActive &&
               !legacyOutput->signalSourceConfigured && !legacyOutput->signalSourceActive &&
               current->playState == "paused" && current->nodeRole == dms::NodeRole::DECODE &&
               current->audioOutputMode == "analog" && current->masterClockSynchronized &&
               current->masterClockOffsetMs == -7 && current->commandId == 123 &&
               current->actualStartTimestamp == 456 && current->deviceModel == "rk3566-test" &&
               current->deviceName == "node-7" && current->softwareVersion == "1.0.0" &&
               current->boardInfo == "test-board" && current->macAddress == "AA:BB:CC:DD:EE:FF" &&
               current->subnetMask == "255.255.255.0" && current->gateway == "10.0.0.1" &&
               current->deviceType == "output" && current->lastError.empty() && current->kvmEnabled &&
               current->kvmRole == dms::KvmRole::BOTH && current->kvmPort == 9101 &&
               legacy->deviceModel.empty() && legacy->deviceName.empty() &&
               legacy->softwareVersion.empty() && legacy->boardInfo.empty() &&
               legacy->macAddress.empty() && legacy->subnetMask.empty() &&
               legacy->gateway.empty() && legacy->deviceType.empty();
    }, std::chrono::milliseconds(1500)), "strict node discovery heartbeat processing");

    std::atomic<bool> requestWasValid{false};
    const int responderSocket = socket(AF_INET, SOCK_DGRAM, 0);
    require(responderSocket >= 0, "active discovery responder socket");
    timeval responderTimeout{};
    responderTimeout.tv_sec = 2;
    setsockopt(responderSocket, SOL_SOCKET, SO_RCVTIMEO,
               &responderTimeout, sizeof(responderTimeout));
    sockaddr_in responderAddress{};
    responderAddress.sin_family = AF_INET;
    responderAddress.sin_port = htons(activeDiscoveryPort);
    responderAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(responderSocket, reinterpret_cast<sockaddr*>(&responderAddress),
                 sizeof(responderAddress)) == 0,
            "active discovery responder bind");
    std::thread responder([&] {
        char buffer[1024];
        sockaddr_in sender{};
        socklen_t senderLength = sizeof(sender);
        const ssize_t length = recvfrom(responderSocket, buffer, sizeof(buffer), 0,
                                        reinterpret_cast<sockaddr*>(&sender), &senderLength);
        uint64_t requestId = 0;
        if (length > 0 && dms::parseDiscoveryRequest(
                std::string(buffer, static_cast<size_t>(length)), requestId)) {
            requestWasValid.store(true);
            const std::string heartbeat = makeHeartbeat(11, "idle", 0, 0);
            sendto(responderSocket, heartbeat.data(), heartbeat.size(), 0,
                   reinterpret_cast<sockaddr*>(&sender), senderLength);
        }
        close(responderSocket);
    });
    auto scanFuture = std::async(std::launch::async, [&] { return discovery.discover(300); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const dms::DiscoveryScanResult duplicateScan = discovery.discover(50);
    require(duplicateScan.busy && !duplicateScan.success,
            "concurrent discovery scan rejection");
    const dms::DiscoveryScanResult scan = scanFuture.get();
    responder.join();
    require(requestWasValid.load(), "active discovery request schema");
    require(scan.success && !scan.busy && scan.networks.size() == 1 &&
                scan.networks.front() == "127.0.0.1/32" && scan.addressesProbed == 1 &&
                scan.nodesFound == 1 && scan.totalNodes == 5,
            "active unicast discovery result");
    require(waitFor([&] {
        const auto nodes = discovery.getNodes();
        return std::any_of(nodes.begin(), nodes.end(), [](const dms::DiscoveredNode& node) {
            return node.nodeId == 11;
        });
    }, std::chrono::milliseconds(500)), "active discovery heartbeat registration");

    require(waitFor([&] { return discovery.getNodes().empty(); }, std::chrono::milliseconds(12000)),
            "stale node cleanup");
    const auto stopStarted = std::chrono::steady_clock::now();
    discovery.shutdown();
    require(std::chrono::steady_clock::now() - stopStarted < std::chrono::seconds(2),
            "node discovery shutdown latency");

    dms::NodeDiscovery duplicateIdDiscovery;
    const std::string duplicateGroup = "239.255.77.81";
    require(duplicateIdDiscovery.initialize(duplicateGroup, static_cast<uint16_t>(port + 6),
                                            static_cast<uint16_t>(activeDiscoveryPort + 6)),
            "duplicate node ID discovery initialization");
    require(duplicateIdDiscovery.start(), "duplicate node ID discovery start");
    auto duplicateHeartbeatA = nlohmann::json::parse(makeHeartbeat(1, "idle", 0, 0));
    duplicateHeartbeatA["ip"] = "10.0.1.10";
    duplicateHeartbeatA["macAddress"] = "00:11:22:33:44:10";
    auto duplicateHeartbeatB = nlohmann::json::parse(makeHeartbeat(1, "idle", 0, 0));
    duplicateHeartbeatB["ip"] = "10.0.1.11";
    duplicateHeartbeatB["macAddress"] = "00:11:22:33:44:11";
    auto outOfRangeHeartbeat = nlohmann::json::parse(makeHeartbeat(100, "idle", 0, 0));
    outOfRangeHeartbeat["ip"] = "10.0.1.12";
    outOfRangeHeartbeat["macAddress"] = "00:11:22:33:44:12";
    require(sendMulticast(duplicateGroup, static_cast<uint16_t>(port + 6),
                          duplicateHeartbeatA.dump()), "first duplicate node heartbeat send");
    require(sendMulticast(duplicateGroup, static_cast<uint16_t>(port + 6),
                          duplicateHeartbeatB.dump()), "second duplicate node heartbeat send");
    require(sendMulticast(duplicateGroup, static_cast<uint16_t>(port + 6),
                          outOfRangeHeartbeat.dump()), "out-of-range node heartbeat send");
    require(waitFor([&] {
        const auto nodes = duplicateIdDiscovery.getNodes();
        const auto outOfRange = std::find_if(nodes.begin(), nodes.end(), [](const dms::DiscoveredNode& node) {
            return node.ip == "10.0.1.12";
        });
        std::set<uint32_t> assignedNodeIds;
        const size_t duplicateAdvertisedIds = static_cast<size_t>(std::count_if(
            nodes.begin(), nodes.end(), [](const dms::DiscoveredNode& node) {
                return node.advertisedNodeId == 1;
            }));
        for (const auto& node : nodes) assignedNodeIds.insert(node.nodeId);
        return nodes.size() == 3 && assignedNodeIds.size() == 3 &&
               duplicateAdvertisedIds == 2 &&
               outOfRange != nodes.end() && outOfRange->advertisedNodeId == 100 &&
               outOfRange->nodeId >= 1 && outOfRange->nodeId <= dms::MAX_NODES &&
               std::any_of(nodes.begin(), nodes.end(), [](const dms::DiscoveredNode& node) {
                   return node.ip == "10.0.1.10";
               }) &&
               std::any_of(nodes.begin(), nodes.end(), [](const dms::DiscoveredNode& node) {
                   return node.ip == "10.0.1.11";
               });
    }, std::chrono::milliseconds(1500)), "duplicate node IDs are assigned automatically");
    const auto duplicateNodesBeforeRefresh = duplicateIdDiscovery.getNodes();
    require(sendMulticast(duplicateGroup, static_cast<uint16_t>(port + 6),
                          duplicateHeartbeatA.dump()), "first duplicate node refresh send");
    require(sendMulticast(duplicateGroup, static_cast<uint16_t>(port + 6),
                          duplicateHeartbeatB.dump()), "second duplicate node refresh send");
    require(sendMulticast(duplicateGroup, static_cast<uint16_t>(port + 6),
                          outOfRangeHeartbeat.dump()), "out-of-range node refresh send");
    require(waitFor([&] {
        const auto nodes = duplicateIdDiscovery.getNodes();
        if (nodes.size() != duplicateNodesBeforeRefresh.size()) return false;
        for (const auto& before : duplicateNodesBeforeRefresh) {
            const auto current = std::find_if(nodes.begin(), nodes.end(),
                [&](const dms::DiscoveredNode& value) { return value.ip == before.ip; });
            if (current == nodes.end() || current->nodeId != before.nodeId) return false;
        }
        return true;
    }, std::chrono::milliseconds(1500)), "duplicate node IDs remain stable across heartbeats");
    duplicateIdDiscovery.shutdown();

    dms::NodeDiscovery cidrDiscovery;
    require(cidrDiscovery.initialize(group, static_cast<uint16_t>(port + 2),
                                     static_cast<uint16_t>(activeDiscoveryPort + 2),
                                     {"192.0.2.0/30"}),
            "CIDR discovery initialization");
    require(cidrDiscovery.start(), "CIDR discovery start");
    const dms::DiscoveryScanResult cidrScan = cidrDiscovery.discover(10);
    require(cidrScan.success && cidrScan.addressesProbed == 2 &&
                cidrScan.networks.size() == 1 && cidrScan.networks.front() == "192.0.2.0/30",
            "CIDR network and broadcast address exclusion");
    cidrDiscovery.shutdown();

    dms::NodeDiscovery invalidCidrDiscovery;
    require(invalidCidrDiscovery.initialize(group, static_cast<uint16_t>(port + 4),
                                            static_cast<uint16_t>(activeDiscoveryPort + 4),
                                            {"192.168.0.0/0"}),
            "invalid CIDR discovery initialization");
    require(invalidCidrDiscovery.start(), "invalid CIDR discovery start");
    const dms::DiscoveryScanResult invalidCidrScan = invalidCidrDiscovery.discover(10);
    require(!invalidCidrScan.success && !invalidCidrScan.error.empty(),
            "CIDR /0 rejection");
    invalidCidrDiscovery.shutdown();
}

void testMediaPlayer() {
    dms::MediaPlayerOptions options;
    options.startupTimeoutMs = 250;

    dms::SlaveNodeOptions invalidIpOptions;
    invalidIpOptions.nodeId = 1;
    invalidIpOptions.network.ipMode = dms::IpAssignmentMode::MANUAL;
    invalidIpOptions.network.localIp.clear();
    dms::SlaveNodeApp invalidIpApp;
    require(!invalidIpApp.initialize(invalidIpOptions), "manual IP without address rejection");
    invalidIpOptions.network.localIp = "192.168.2.999";
    require(!invalidIpApp.initialize(invalidIpOptions), "invalid manual IP rejection");
    invalidIpOptions.network.ipMode = dms::IpAssignmentMode::AUTO;
    invalidIpOptions.network.localIp = "192.168.2.102";
    require(!invalidIpApp.initialize(invalidIpOptions), "automatic IP with address rejection");

    dms::MediaPlayer invalid;
    require(!invalid.initialize(0, options), "media player zero node rejection");
    auto invalidOptions = options;
    invalidOptions.startupTimeoutMs = 0;
    require(!invalid.initialize(1, invalidOptions), "zero player timeout rejection");
    invalidOptions = options;
    invalidOptions.connectorId = std::numeric_limits<uint32_t>::max();
    require(!invalid.initialize(1, invalidOptions), "out-of-range connector rejection");
    invalidOptions = options;
    invalidOptions.capture.devicePath.clear();
    require(!invalid.initialize(1, invalidOptions), "empty capture device rejection");
    invalidOptions = options;
    invalidOptions.capture.pixelFormat = "RGB16";
    require(!invalid.initialize(1, invalidOptions), "unsupported capture format rejection");
    invalidOptions = options;
    invalidOptions.capture.width = 1920;
    require(!invalid.initialize(1, invalidOptions), "partial capture dimensions rejection");
    invalidOptions = options;
    invalidOptions.capture.framerateDenominator = 0;
    require(!invalid.initialize(1, invalidOptions), "zero capture frame rate denominator rejection");
    invalidOptions = options;
    invalidOptions.capture.colorimetry = "smpte240m";
    require(!invalid.initialize(1, invalidOptions), "unsupported capture colorimetry rejection");
    invalidOptions = options;
    invalidOptions.capture.audioEnabled = true;
    invalidOptions.capture.audioCaptureDevice.clear();
    require(!invalid.initialize(1, invalidOptions), "empty audio capture device rejection");
    invalidOptions = options;
    invalidOptions.capture.audioEnabled = true;
    invalidOptions.capture.hdmiAudioOutput.device.clear();
    require(!invalid.initialize(1, invalidOptions), "empty HDMI audio output rejection");
    invalidOptions = options;
    invalidOptions.capture.audioEnabled = true;
    invalidOptions.capture.hdmiAudioOutput.volume = 0.0;
    require(!invalid.initialize(1, invalidOptions), "zero HDMI audio volume rejection");

    dms::MediaPlayer player;
    if (!player.initialize(9, options)) {
        const std::string error = player.getLastError();
        require(error.find("GStreamer") != std::string::npos ||
                error.find("plugin") != std::string::npos,
                "missing target media runtime must report a precise error");
        return;
    }

    require(!player.showImage("/path/that/does/not/exist", "192.0.2.9"),
            "missing idle image rejection");
    require(!player.showImage("/path/that/does/not/exist", ""),
            "empty idle image node IP rejection");
    require(!player.playStartupAnimation("/path/that/does/not/exist", 250),
            "missing startup animation rejection");
    require(!player.prepare("/path/that/does/not/exist", 1, 1, {9, 0, 0, 1, 1}),
            "missing media rejection");
    const auto directory = std::filesystem::temp_directory_path() /
                           ("dms-component-test-" + std::to_string(getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto video = directory / "invalid-media.bin";
    std::ofstream(video) << "not media";
    require(!player.prepare(video.string(), 1, 1, {8, 0, 0, 1, 1}),
            "crop node mismatch rejection");
    require(!player.prepare(video.string(), 1, 1, {9, 0, 0, 0, 1}),
            "zero crop width rejection");
    require(!player.prepare(video.string(), 1, 1, {9, 1, 0, 1, 1}),
            "crop outside source bounds rejection");
    require(!player.play(), "play before prepare rejection");
    require(!player.pause(), "pause before play rejection");
    require(!player.resume(), "resume before pause rejection");
    player.stop();
    player.stop();
    require(!player.isPrepared() && !player.isRunning() && !player.isCaptureRunning() &&
            !player.isImageRunning(),
            "idempotent media stop state");
    std::filesystem::remove_all(directory);
}

} // namespace

int main() {
    testTimestampGenerator();
    testSharedLivePreview();
    testCommandTransport();
    testNodeDiscovery();
    testMasterPlaybackAggregation();
    testMediaPlayer();
    std::cout << "component_tests passed\n";
}
