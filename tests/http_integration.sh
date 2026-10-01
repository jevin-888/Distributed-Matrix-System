#!/usr/bin/env bash
set -euo pipefail
binary="${1:?binary required}"
web_root="${2:?web root required}"
port="${DMS_TEST_PORT:-18080}"
cache="$(mktemp -d)"
log="$(mktemp)"
config="$cache/master.json"
pid=""
cleanup() {
  if [[ -n "$pid" ]]; then kill -TERM "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
  rm -rf -- "$cache"
  rm -f -- "$log"
}
trap cleanup EXIT
printf '0123456789' >"$cache/sample.bin"
printf '{"network":{"discovery_port":19003,"discovery_networks":["127.0.0.1/32"]}}\n' >"$config"
"$binary" --role master --config "$config" --port "$port" --cache-dir "$cache" --web-root "$web_root" >"$log" 2>&1 &
pid=$!
for _ in $(seq 1 50); do
  if curl -fsS "http://127.0.0.1:$port/api/status" >/dev/null; then break; fi
  sleep 0.1
done
if ! kill -0 "$pid" 2>/dev/null; then cat "$log" >&2; exit 1; fi
status_code() { curl --path-as-is -sS -o /dev/null -w '%{http_code}' "$@"; }

status="$(curl -fsS "http://127.0.0.1:$port/api/status")"
[[ "$status" == *'"running":true'* ]]
[[ "$(curl -fsS "http://127.0.0.1:$port/api/nodes")" == *'"nodes":[]'* ]]
discovery="$(curl -fsS -X POST -H 'Content-Type: application/json' -d '{}' "http://127.0.0.1:$port/api/nodes/discover")"
[[ "$discovery" == *'"success":true'* && "$discovery" == *'"addressesProbed":1'* ]]
[[ "$(status_code "http://127.0.0.1:$port/api/nodes/discover")" == 405 ]]
[[ "$(status_code -X POST -H 'Content-Type: application/json' -d '{"extra":true}' "http://127.0.0.1:$port/api/nodes/discover")" == 400 ]]
[[ "$(curl -fsS "http://127.0.0.1:$port/api/regions")" == *'"regions":[]'* ]]
screens="$(curl -fsS "http://127.0.0.1:$port/api/screens")"
[[ "$screens" == *'"rows":2'* && "$screens" == *'"nodeId":1'* && "$screens" == *'"nodeId":4'* ]]
[[ "$screens" == *'"placements":[]'* ]]
custom_screens='{"rows":2,"cols":2,"screenWidth":1920,"screenHeight":1080,"placements":[{"nodeId":7,"x":0.5,"y":0,"width":0.5,"height":0.5},{"nodeId":3,"x":0,"y":0.5,"width":0.5,"height":0.5}]}'
curl -fsS -X PUT -H 'Content-Type: application/json' -d "$custom_screens" "http://127.0.0.1:$port/api/screens" | grep -q '"success":true'
custom_saved="$(curl -fsS "http://127.0.0.1:$port/api/screens")"
python3 - "$custom_saved" <<'PY'
import json
import sys

placements = {item["nodeId"]: item for item in json.loads(sys.argv[1])["placements"]}
assert placements[7] == {"nodeId": 7, "x": 0.5, "y": 0.0, "width": 0.5, "height": 0.5}
assert placements[3] == {"nodeId": 3, "x": 0.0, "y": 0.5, "width": 0.5, "height": 0.5}
PY
[[ "$(curl -fsS "http://127.0.0.1:$port/api/audio-output")" == *'"mode":"both"'* ]]
[[ "$(curl -fsS "http://127.0.0.1:$port/api/audio-volume")" == *'"volumePercent":100'* ]]
[[ "$(curl -fsS "http://127.0.0.1:$port/api/kvm/status")" == *'"state":"idle"'* ]]
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d '{}' "http://127.0.0.1:$port/api/kvm/status")" == 405 ]]
[[ "$(status_code -X POST -H 'Content-Type: application/json' -d '{}' "http://127.0.0.1:$port/api/node-network")" == 405 ]]
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d '{}' "http://127.0.0.1:$port/api/node-network")" == 400 ]]
[[ "$(status_code -X POST -H 'Content-Type: application/json' -d '{"targetNodeId":2,"leaseMs":300000,"extra":true}' "http://127.0.0.1:$port/api/kvm/acquire")" == 400 ]]
[[ "$(status_code -X POST -H 'Content-Type: application/json' -d '{"sessionId":0}' "http://127.0.0.1:$port/api/kvm/release")" == 400 ]]
curl -fsS "http://127.0.0.1:$port/" | grep -q 'id="app"'
[[ "$(status_code -I "http://127.0.0.1:$port/")" == 200 ]]
[[ "$(curl -fsS "http://127.0.0.1:$port/media/sample.bin")" == '0123456789' ]]
[[ "$(curl -fsS -H 'Range: bytes=0-3' "http://127.0.0.1:$port/media/sample.bin")" == '0123' ]]
[[ "$(curl -fsS -H 'Range: bytes=3-' "http://127.0.0.1:$port/media/sample.bin")" == '3456789' ]]
[[ "$(curl -fsS -H 'Range: bytes=-3' "http://127.0.0.1:$port/media/sample.bin")" == '789' ]]
[[ "$(curl -fsS -H 'Range: bytes=3-99' "http://127.0.0.1:$port/media/sample.bin")" == '3456789' ]]
[[ "$(status_code -H 'Range: bytes=99-' "http://127.0.0.1:$port/media/sample.bin")" == 416 ]]
[[ "$(status_code -H 'Range: bytes=0-1,3-4' "http://127.0.0.1:$port/media/sample.bin")" == 416 ]]
[[ "$(status_code "http://127.0.0.1:$port/../config/master_config.json")" == 404 ]]
[[ "$(status_code "http://127.0.0.1:$port/%2e%2e/config/master_config.json")" == 404 ]]
[[ "$(status_code "http://127.0.0.1:$port/%ZZ")" == 404 ]]
[[ "$(status_code "http://127.0.0.1:$port/%00")" == 404 ]]
[[ "$(status_code "http://127.0.0.1:$port/api/play")" == 405 ]]
[[ "$(status_code -X POST -H 'Content-Type: application/json' -d '{"mode":"hdmi"}' "http://127.0.0.1:$port/api/audio-output")" == 405 ]]
[[ "$(status_code -X POST -H 'Content-Type: text/plain' -d '{}' "http://127.0.0.1:$port/api/preload")" == 415 ]]
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d bad "http://127.0.0.1:$port/api/screens")" == 400 ]]
[[ "$(status_code -X POST -H 'Content-Type: application/json' -d '{"videoUrl":""}' "http://127.0.0.1:$port/api/preload")" == 400 ]]
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d '{"rows":1,"cols":2,"screenWidth":1280,"screenHeight":720,"extra":true}' "http://127.0.0.1:$port/api/screens")" == 400 ]]
[[ "$(status_code -X POST -H 'Content-Type: application/json' -d '{"videoUrl":"x","video":{"width":1,"height":1},"extra":true}' "http://127.0.0.1:$port/api/play")" == 400 ]]
[[ "$(status_code -X POST -H 'Content-Type: application/json' -d '{"extra":true}' "http://127.0.0.1:$port/api/pause")" == 400 ]]
[[ "$(status_code -X POST -H 'Content-Type: application/json' -d '{"videoUrl":"x","extra":true}' "http://127.0.0.1:$port/api/preload")" == 400 ]]
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d '{"mode":"invalid"}' "http://127.0.0.1:$port/api/audio-output")" == 400 ]]
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d '{"mode":"hdmi","extra":true}' "http://127.0.0.1:$port/api/audio-output")" == 400 ]]
[[ "$(status_code -X POST -H 'Content-Type: application/json' -d '{}' "http://127.0.0.1:$port/api/audio-volume")" == 405 ]]
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d '{"volumePercent":101}' "http://127.0.0.1:$port/api/audio-volume")" == 400 ]]
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d '{"volumePercent":1.5}' "http://127.0.0.1:$port/api/audio-volume")" == 400 ]]
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d '{"volumePercent":72,"extra":true}' "http://127.0.0.1:$port/api/audio-volume")" == 400 ]]
curl -fsS -X PUT -H 'Content-Type: application/json' -d '{"volumePercent":72}' "http://127.0.0.1:$port/api/audio-volume" | grep -q '"volumePercent":72'
[[ "$(curl -fsS "http://127.0.0.1:$port/api/audio-volume")" == *'"volumePercent":72'* ]]
[[ "$(status_code -X POST -H 'Content-Type: application/json' -d '{"regions":[]}' "http://127.0.0.1:$port/api/regions")" == 405 ]]
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d '{"regions":[{"name":"A","inputNodeIds":[],"layout":{"rows":1,"cols":1,"screenWidth":1920,"screenHeight":1080},"placements":[{"nodeId":1,"x":90,"y":0,"width":20,"height":100}],"savedAt":1}]}' "http://127.0.0.1:$port/api/regions")" == 400 ]]
regions_payload='{"regions":[{"name":"手术室 A","inputNodeIds":[],"layout":{"rows":1,"cols":2,"screenWidth":1920,"screenHeight":1080},"placements":[{"nodeId":7,"x":0,"y":0,"width":1,"height":1}],"savedAt":1}]}'
curl -fsS -X PUT -H 'Content-Type: application/json' -d "$regions_payload" "http://127.0.0.1:$port/api/regions" | grep -q '"name":"手术室 A"'
[[ "$(curl -fsS "http://127.0.0.1:$port/api/regions")" == *'"nodeId":7'* ]]
duplicate_region_nodes='{"regions":[{"name":"A","inputNodeIds":[11],"layout":{"rows":1,"cols":1,"screenWidth":1920,"screenHeight":1080},"placements":[],"savedAt":1},{"name":"B","inputNodeIds":[11],"layout":{"rows":1,"cols":1,"screenWidth":1920,"screenHeight":1080},"placements":[],"savedAt":1}]}'
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d "$duplicate_region_nodes" "http://127.0.0.1:$port/api/regions")" == 400 ]]
duplicate_region_outputs='{"regions":[{"name":"A","inputNodeIds":[],"layout":{"rows":1,"cols":1,"screenWidth":1920,"screenHeight":1080},"placements":[{"nodeId":11,"x":0,"y":0,"width":1,"height":1}],"savedAt":1},{"name":"B","inputNodeIds":[],"layout":{"rows":1,"cols":1,"screenWidth":1920,"screenHeight":1080},"placements":[{"nodeId":11,"x":0,"y":0,"width":1,"height":1}],"savedAt":1}]}'
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d "$duplicate_region_outputs" "http://127.0.0.1:$port/api/regions")" == 400 ]]
overlapping_region_binding='{"regions":[{"name":"A","inputNodeIds":[11],"layout":{"rows":1,"cols":1,"screenWidth":1920,"screenHeight":1080},"placements":[{"nodeId":11,"x":0,"y":0,"width":1,"height":1}],"savedAt":1}]}'
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d "$overlapping_region_binding" "http://127.0.0.1:$port/api/regions")" == 400 ]]
curl -fsS -X PUT -H 'Content-Type: application/json' -d '{"mode":"hdmi"}' "http://127.0.0.1:$port/api/audio-output" | grep -q '"mode":"hdmi"'
[[ "$(curl -fsS "http://127.0.0.1:$port/api/audio-output")" == *'"mode":"hdmi"'* ]]
curl -fsS -X PUT -H 'Content-Type: application/json' -d '{"mode":"analog"}' "http://127.0.0.1:$port/api/audio-output" | grep -q '"mode":"analog"'
curl -fsS -X PUT -H 'Content-Type: application/json' -d '{"mode":"both"}' "http://127.0.0.1:$port/api/audio-output" | grep -q '"mode":"both"'
curl -fsS -X PUT -H 'Content-Type: application/json' -d '{"rows":1,"cols":2,"screenWidth":1280,"screenHeight":720}' "http://127.0.0.1:$port/api/screens" | grep -q '"success":true'
updated="$(curl -fsS "http://127.0.0.1:$port/api/screens")"
[[ "$updated" == *'"nodeId":1'* && "$updated" == *'"nodeId":2'* ]]
[[ "$updated" == *'"placements":[]'* ]]

python3 - "$port" <<'PY'
import socket,sys,time,json
port=int(sys.argv[1])
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
s.sendto(b'{bad',('127.0.0.1',9002))
s.sendto(json.dumps({'type':'heartbeat','nodeId':0}).encode(),('127.0.0.1',9002))
s.sendto(json.dumps({'type':'heartbeat','nodeId':6,'ip':'10.0.0.6','status':'online','playState':'idle','resolution':'1920x1080','cpu':1.0,'memory':2.0,'timestamp':1}).encode(),('127.0.0.1',9002))
s.sendto(json.dumps({'type':'heartbeat','nodeId':8,'ip':'10.0.0.8','status':'online','playState':'idle','resolution':'1920x1080','cpu':1.0,'memory':2.0,'audioOutputMode':'both','masterClockSynchronized':False,'masterClockOffsetMs':0,'commandId':0,'actualStartTimestamp':0,'lastError':'','timestamp':1,'extra':True}).encode(),('127.0.0.1',9002))
s.sendto(json.dumps({'type':'heartbeat','nodeId':9,'ip':'10.0.0.9','status':'online','playState':'idle','resolution':'1920x1080','cpu':1.0,'memory':2.0,'audioOutputMode':'both','masterClockSynchronized':True,'masterClockOffsetMs':1.5,'commandId':0,'actualStartTimestamp':0,'lastError':'','timestamp':1}).encode(),('127.0.0.1',9002))
def heartbeat(node_id, role, state='idle'):
    return {'type':'heartbeat','nodeId':node_id,'nodeRole':'decode','ipMode':'manual' if node_id == 7 else 'auto','ip':f'10.0.0.{node_id}','status':'online',
            'deviceModel':'test-device','deviceName':f'test-node-{node_id}','softwareVersion':'1.0.0',
            'boardInfo':'test-board','macAddress':f'00:11:22:33:44:{node_id:02d}','subnetMask':'255.255.255.0',
            'gateway':'10.0.0.1','deviceType':'output',
            'playState':'paused' if node_id == 7 else 'idle','resolution':'1920x1080',
            'cpu':12.5,'memory':33.0,'audioOutputMode':'analog' if node_id == 7 else 'both',
            'masterClockSynchronized':True,'masterClockOffsetMs':-7 if node_id == 7 else 0,
            'commandId':123 if node_id == 7 else 0,'actualStartTimestamp':456 if node_id == 7 else 0,
            'lastError':'','kvmEnabled':True,'kvmRole':role,'kvmPort':9101,
            'kvmSessionId':0,'kvmState':state,'kvmLastError':'',
            'signalSourceType':'capture','signalSourceConfigured':True,
            'signalSourceActive':node_id == 10,'signalSourceEndpoint':'/dev/video0',
            'signalSourceWidth':1920,'signalSourceHeight':1080,
            'signalSourceFramerateNumerator':60,'signalSourceFramerateDenominator':1,
            'signalSourcePixelFormat':'auto','timestamp':1}
s.sendto(json.dumps(heartbeat(7, 'controller')).encode(),('127.0.0.1',9002))
s.sendto(json.dumps(heartbeat(10, 'target')).encode(),('127.0.0.1',9002))
time.sleep(.2)
PY
nodes="$(curl -fsS "http://127.0.0.1:$port/api/nodes")"
[[ "$nodes" == *'"nodeId":7'* && "$nodes" == *'"displayNodeId":"007"'* && "$nodes" == *'"ipMode":"manual"'* && "$nodes" == *'"deviceModel":"test-device"'* && "$nodes" == *'"deviceName":"test-node-7"'* && "$nodes" == *'"softwareVersion":"1.0.0"'* && "$nodes" == *'"boardInfo":"test-board"'* && "$nodes" == *'"macAddress":"00:11:22:33:44:07"'* && "$nodes" == *'"subnetMask":"255.255.255.0"'* && "$nodes" == *'"gateway":"10.0.0.1"'* && "$nodes" == *'"deviceType":"output"'* && "$nodes" == *'"nodeId":10'* && "$nodes" == *'"signalSourceActive":true'* && "$nodes" == *'"playState":"paused"'* && "$nodes" == *'"audioOutputMode":"analog"'* && "$nodes" == *'"masterClockSynchronized":true'* && "$nodes" == *'"masterClockOffsetMs":-7'* && "$nodes" != *'"nodeId":0'* && "$nodes" != *'"nodeId":6'* && "$nodes" != *'"nodeId":8'* && "$nodes" != *'"nodeId":9'* ]]
wrong_region_role='{"regions":[{"name":"A","inputNodeIds":[7],"layout":{"rows":1,"cols":1,"screenWidth":1920,"screenHeight":1080},"placements":[],"savedAt":1}]}'
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d "$wrong_region_role" "http://127.0.0.1:$port/api/regions")" == 400 ]]
curl -fsS -X PUT -H 'Content-Type: application/json' -d '{"nodeId":7,"role":"decode"}' "http://127.0.0.1:$port/api/node-role" | grep -q '"success":true'
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d '{"nodeId":7,"role":"unassigned"}' "http://127.0.0.1:$port/api/node-role")" == 409 ]]
curl -fsS -X PUT -H 'Content-Type: application/json' -d '{"nodeId":10,"role":"unassigned"}' "http://127.0.0.1:$port/api/node-role" | grep -q '"role":"unassigned"'
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d '{"nodeId":7,"role":"bad"}' "http://127.0.0.1:$port/api/node-role")" == 400 ]]
network_payload='{"nodeId":7,"mode":"manual","address":"192.168.2.103","prefixLength":24,"gateway":"192.168.2.1","dnsServers":["223.5.5.5","114.114.114.114"]}'
curl -fsS -X PUT -H 'Content-Type: application/json' -d "$network_payload" "http://127.0.0.1:$port/api/node-network" | grep -q '"reconnectRequired":true'
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d '{"nodeId":7,"mode":"auto","address":"192.168.2.103","prefixLength":0,"gateway":"","dnsServers":[]}' "http://127.0.0.1:$port/api/node-network")" == 400 ]]
[[ "$(status_code -X PUT -H 'Content-Type: application/json' -d '{"nodeId":7,"mode":"manual","address":"192.168.2.999","prefixLength":24,"gateway":"192.168.2.1","dnsServers":["223.5.5.5"]}' "http://127.0.0.1:$port/api/node-network")" == 400 ]]
[[ "$nodes" != *'"id"'* && "$nodes" != *'"name"'* ]]

kvm_session="$(curl -fsS -X POST -H 'Content-Type: application/json' -d '{"targetNodeId":10,"leaseMs":300000}' "http://127.0.0.1:$port/api/kvm/acquire")"
[[ "$kvm_session" == *'"controllerNodeId":0'* && "$kvm_session" == *'"controllerNodeLabel":""'* && "$kvm_session" == *'"targetNodeLabel":"010"'* && "$kvm_session" == *'"state":"acquired"'* && "$kvm_session" == *'"targetIp":"10.0.0.10"'* && "$kvm_session" == *'"targetPort":9101'* && "$kvm_session" == *'"sessionToken":'* ]]
session_id="$(printf '%s' "$kvm_session" | python3 -c 'import json,sys; print(json.load(sys.stdin)["sessionId"])')"
[[ "$(status_code -X POST -H 'Content-Type: application/json' -d "{\"targetNodeId\":10,\"leaseMs\":300000}" "http://127.0.0.1:$port/api/kvm/acquire")" == 409 ]]
curl -fsS -X POST -H 'Content-Type: application/json' -d "{\"sessionId\":$session_id}" "http://127.0.0.1:$port/api/kvm/release" | grep -q '"success":true'

python3 - "$port" <<'PY'
import socket,sys
port=int(sys.argv[1])
def request(raw):
    sock=socket.create_connection(('127.0.0.1',port),2)
    sock.sendall(raw)
    data=b''
    while True:
        chunk=sock.recv(4096)
        if not chunk: break
        data+=chunk
    sock.close()
    return int(data.split(b' ',2)[1])
base=b'POST /api/preload HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n'
assert request(base+b'Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}') == 400
assert request(base+b'Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n') == 400
assert request(b'get / HTTP/1.1\r\nHost: localhost\r\n\r\n') == 400
PY

kill -TERM "$pid"
wait "$pid"
pid=""
echo 'http_integration passed'
