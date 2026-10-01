# Distributed Matrix System V1.0 API Reference

## 1. Rules

- REST uses one authoritative method/path mapping. No compatibility aliases are exposed.
- JSON objects use strict field validation. Unknown fields, missing required fields, and wrong types are rejected.
- HTTP JSON requests must use `Content-Type: application/json`.
- Master playback is coordinated internally through `PREPARE_PLAY -> COMMIT_PLAY`.
- UDP wire protocol version: `0x00010100` (V1.1 wire format).

Node IP mode is configured in both Master and Slave `network` objects. Use
`{"ip_mode":"auto","local_ip":""}` to select the first usable non-loopback
interface automatically, or `{"ip_mode":"manual","local_ip":"192.168.2.102"}`
to advertise a fixed local IPv4 address. A manual address must be a valid IPv4
address; an automatic configuration must leave `local_ip` empty. The `--ip-mode`
and `--ip` command-line options apply the same rule to either role. This setting
controls the address used by the application for its HTTP URL, heartbeat and
node telemetry. Runtime Slave changes use `/api/node-network`; the node persists
that setting and applies DHCP or a static IPv4 configuration to `eth0`.

## 2. REST API

| Method | Path | Request | Success |
|---|---|---|---|
| GET | `/api/nodes` | none | node discovery, telemetry, and device foundation information |
| POST | `/api/nodes/discover` | `{}` | routed IPv4 unicast discovery scan summary |
| PUT | `/api/node-role` | `{"nodeId":1,"role":"unassigned|encode|decode|codec"}` | persisted role command sent to one node; incompatible changes are rejected while the node is bound to a region |
| PUT | `/api/node-network` | complete node network object | persisted DHCP/static IPv4 command sent to one online node |
| GET | `/api/regions` | none | persisted region bindings and output-wall layouts |
| PUT | `/api/regions` | complete regions document | persisted input/output node bindings and output-wall layouts |
| GET | `/api/screens` | none | currently active output-wall layout and output-node mapping |
| PUT | `/api/screens` | output-wall layout object | `{"success":true}` |
| GET | `/api/windows` | none | current input-window canvas placements |
| PUT | `/api/windows` | `{"windows":[...]}` | updated input-window canvas and routed output layers |
| GET | `/api/status` | none | Master playback transaction and HTTP state |
| GET | `/api/audio-output` | none | current audio output mode |
| PUT | `/api/audio-output` | `{"mode":"hdmi|analog|both"}` | updated audio output mode |
| GET | `/api/audio-volume` | none | current volume percentage |
| PUT | `/api/audio-volume` | `{"volumePercent":0..100}` | updated volume percentage |
| GET | `/api/kvm/status` | none | current exclusive KVM session |
| POST | `/api/kvm/acquire` | target and lease object | acquired KVM session and direct connection parameters |
| POST | `/api/kvm/release` | `{"sessionId":1}` | released KVM session |
| POST | `/api/play` | playback request object | `{"success":true,"status":"scheduled","commandId":42,"syncTimestamp":...}` |
| POST | `/api/pause` | `{}` | `{"success":true,"operation":"pause"}` |
| POST | `/api/resume` | `{}` | `{"success":true,"operation":"resume"}` |
| POST | `/api/stop` | `{}` | `{"success":true,"operation":"stop"}` |
| POST | `/api/preload` | `{"videoUrl":"..."}` | `{"success":true}` |

Unknown paths return `404`; a wrong method on a known path returns `405`.

Each node returned by `GET /api/nodes` includes the basic information reported by the
node heartbeat: `deviceModel`, `deviceName`, `softwareVersion`, `boardInfo`, `macAddress`,
`subnetMask`, `gateway`, and `deviceType`. `deviceType` is `input` for an encoder,
`output` for a decoder, and `input/output` for a codec. These values are collected from
the node's device tree, hostname, installed application version, active network interface,
and routing table; they are not management-page placeholder data. Nodes running an older
binary remain discoverable and return empty values for fields that they cannot report.
When multiple endpoints advertise the same `nodeId` (for example, when they all use the
default Slave configuration), the Master keeps them as separate discovered nodes by
matching the heartbeat IP and assigning the first available numeric ID to each endpoint.

### POST `/api/nodes/discover`

