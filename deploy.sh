#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "${ROOT_DIR}/system/firefly-rk356x/build-baseline.env"
BINARY="${DMS_SDK_DIR}/buildroot/output/${DMS_BUILDROOT_OUTPUT}/target/usr/bin/distributed-matrix"
MASTER_IP=${MASTER_IP:-192.168.2.101}
SLAVE_IPS=${SLAVE_IPS:-"192.168.2.102"}
SSH_USER=${SSH_USER:-root}
PIN_DEVICE_IPS=${PIN_DEVICE_IPS:-1}
NETWORK_PREFIX=${NETWORK_PREFIX:-24}
NETWORK_GATEWAY=${NETWORK_GATEWAY:-192.168.2.1}
NETWORK_DNS=${NETWORK_DNS:-223.5.5.5}
DRY_RUN=${DRY_RUN:-0}
RESTART_SERVICE=${RESTART_SERVICE:-1}

DAEMON=/usr/bin/distributed-matrix
CONFIG_DIR=/etc/distributed_matrix
DEFAULTS=/etc/default/distributed-matrix
INIT_SCRIPT=/etc/init.d/S99distributed-matrix
DISPLAY_SCRIPT=/usr/local/sbin/distributed-matrix-display-mode
WEB_DIR=/usr/share/distributed_matrix/web
ASSET_DIR=/usr/share/distributed_matrix/assets
PACKAGE_DIR=${ROOT_DIR}/system/firefly-rk356x/package/distributed-matrix
SSH_OPTIONS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15)
SCP_OPTIONS=(-O "${SSH_OPTIONS[@]}")

usage() {
    cat <<'EOF'
Usage: ./deploy.sh [--dry-run] [--no-restart]

Deploys the active Buildroot installation to 192.168.2.101 and 192.168.2.102
by default. The installed service always runs /usr/bin/distributed-matrix with
configuration under /etc/distributed_matrix.

Environment overrides:
  MASTER_IP, SLAVE_IPS, SSH_USER (binary comes from the canonical ROM build)
  PIN_DEVICE_IPS=0|1 (default 1)
  NETWORK_PREFIX, NETWORK_GATEWAY, NETWORK_DNS
  DRY_RUN=0|1, RESTART_SERVICE=0|1
EOF
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --dry-run) DRY_RUN=1 ;;
        --no-restart) RESTART_SERVICE=0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

for required in \
    "${BINARY}" \
    "${ROOT_DIR}/config/master_config.json" \
    "${ROOT_DIR}/config/slave_config.json" \
    "${ROOT_DIR}/web/index.html" \
    "${ROOT_DIR}/assets/default-idle.png" \
    "${ROOT_DIR}/assets/boot-animation.mp4" \
    "${ROOT_DIR}/scripts/configure-display-mode.sh" \
    "${PACKAGE_DIR}/S99distributed-matrix"; do
    [[ -f "${required}" ]] || { echo "Required deployment file missing: ${required}" >&2; exit 1; }
done

[[ "${PIN_DEVICE_IPS}" == 0 || "${PIN_DEVICE_IPS}" == 1 ]] || {
    echo "PIN_DEVICE_IPS must be 0 or 1" >&2
    exit 2
}
[[ "${RESTART_SERVICE}" == 0 || "${RESTART_SERVICE}" == 1 ]] || {
    echo "RESTART_SERVICE must be 0 or 1" >&2
    exit 2
}
[[ "${NETWORK_PREFIX}" =~ ^[0-9]+$ ]] &&
    (( NETWORK_PREFIX >= 1 && NETWORK_PREFIX <= 32 )) || {
    echo "NETWORK_PREFIX must be between 1 and 32" >&2
    exit 2
}

run() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    if [[ "${DRY_RUN}" != 1 ]]; then
        "$@"
    fi
}

remote() {
    local host=$1
    shift
    run ssh "${SSH_OPTIONS[@]}" "${SSH_USER}@${host}" "$@"
}

copy_to() {
    local source=$1
    local host=$2
    local destination=$3
    run scp "${SCP_OPTIONS[@]}" -r "${source}" "${SSH_USER}@${host}:${destination}"
}

deploy_node() {
    local host=$1
    local node_id=$2
    local service_role=$3
    local node_role=$4
    local stage="/tmp/distributed-matrix-deploy-${node_id}-$$"

    if (( node_id < 1 || node_id > 64 )); then
        echo "Refusing invalid node ID ${node_id} for ${host}" >&2
        return 1
    fi

    echo "Deploying ${host} as node ${node_id} (${service_role}/${node_role})"
    remote "${host}" "rm -rf '${stage}' && mkdir -p '${stage}'"
    copy_to "${BINARY}" "${host}" "${stage}/distributed-matrix"
    copy_to "${ROOT_DIR}/config/master_config.json" "${host}" "${stage}/master_config.json"
    copy_to "${ROOT_DIR}/config/slave_config.json" "${host}" "${stage}/slave_config.json"
    copy_to "${PACKAGE_DIR}/S99distributed-matrix" "${host}" "${stage}/S99distributed-matrix"
    copy_to "${ROOT_DIR}/scripts/configure-display-mode.sh" "${host}" "${stage}/distributed-matrix-display-mode"
    copy_to "${ROOT_DIR}/web" "${host}" "${stage}/web"
    copy_to "${ROOT_DIR}/assets/default-idle.png" "${host}" "${stage}/default-idle.png"
    copy_to "${ROOT_DIR}/assets/boot-animation.mp4" "${host}" "${stage}/boot-animation.mp4"

    local network_update=:
    if [[ "${PIN_DEVICE_IPS}" == 1 ]]; then
        network_update="printf '%s\\n' \
'IP_MODE=manual' \
'IP_ADDRESS=${host}' \
'PREFIX_LENGTH=${NETWORK_PREFIX}' \
'GATEWAY=${NETWORK_GATEWAY}' \
'DNS_SERVERS=${NETWORK_DNS}' > '${CONFIG_DIR}/network.conf.tmp'
mv '${CONFIG_DIR}/network.conf.tmp' '${CONFIG_DIR}/network.conf'"
    fi

    local start_command=:
    if [[ "${RESTART_SERVICE}" == 1 ]]; then
        start_command="if ! '${INIT_SCRIPT}' start; then
  echo 'New service failed; restoring previous runtime files' >&2
  '${INIT_SCRIPT}' stop >/dev/null 2>&1 || true
  [ ! -f \"\$backup/distributed-matrix\" ] || cp -p \"\$backup/distributed-matrix\" '${DAEMON}'
  [ ! -f \"\$backup/S99distributed-matrix\" ] || cp -p \"\$backup/S99distributed-matrix\" '${INIT_SCRIPT}'
  [ ! -f \"\$backup/master_config.json\" ] || cp -p \"\$backup/master_config.json\" '${CONFIG_DIR}/master_config.json'
  [ ! -f \"\$backup/slave_config.json\" ] || cp -p \"\$backup/slave_config.json\" '${CONFIG_DIR}/slave_config.json'
  '${INIT_SCRIPT}' start || true
  exit 1
fi"
    fi

    remote "${host}" "set -eu
chmod 0755 '${stage}/distributed-matrix'
'${stage}/distributed-matrix' --help >/dev/null
mkdir -p '${CONFIG_DIR}' '${WEB_DIR}' '${ASSET_DIR}' /etc/default /usr/local/sbin
backup='${stage}/backup'
mkdir -p \"\$backup\"
[ ! -f '${DAEMON}' ] || cp -p '${DAEMON}' \"\$backup/distributed-matrix\"
[ ! -f '${INIT_SCRIPT}' ] || cp -p '${INIT_SCRIPT}' \"\$backup/S99distributed-matrix\"
[ ! -f '${CONFIG_DIR}/master_config.json' ] || cp -p '${CONFIG_DIR}/master_config.json' \"\$backup/master_config.json\"
[ ! -f '${CONFIG_DIR}/slave_config.json' ] || cp -p '${CONFIG_DIR}/slave_config.json' \"\$backup/slave_config.json\"
[ ! -x '${INIT_SCRIPT}' ] || '${INIT_SCRIPT}' stop
install -m 0755 '${stage}/distributed-matrix' '${DAEMON}'
install -m 0755 '${stage}/S99distributed-matrix' '${INIT_SCRIPT}'
install -m 0755 '${stage}/distributed-matrix-display-mode' '${DISPLAY_SCRIPT}'
install -m 0644 '${stage}/master_config.json' '${CONFIG_DIR}/master_config.json'
install -m 0644 '${stage}/slave_config.json' '${CONFIG_DIR}/slave_config.json'
install -m 0644 '${stage}/default-idle.png' '${ASSET_DIR}/default-idle.png'
install -m 0644 '${stage}/boot-animation.mp4' '${ASSET_DIR}/boot-animation.mp4'
rm -rf '${WEB_DIR}.new'
cp -a '${stage}/web' '${WEB_DIR}.new'
rm -rf '${WEB_DIR}'
mv '${WEB_DIR}.new' '${WEB_DIR}'
printf '%s\\n' '# distributed-matrix service role' 'ROLE=${service_role}' > '${DEFAULTS}.tmp'
mv '${DEFAULTS}.tmp' '${DEFAULTS}'
current_node_id=0
[ ! -r '${CONFIG_DIR}/node_id' ] || current_node_id=\$(cat '${CONFIG_DIR}/node_id' 2>/dev/null || echo 0)
case \"\$current_node_id\" in ''|*[!0-9]*) current_node_id=0 ;; esac
if [ \"\$current_node_id\" -lt 1 ] || [ \"\$current_node_id\" -gt 64 ]; then
  printf '%s\\n' '${node_id}' > '${CONFIG_DIR}/node_id.tmp'
  mv '${CONFIG_DIR}/node_id.tmp' '${CONFIG_DIR}/node_id'
fi
current_node_role=
[ ! -r '${CONFIG_DIR}/node_role' ] || current_node_role=\$(cat '${CONFIG_DIR}/node_role' 2>/dev/null || true)
case \"\$current_node_role\" in unassigned|encode|decode|codec) ;; *)
  printf '%s\\n' '${node_role}' > '${CONFIG_DIR}/node_role.tmp'
  mv '${CONFIG_DIR}/node_role.tmp' '${CONFIG_DIR}/node_role'
;; esac
${network_update}
${start_command}
rm -f /opt/distributed-matrix/start.sh /opt/distributed-matrix/bin/distributed-matrix
rm -rf '${stage}'
sync"

    if [[ "${DRY_RUN}" != 1 && "${RESTART_SERVICE}" == 1 ]]; then
        remote "${host}" "test \"\$(readlink /proc/\$(cat /var/run/distributed-matrix-slave.pid)/exe)\" = '${DAEMON}'
printf 'node_id='; cat '${CONFIG_DIR}/node_id'
printf 'node_role='; cat '${CONFIG_DIR}/node_role'
sha256sum '${DAEMON}'"
    fi
}

deploy_node "${MASTER_IP}" 1 both decode

node_id=2
for slave_ip in ${SLAVE_IPS}; do
    deploy_node "${slave_ip}" "${node_id}" slave encode
    node_id=$((node_id + 1))
done

echo "Deployment completed using ${DAEMON}."