The Flutter connection picker also performs discovery before connecting to a Master.
It reads active interface IPv4 prefixes and sends the existing UDP request
`{"type":"discovery_request","version":1,"requestId":<positive integer>}` to port 9003.
Replies are existing heartbeat objects; the sender address identifies the endpoint.
The picker independently verifies `GET /api/status` and `GET /api/nodes` on HTTP port
8080 and the currently configured port, with at most 64 concurrent probes. A device
is selectable only when its Master service responds with the expected contract.
UDP-only nodes remain visible without a connect action. This adds no REST endpoint
or discovery-protocol alias and does not require calling the following Master API first.

The request body must be exactly `{}`. The Master sends a strict UDP discovery request to port
`9003` of every usable IPv4 address in `network.discovery_networks`. When that list is empty, the
Master uses non-loopback interface networks and non-default routes visible in `/proc/net/route`.
The total scan is limited to 65,536 unique addresses; `/0`, invalid CIDRs, and oversized scans are
rejected. `addressesProbed` is the number of destination addresses attempted; `nodesFound` counts
only nodes that actually replied. A Slave replies by unicast with its existing strict heartbeat object.

```json
{
  "success": true,
  "networks": ["192.168.2.0/24", "192.168.3.0/24"],
  "addressesProbed": 508,
  "nodesFound": 4,
  "totalNodes": 4,
  "elapsedMs": 1500
}
```

Cross-subnet discovery requires a route from the Master to the target subnet, router forwarding,
and UDP `9003` to be allowed by host and network firewalls. It cannot bypass VLAN ACLs or a missing
route. Existing multicast heartbeats on UDP `9002` remain enabled for same-subnet automatic discovery.

## 3. Request formats

### PUT `/api/node-network`

Automatic address assignment:

```json
{"nodeId":1,"mode":"auto","address":"","prefixLength":0,"gateway":"","dnsServers":[]}
```

Manual address assignment (`prefixLength` is the API form of the subnet mask):

```json
{"nodeId":1,"mode":"manual","address":"192.168.2.103","prefixLength":24,"gateway":"192.168.2.1","dnsServers":["223.5.5.5","114.114.114.114"]}
```

All six fields are required. Manual mode accepts prefix lengths `1..32` and one
or two IPv4 DNS servers. Automatic mode requires all static fields to be empty
or zero. The target must be online. A successful response confirms command
delivery and includes `"reconnectRequired":true`. The node ignores an identical
configuration. Changed parameters are persisted to
`/etc/distributed_matrix/network.conf` and reload `eth0`; the distributed-matrix
service is restarted only when `mode` actually changes. The old address may stop
responding immediately.

### GET/PUT `/api/screens`

```json
{"rows":2,"cols":2,"screenWidth":1920,"screenHeight":1080,"placements":[{"nodeId":7,"x":0.5,"y":0,"width":0.5,"height":0.5}]}
```

All four fields are required positive integers and the layout must be accepted by `LayoutCalculator`.
The optional `placements` array uses normalized coordinates relative to the complete display wall. `x` and `y` are the top-left position; `width` and `height` are the occupied size. All four values must remain inside `[0,1]`. `nodeId` identifies an output-capable decoder/codec node. `PUT` replaces the active physical output-wall layout. It does not describe the Flutter input-window canvas.

For live input routing, each selected output node renders the complete input frame. Placement geometry identifies the output-wall window but is never converted into a crop of that input. Synchronized local video playback keeps its separate wall-crop calculation.

### GET/PUT `/api/regions`

```json
{"regions":[{"name":"手术室 A","inputNodeIds":[2,3],"layout":{"rows":1,"cols":2,"screenWidth":1920,"screenHeight":1080},"placements":[{"nodeId":7,"x":0,"y":0,"width":0.5,"height":1},{"nodeId":8,"x":0.5,"y":0,"width":0.5,"height":1}],"savedAt":1787620000000}]}
```

`PUT` replaces the complete region list. Each region owns one physical output-wall `layout`, an `inputNodeIds` list, and its output-node `placements`; these values are selected when the region is created. Node capability follows the configured `nodeRole`: `unassigned` is not available to regions, `encode` is input, `decode` is output, and `codec` supports both for compatibility. `deviceType` is used only as a discovery fallback when a legacy node does not report `nodeRole`. A node may be bound to at most one region and may not appear in both lists of the same region. The Flutter client shows the selected region's input nodes in its left panel and keeps the input-window count/layout as client state. Selecting a region activates that region's output wall through `/api/screens`; input windows are never written as output-node placements.

The Flutter input canvas allows at most 16 independent windows for each input node. Every window renders the node's complete input frame; its placement changes only the window position, size, and stacking priority and never crops the input into wall quadrants. This is a client-side canvas limit and does not change the `/api/screens` output-node placement contract.

### GET/PUT `/api/windows`

`GET` returns `{"windows":[...]}`. `PUT` replaces the complete input canvas and
accepts normalized `x`, `y`, `width`, `height` coordinates plus `windowId`,
`sourceNodeId`, and `zOrder`. Window IDs are unique, and each input node may
have at most 16 windows. The Master intersects these input windows with the
active output-wall screen placements and sends each output node only the local
layers it must render.

### POST `/api/play`

```json
{
  "videoUrl":"/data/video/example.mp4",
  "video":{"width":640,"height":360,"fps":30,"duration":20},
  "delayMs":3000,
  "layout":{"rows":2,"cols":2,"screenWidth":1920,"screenHeight":1080}
}
```

Required top-level fields: `videoUrl`, `video`.
Optional top-level fields: `delayMs`, `layout`, `placements`.
Required `video` fields: `width`, `height`.
Optional `video` fields: `fps`, `duration`.
Video dimensions are `1..65535`; delay is limited to 24 hours.

If `layout` is present, it must contain exactly `rows`, `cols`, `screenWidth`, and `screenHeight`. When `placements` is present, it uses the same normalized placement format as `/api/screens` and is applied together with `layout` for this playback.

### POST `/api/pause`, `/api/resume`, `/api/stop`

The request body must be exactly:

```json
{}
```

### POST `/api/preload`

```json
{"videoUrl":"https://example.invalid/video.mp4"}
```

The request body contains only `videoUrl`, which must not be empty.

### KVM session API

KVM is an optional subsystem and does not share the playback data path. Acquire accepts exactly:

```json
{"targetNodeId":2,"leaseMs":300000}
```

The selected target must be present in discovery and report role `target` or `both`. The Flutter
client is represented by reserved `controllerNodeId: 0`. `leaseMs` is `1000..3600000`.
Only one KVM session may exist at a time. Release accepts exactly `{"sessionId":1}`.

Acquire returns `targetIp`, `targetPort`, and a random `sessionToken` for the direct TCP connection.
The token is never returned by the status endpoint. Target responses retain numeric IDs and add
display labels such as `"targetNodeLabel":"001"`. The target's single read-only preview endpoint
is `http://targetIp:(targetPort + 1)/kvm/preview.jpg`. The control canvas requests its continuous
MJPEG mode with `?stream=1`; omitting `stream` returns one JPEG for diagnostics and compatibility.
KVM requests add `sessionId` and `token`, which are checked against the active
route, target node and lease expiry. The preview port is opened only for an enabled KVM target.
`v` remains an optional client cache-buster. This shared endpoint avoids separate canvas and KVM preview APIs.
Node IDs remain JSON integers in every request, response and configuration file; leading zeros are
presentation only.

## 4. Response formats

### GET `/api/status`

```json
{
  "running": true,
  "state": "scheduled",
  "videoUrl": "http://192.168.1.10:8080/media/example.mp4",
  "commandId": 42,
  "syncTimestamp": 1785900000000,
  "updatedAt": 1785899997000,
  "nodes": {
    "expected": 4,
    "ready": 4,
    "playing": 0,
    "error": 0,
    "missing": 0,
    "maxStartSkewMs": 0
  },
  "http": {
    "activeConnections": 0,
    "totalRequests": 12,
    "totalBytesSent": 123456
  }
}
```

Main `state` values are `idle`, `preparing`, `committing`, `scheduled`, `playing`,
`paused`, `degraded`, `stopped`, and `error`. The public REST paths remain unchanged.
`commandId` is the correlation ID of the active PREPARE transaction; idle is `0`.

`nodes` is a read-only aggregation of heartbeat telemetry for the current `commandId`:
`expected` is the crop map cardinality, `ready` includes ready/paused/playing nodes,
`playing` counts actual playing nodes, `error` counts nodes reporting an error,
`missing` counts expected nodes without a matching current-command report, and
`maxStartSkewMs` is the signed skew with the largest absolute magnitude among reported
`actualStartTimestamp` values. A missing report before the sync timestamp is observable
but does not by itself change `scheduled` to `degraded`; after a partial report is present,
missing or error reports make the transaction `degraded`.

### GET `/api/nodes`

Each node contains:

```json
{
  "nodeId": 1,
  "displayNodeId": "001",
  "nodeRole": "decode",
  "ip": "192.168.1.21",
  "resolution": "1920x1080",
  "status": "online",
  "playState": "READY",
  "cpu": 12.5,
  "memory": 33.0,
  "masterClockSynchronized": true,
  "masterClockOffsetMs": -2,
  "commandId": 42,
  "actualStartTimestamp": 1785900000000,
  "lastError": "",
  "kvmEnabled": true,
  "kvmRole": "both",
  "kvmPort": 9101,
  "kvmSessionId": 0,
  "kvmState": "idle",
  "kvmLastError": "",
  "lastSeen": 1785900001000
}
```

The heartbeat schema has exactly 23 fields. Missing and extra fields are rejected. `nodeRole` is
the device-persisted video role and is independent from the KVM role.

## 5. UDP playback transaction

### PREPARE_PLAY

The UDP packet header `sequenceId` is also the playback transaction `commandId`.

```json
{
  "cmd":"prepare_play",
  "video_url":"http://master/media/example.mp4",
  "video_width":640,
  "video_height":360,
  "sync_timestamp":1785900000000,
  "crops":[{"node_id":1,"x":0,"y":0,"width":320,"height":180}]
}
```

A Slave enters `PREPARING`, caches the video, and calls `MediaPlayer::prepare()`.
After preparation it enters `READY`, but it must not start playback until the matching COMMIT is received.

### COMMIT_PLAY

```json
{"cmd":"commit_play","command_id":42}
```

A Slave accepts only the matching `command_id`, then starts within the `sync_timestamp` start window.
A commit that arrives while preparation is still running is retained and applied after `READY`.

### STOP / PAUSE / RESUME

These commands use strict one-field payloads, for example:

```json
{"cmd":"stop"}
```

Each logical packet is sent three times. The Slave tracks the most recent 128 packet identities (`commandType + sequenceId + timestamp`) and suppresses retransmission duplicates.

## 6. KVM control and data paths

The Flutter client displays the takeover action only for a selected canvas layer. The Master
arbitrates the exclusive lease and broadcasts strict `KVM_ROUTE` and `KVM_RELEASE` packets over the
existing UDP multicast channel. HID reports never pass through the Master. The client connects
directly to the selected target Agent over TCP port `9101`; the target writes 8-byte keyboard and
4-byte relative mouse reports to `/dev/hidg0` and `/dev/hidg1`.

Each route has a random 64-bit token and an expiring lease. This V1 transport is intended for a
controlled, trusted LAN. It does not provide mTLS, encrypted HID traffic, or Internet-safe access.
The product kernel and USB Gadget configuration must expose both HID functions for target mode.
Playback-node configuration enables KVM with role `target`; physical input devices are not read on
the node because the Flutter client is the only controller in this design.
KVM V1.0 carries real keyboard, mouse, and touch-derived relative pointer input. The target's
active signal pipeline also branches to `jpegenc -> appsink`; the adjacent preview port streams
those frames as multipart MJPEG. The Flutter client keeps one connection open and retains the last
decoded frame while reconnecting, avoiding blank flashes between frames.

The TCP wire header is exactly 28 bytes in network byte order: magic `KVM1`, version, frame type,
session ID, sequence, 64-bit session token, and payload length. Frame types are one-to-one across
the Agent and Flutter client: `HELLO=1`, `KEYBOARD=2`, `MOUSE=3`, `ACK=4`, and `PING=5`.
After receiving `HELLO`, the target validates the active route and opens both HID Gadget devices.
Only then may it return `ACK` and enter `active`. A controller must not report takeover success
until that ACK is validated. The Flutter controller sends `PING` once per second; the target echoes
an `ACK` with the same sequence, and the client closes a channel that has no ACK for four seconds.

## 7. V1.0 scope and limits

Implemented in V1.0: two-phase playback transaction, strict protocol parsing, CRC32 validation, retransmission deduplication, Master command status, Slave command/actual-start/error telemetry, strict node discovery API, and exclusive trusted-LAN KVM sessions.

Not claimed as complete in V1.0: UDP ACK/retry feedback loop, HMAC identity authentication, Master HA/election, PTP hardware clock discipline, hard frame lock, and multi-Master conflict arbitration.
