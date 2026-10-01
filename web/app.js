'use strict';

const API = Object.freeze({
  nodes: { method: 'GET', path: '/api/nodes' },
  setNodeRole: { method: 'PUT', path: '/api/node-role' },
  setNodeNetwork: { method: 'PUT', path: '/api/node-network' },
  setNodeSource: { method: 'PUT', path: '/api/node-source' },
  discoverNodes: { method: 'POST', path: '/api/nodes/discover' },
  regions: { method: 'GET', path: '/api/regions' },
  setRegions: { method: 'PUT', path: '/api/regions' },
  screens: { method: 'GET', path: '/api/screens' },
  setScreens: { method: 'PUT', path: '/api/screens' },
  audioOutput: { method: 'GET', path: '/api/audio-output' },
  setAudioOutput: { method: 'PUT', path: '/api/audio-output' },
  audioVolume: { method: 'GET', path: '/api/audio-volume' },
  setAudioVolume: { method: 'PUT', path: '/api/audio-volume' },
  status: { method: 'GET', path: '/api/status' }
});

const PAGE_META = Object.freeze({
  devices: { kicker: '设备与节点', title: '设备管理', description: '发现、查看并监控分布式显示节点' },
  inputs: { kicker: '节点分类', title: '输入管理', description: '定义输入节点并管理输入信号属性' },
  outputs: { kicker: '节点分类', title: '输出管理', description: '定义输出节点并查看输出显示属性' },
  zones: { kicker: '节点与输出墙区域', title: '区域管理', description: '管理区域输入/输出节点绑定和输出墙布局' },
  cameras: { kicker: '信号与媒体', title: '摄像头管理', description: '管理摄像头信号源与接入状态' },
  users: { kicker: '访问控制', title: '用户管理', description: '管理后台用户、角色与权限' },
  data: { kicker: '数据维护', title: '数据管理', description: '管理配置数据、导入导出与备份' },
  status: { kicker: '实时监控', title: '设备状态', description: '查看主节点与分布式节点运行指标' },
  settings: { kicker: '系统维护', title: '系统设置', description: '查看主节点连接参数与开放接口' },
  logs: { kicker: '系统维护', title: '系统日志', description: '查看管理端请求与运行记录' }
});

const appState = {
  nodes: [],
  regions: [],
  layout: null,
  status: null,
  audioOutput: null,
  audioVolume: null,
  selectedNodeId: null,
  assignmentSelection: { encode: null, decode: null },
  refreshing: false,
  initialized: false
};

const byId = id => document.getElementById(id);
const numberValue = id => Number(byId(id).value);
const text = value => String(value ?? '--');

function formatNodeId(value) {
  const id = Number(value);
  return Number.isInteger(id) && id > 0 ? String(id).padStart(3, '0') : '---';
}

const NODE_ROLES = Object.freeze({
  unassigned: '未分配',
  encode: '输入',
  decode: '输出',
  codec: '输入/输出'
});
const NODE_PROFILE_STORAGE_KEY = 'distributed-matrix.node-profiles.v1';
const NODE_RESOLUTION_PRESETS = Object.freeze([
  { value: '1280x720', width: 1280, height: 720, label: '1280 × 720' },
  { value: '1920x1080', width: 1920, height: 1080, label: '1920 × 1080' },
  { value: '2560x1440', width: 2560, height: 1440, label: '2560 × 1440' },
  { value: '3840x2160', width: 3840, height: 2160, label: '3840 × 2160' },
  { value: '4096x2160', width: 4096, height: 2160, label: '4096 × 2160' }
]);

function readNodeProfiles() {
  try {
    const profiles = JSON.parse(localStorage.getItem(NODE_PROFILE_STORAGE_KEY) || '{}');
    return profiles && typeof profiles === 'object' && !Array.isArray(profiles) ? profiles : {};
  } catch {
    return {};
  }
}

function writeNodeProfiles(profiles) {
  localStorage.setItem(NODE_PROFILE_STORAGE_KEY, JSON.stringify(profiles));
}

function parseNodeResolution(value) {
  const match = String(value || '').match(/(\d{3,5})\s*[x×*]\s*(\d{3,5})/i);
  return match ? { width: Number(match[1]), height: Number(match[2]) } : null;
}

function nodeProfile(node) {
  const stored = readNodeProfiles()[String(node.nodeId)] || {};
  const source = parseNodeResolution(node.resolution) || { width: 1920, height: 1080 };
  const width = Number(stored.width) || source.width;
  const height = Number(stored.height) || source.height;
  const role = Object.hasOwn(NODE_ROLES, node.nodeRole) ? node.nodeRole : 'unassigned';
  const reportedName = String(node.deviceName || '').trim();
  const matchingPreset = NODE_RESOLUTION_PRESETS.find(item => item.width === width && item.height === height);
  const storedModeIsValid = stored.resolutionMode === 'custom'
    || NODE_RESOLUTION_PRESETS.some(item => item.value === stored.resolutionMode);

  return {
    name: String(stored.name || ((!reportedName || reportedName.toLowerCase() === 'unknown')
      ? `节点 ${formatNodeId(node.nodeId)}`
      : reportedName)).trim() || `节点 ${formatNodeId(node.nodeId)}`,
    role,
    width,
    height,
    resolutionMode: storedModeIsValid ? stored.resolutionMode : (matchingPreset?.value || 'custom')
  };
}

function nodeDisplayName(node) {
  return nodeProfile(node).name;
}

function nodeDisplayResolution(node) {
  const profile = nodeProfile(node);
  return `${profile.width} × ${profile.height}`;
}

function nodeRoleLabel(role) {
  return NODE_ROLES[role] || NODE_ROLES.unassigned;
}

function nodeFoundationValue(node, field) {
  const value = String(node?.[field] || '').trim();
  return escapeHtml(!value || value.toLowerCase() === 'unknown' ? '--' : value);
}

function nodeDeviceTypeLabel(value) {
  const labels = { input: 'input', output: 'output', 'input/output': 'input/output' };
  return labels[String(value || '').toLowerCase()] || '--';
}

function isIpv4(value) {
  const parts = String(value || '').trim().split('.');
  return parts.length === 4 && parts.every(part => /^\d{1,3}$/.test(part) && Number(part) <= 255);
}

function subnetMaskToPrefixLength(value) {
  if (!isIpv4(value)) return null;
  const octets = String(value).trim().split('.').map(Number);
  const bits = octets.map(octet => octet.toString(2).padStart(8, '0')).join('');
  if (!/^1*0*$/.test(bits)) return null;
  const prefixLength = bits.indexOf('0');
  const length = prefixLength === -1 ? 32 : prefixLength;
  return length >= 1 ? length : null;
}

function escapeHtml(value) {
  return text(value).replace(/[&<>'"]/g, character => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;'
  })[character]);
}

function formatDate(value) {
  if (value === null || value === undefined || value === '') return '--';
  const date = new Date(value);
  return Number.isNaN(date.getTime())
    ? '--'
    : date.toLocaleString('zh-CN', { hour12: false });
}

async function requestApi(name, payload) {
  const spec = API[name];
  if (!spec) throw new Error(`未定义的 API 操作：${name}`);

  const init = {
    method: spec.method,
    headers: { Accept: 'application/json' }
  };

  if (spec.method === 'POST' || spec.method === 'PUT') {
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(payload);
  }

  const response = await fetch(spec.path, init);
  const result = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(result.error || `${spec.method} ${spec.path} 返回 HTTP ${response.status}`);
  }
  return result;
}

function addLog(message, isError = false) {
  const list = byId('logList');
  const item = document.createElement('li');
  item.className = isError ? 'error' : '';
  item.textContent = `[${new Date().toLocaleTimeString('zh-CN', { hour12: false })}] ${message}`;
  list.prepend(item);
  while (list.children.length > 200) list.lastElementChild.remove();
}

function toast(message, type = 'success') {
  const item = document.createElement('div');
  item.className = `toast ${type}`;
  item.textContent = message;
  byId('toastStack').append(item);
  setTimeout(() => item.remove(), 3600);
}

function preparePageActions() {
  const storage = byId('pageActionStorage');
  document.querySelectorAll('.page').forEach(page => {
    const actionBar = page.querySelector(':scope > .page-actions');
    if (!actionBar) return;

    Array.from(actionBar.children)
      .filter(child => !child.classList.contains('section-intro'))
      .forEach(control => {
        control.dataset.pageActionsFor = page.id.replace('page-', '');
        storage.append(control);
      });

    if (!actionBar.querySelector('.section-intro')) actionBar.remove();
  });
}

function showPageActions(pageName) {
  const slot = byId('topbarPageActions');
  const storage = byId('pageActionStorage');
  Array.from(slot.children).forEach(control => storage.append(control));
  storage.querySelectorAll(`[data-page-actions-for="${pageName}"]`)
    .forEach(control => slot.append(control));
  slot.hidden = slot.children.length === 0;
}

function openPage(pageName, updateHash = true) {
  const target = byId(`page-${pageName}`) ? pageName : 'devices';
  const meta = PAGE_META[target];

  document.querySelectorAll('.page').forEach(page => {
    page.classList.toggle('active', page.id === `page-${target}`);
  });
  document.querySelectorAll('.nav-item[data-page]').forEach(button => {
    button.classList.toggle('active', button.dataset.page === target);
  });

  byId('pageKicker').textContent = meta.kicker;
  byId('pageTitle').textContent = meta.title;
  byId('pageDescription').textContent = meta.description;
  showPageActions(target);
  document.title = `${meta.title} · 分布式矩阵系统`;
  document.body.classList.remove('nav-open');

  if (target === 'zones') renderZoneManagementPage();
  if (target === 'inputs' || target === 'outputs') renderNodeAssignments();

  if (updateHash && location.hash !== `#${target}`) {
    history.replaceState(null, '', `#${target}`);
  }
}

function loadBar(value) {
  const safeValue = Math.max(0, Math.min(100, Number(value) || 0));
  return `<div class="load-bar"><i style="width:${safeValue}%"></i></div>`;
}

function renderDevices() {
  const body = byId('deviceRows');
  const detailPanel = byId('deviceSummary');
  body.replaceChildren();

  if (!appState.nodes.length) {
    body.innerHTML = '<tr><td colspan="6" class="empty-cell">没有发现运行 Slave 服务的节点</td></tr>';
    renderDeviceDetail(null);
    return;
  }

  if (!appState.nodes.some(node => node.nodeId === appState.selectedNodeId)) {
    appState.selectedNodeId = appState.nodes[0].nodeId;
    detailPanel.dataset.dirty = 'false';
  }

  appState.nodes.forEach(node => {
    const profile = nodeProfile(node);
    const row = document.createElement('tr');
    const isOnline = node.status === 'online';
    const cpu = Number(node.cpu) || 0;
    const memory = Number(node.memory) || 0;
    row.classList.toggle('selected', node.nodeId === appState.selectedNodeId);
    row.tabIndex = 0;
    row.innerHTML = `
      <td><div class="device-cell"><span class="device-mini-icon">${escapeHtml(formatNodeId(node.nodeId))}</span><div><strong>${escapeHtml(profile.name)}</strong><small>ID ${escapeHtml(formatNodeId(node.nodeId))} · CPU ${escapeHtml(cpu)}% · 内存 ${escapeHtml(memory)}%</small></div></div></td>
      <td>${escapeHtml(node.ip)}</td>
      <td><span class="node-role-badge ${escapeHtml(profile.role)}">${escapeHtml(nodeRoleLabel(profile.role))}</span></td>
      <td>${escapeHtml(nodeDisplayResolution(node))}</td>
      <td>${escapeHtml(node.playState)}</td>
      <td><span class="status-badge ${isOnline ? 'online' : 'offline'}">${escapeHtml(node.status)}</span></td>`;

    const selectRow = () => {
      appState.selectedNodeId = node.nodeId;
      detailPanel.dataset.dirty = 'false';
      renderDevices();
    };
    row.addEventListener('click', selectRow);
    row.addEventListener('keydown', event => {
      if (event.key === 'Enter' || event.key === ' ') {
        event.preventDefault();
        selectRow();
      }
    });
    body.append(row);
  });

  if (detailPanel.dataset.dirty !== 'true' && !detailPanel.contains(document.activeElement)) {
    renderDeviceDetail(appState.nodes.find(node => node.nodeId === appState.selectedNodeId));
  }
}

function renderDeviceDetail(node) {
  const panel = byId('deviceSummary');
  if (!node) {
    panel.className = 'device-summary empty';
    panel.dataset.dirty = 'false';
    panel.innerHTML = '<div class="empty-detail"><span>01</span><h4>选择一个设备</h4><p>设备的网络、分辨率及运行状态会显示在这里。</p></div>';
    return;
  }

  const profile = nodeProfile(node);
  const isOnline = node.status === 'online';
  const networkInputDisabled = node.ipMode === 'manual' ? '' : ' disabled';
  const sourceType = node.signalSourceType || 'none';
  const sourceConfigured = node.signalSourceConfigured === true;
  const sourceEndpoint = sourceConfigured ? (node.signalSourceEndpoint || '') : '';
  const sourceWidth = Number(node.signalSourceWidth) || 0;
  const sourceHeight = Number(node.signalSourceHeight) || 0;
  const sourceFramerateNumerator = Number(node.signalSourceFramerateNumerator) || 0;
  const sourceFramerateDenominator = Number(node.signalSourceFramerateDenominator) || 1;
  const sourceFramerateLabel = sourceFramerateNumerator === 0
    ? '自动匹配输入信号'
    : `${sourceFramerateNumerator}/${sourceFramerateDenominator} fps`;
  const sourcePixelFormat = node.signalSourcePixelFormat || 'auto';
  const resolutionOptions = NODE_RESOLUTION_PRESETS
    .map(item => `<option value="${item.value}"${profile.resolutionMode === item.value ? ' selected' : ''}>${item.label}</option>`)
    .join('');
  panel.className = 'device-summary';
  panel.dataset.dirty = 'false';
  panel.innerHTML = `
    <div class="device-detail-head">
      <div class="device-avatar">${escapeHtml(formatNodeId(node.nodeId))}</div>
      <div><h2>${escapeHtml(profile.name)}</h2><p>${escapeHtml(node.ip)} · ID ${escapeHtml(formatNodeId(node.nodeId))}</p></div>
    </div>
    <section class="node-foundation" aria-labelledby="nodeFoundationTitle">
      <div class="node-foundation-title"><strong id="nodeFoundationTitle">设备基础信息</strong></div>
      <dl class="detail-list foundation-list">
        <dt>设备型号</dt><dd>${nodeFoundationValue(node, 'deviceModel')}</dd>
        <dt>设备名称</dt><dd>${nodeFoundationValue(node, 'deviceName')}</dd>
        <dt>软件信息</dt><dd>${nodeFoundationValue(node, 'softwareVersion')}</dd>
        <dt>主板信息</dt><dd>${nodeFoundationValue(node, 'boardInfo')}</dd>
        <dt>DHCP</dt><dd>${node.ipMode === 'manual' ? '禁用' : '启用'}</dd>
        <dt>设备 IP</dt><dd>${escapeHtml(node.ip || '--')}</dd>
        <dt>子网掩码</dt><dd>${nodeFoundationValue(node, 'subnetMask')}</dd>
        <dt>网关</dt><dd>${nodeFoundationValue(node, 'gateway')}</dd>
        <dt>MAC</dt><dd>${nodeFoundationValue(node, 'macAddress')}</dd>
        <dt>设备状态</dt><dd><span class="status-badge ${isOnline ? 'online' : 'offline'}">${isOnline ? '在线' : '离线'}</span></dd>
        <dt>设备类型</dt><dd>${escapeHtml(nodeDeviceTypeLabel(node.deviceType))}</dd>
      </dl>
    </section>
    <form class="node-profile-form" id="nodeProfileForm">
      <div class="node-profile-title"><strong>节点显示信息</strong><span>节点分类请在输入或输出管理中设置</span></div>
      <label class="node-profile-field full"><span>节点名称</span><input id="nodeEditName" type="text" maxlength="40" value="${escapeHtml(profile.name)}" autocomplete="off"></label>
      <label class="node-profile-field full"><span>显示分辨率</span><select id="nodeResolutionPreset">${resolutionOptions}<option value="custom"${profile.resolutionMode === 'custom' ? ' selected' : ''}>自定义</option></select></label>
      <div class="node-custom-resolution full${profile.resolutionMode === 'custom' ? '' : ' hidden'}" id="nodeCustomResolution">
        <label class="node-profile-field"><span>自定义宽度</span><input id="nodeCustomWidth" type="number" min="1280" max="4096" step="1" value="${escapeHtml(profile.width)}"><small>1280–4096 px</small></label>
        <span class="resolution-times">×</span>
        <label class="node-profile-field"><span>自定义高度</span><input id="nodeCustomHeight" type="number" min="720" max="4096" step="1" value="${escapeHtml(profile.height)}"><small>720–4096 px</small></label>
      </div>
      <button class="button button-primary node-profile-save full" type="submit">保存节点信息</button>
    </form>
    <form class="node-source-form" id="nodeSourceForm">
      <div class="node-profile-title"><strong>信号来源</strong><span>${sourceConfigured ? '已配置' : '未配置时输出默认背景'}</span></div>
      <label class="node-profile-field full"><span>信号类型</span><select id="nodeSourceType"><option value="none"${sourceType === 'none' ? ' selected' : ''}>无信号源</option><option value="capture"${sourceType === 'capture' ? ' selected' : ''}>HDMI / V4L2 采集</option><option value="stream"${sourceType === 'stream' ? ' selected' : ''}>流媒体</option><option value="network_camera"${sourceType === 'network_camera' ? ' selected' : ''}>网络摄像头</option></select></label>
      <div class="node-source-fields full" id="nodeSourceFields">
        <label class="node-profile-field full"><span id="nodeSourceEndpointLabel">设备或流地址</span><input id="nodeSourceEndpoint" type="text" value="${escapeHtml(sourceEndpoint)}" autocomplete="off"></label>
        <label class="node-profile-field"><span>信号宽度</span><input id="nodeSourceWidth" type="number" min="0" max="65535" value="${escapeHtml(sourceWidth)}"></label>
        <label class="node-profile-field"><span>信号高度</span><input id="nodeSourceHeight" type="number" min="0" max="65535" value="${escapeHtml(sourceHeight)}"></label>
        <div class="node-profile-field full"><span>帧率</span><output class="node-source-framerate">${escapeHtml(sourceFramerateLabel)}</output></div>
        <label class="node-profile-field full"><span>像素格式</span><select id="nodeSourcePixelFormat"><option value="auto"${sourcePixelFormat === 'auto' ? ' selected' : ''}>自动</option><option value="NV61"${sourcePixelFormat === 'NV61' ? ' selected' : ''}>NV61</option><option value="NV16"${sourcePixelFormat === 'NV16' ? ' selected' : ''}>NV16</option><option value="NV12"${sourcePixelFormat === 'NV12' ? ' selected' : ''}>NV12</option><option value="YUY2"${sourcePixelFormat === 'YUY2' ? ' selected' : ''}>YUY2</option></select></label>
      </div>
      <button class="button button-secondary node-source-save full" type="submit">保存信号来源</button>
    </form>
    <form class="node-network-form" id="nodeNetworkForm">
      <div class="node-profile-title"><strong>网络配置</strong><span>自动获取或手动指定 eth0 IPv4</span></div>
      <fieldset class="node-network-mode-field full">
        <legend>IP 分配方式</legend>
        <div class="node-network-modes">
          <label class="node-network-mode-option${node.ipMode !== 'manual' ? ' selected' : ''}">
            <input type="radio" name="nodeNetworkMode" value="auto"${node.ipMode !== 'manual' ? ' checked' : ''}>
            <span class="node-network-mode-check" aria-hidden="true"></span>
            <span><strong>自动获取</strong><small>DHCP</small></span>
          </label>
          <label class="node-network-mode-option${node.ipMode === 'manual' ? ' selected' : ''}">
            <input type="radio" name="nodeNetworkMode" value="manual"${node.ipMode === 'manual' ? ' checked' : ''}>
            <span class="node-network-mode-check" aria-hidden="true"></span>
            <span><strong>手动设置</strong><small>固定 IPv4</small></span>
          </label>
        </div>
        <p class="node-network-mode-note" id="nodeNetworkModeNote"></p>
      </fieldset>
      <div class="node-network-fields full" id="nodeNetworkFields">
        <label class="node-profile-field"><span>IPv4 地址</span><input id="nodeNetworkAddress" type="text" inputmode="decimal" placeholder="192.168.2.103" value="${escapeHtml(node.ip || '')}" autocomplete="off"${networkInputDisabled}></label>
        <label class="node-profile-field"><span>子网掩码</span><input id="nodeNetworkPrefix" type="text" inputmode="decimal" placeholder="255.255.255.0" value="${node.ipMode === 'manual' ? escapeHtml(node.subnetMask || '') : ''}" autocomplete="off"${networkInputDisabled}></label>
        <label class="node-profile-field"><span>网关</span><input id="nodeNetworkGateway" type="text" inputmode="decimal" placeholder="192.168.2.1" value="${node.ipMode === 'manual' ? escapeHtml(node.gateway || '') : ''}" autocomplete="off"${networkInputDisabled}></label>
        <label class="node-profile-field"><span>DNS 服务器 1</span><input id="nodeNetworkDns1" type="text" inputmode="decimal" placeholder="223.5.5.5" value="" autocomplete="off"${networkInputDisabled}></label>
        <label class="node-profile-field"><span>DNS 服务器 2（可选）</span><input id="nodeNetworkDns2" type="text" inputmode="decimal" placeholder="114.114.114.114" value="" autocomplete="off"${networkInputDisabled}></label>
      </div>
      <button class="button button-secondary node-network-save full" type="submit">应用网络设置</button>
    </form>
    <div class="node-runtime-title"><strong>运行状态</strong><span>来自节点实时上报，只读</span></div>
    <dl class="detail-list node-runtime-list">
      <dt>连接状态</dt><dd><span class="status-badge ${isOnline ? 'online' : 'offline'}">${escapeHtml(node.status)}</span></dd>
      <dt>播放状态</dt><dd>${escapeHtml(node.playState)}</dd>
      <dt>当前输出</dt><dd>${escapeHtml(nodeDisplayResolution(node))}</dd>
      <dt>CPU 使用率</dt><dd>${escapeHtml(node.cpu)}%</dd>
      <dt>内存使用率</dt><dd>${escapeHtml(node.memory)}%</dd>
      <dt>主时钟</dt><dd>${node.masterClockSynchronized ? '已同步' : '未同步'}</dd>
      <dt>时钟偏移</dt><dd>${escapeHtml(node.masterClockOffsetMs)} ms</dd>
      <dt>最后心跳</dt><dd>${escapeHtml(formatDate(node.lastSeen))}</dd>
    </dl>`;

  const form = byId('nodeProfileForm');
  const preset = byId('nodeResolutionPreset');
  const customFields = byId('nodeCustomResolution');
  const markDirty = () => { panel.dataset.dirty = 'true'; };
  form.addEventListener('input', markDirty);
  form.addEventListener('change', markDirty);
  preset.addEventListener('change', () => {
    const selectedPreset = NODE_RESOLUTION_PRESETS.find(item => item.value === preset.value);
    customFields.classList.toggle('hidden', preset.value !== 'custom');
    if (selectedPreset) {
      byId('nodeCustomWidth').value = String(selectedPreset.width);
      byId('nodeCustomHeight').value = String(selectedPreset.height);
    }
  });
  form.addEventListener('submit', async event => {
    event.preventDefault();
    const name = byId('nodeEditName').value.trim();
    const mode = preset.value;
    const selectedPreset = NODE_RESOLUTION_PRESETS.find(item => item.value === mode);
    const width = selectedPreset?.width ?? Number(byId('nodeCustomWidth').value);
    const height = selectedPreset?.height ?? Number(byId('nodeCustomHeight').value);

    if (!name) {
      toast('节点名称不能为空', 'error');
      byId('nodeEditName').focus();
      return;
    }
    if (!Number.isInteger(width) || width < 1280 || width > 4096) {
      toast('自定义宽度必须是 1280–4096 的整数', 'error');
      byId('nodeCustomWidth').focus();
      return;
    }
    if (!Number.isInteger(height) || height < 720 || height > 4096) {
      toast('自定义高度必须是 720–4096 的整数', 'error');
      byId('nodeCustomHeight').focus();
      return;
    }

    try {
      const profiles = readNodeProfiles();
      profiles[String(node.nodeId)] = {
        name,
        resolutionMode: mode,
        width,
        height,
        updatedAt: Date.now()
      };
      writeNodeProfiles(profiles);
      panel.dataset.dirty = 'false';
      renderDevices();
      renderStatusTable();
      if (!byId('zoneModal').hidden && zoneDraft) {
        renderZoneNodes();
      }
      renderNodeAssignments();
      addLog(`节点 ${formatNodeId(node.nodeId)} 显示信息已更新：${name} · ${width}×${height}`);
      toast('节点信息已保存');
    } catch (error) {
      addLog(`节点 ${formatNodeId(node.nodeId)} 属性保存失败：${error.message}`, true);
      toast(error.message, 'error');
    }
  });

  const sourceForm = byId('nodeSourceForm');
  const sourceTypeInput = byId('nodeSourceType');
  const sourceFields = byId('nodeSourceFields');
  const sourceInputs = Array.from(sourceFields.querySelectorAll('input, select'));
  const sourceEndpointInput = byId('nodeSourceEndpoint');
  const sourceEndpointByType = Object.create(null);
  sourceEndpointByType[sourceTypeInput.value] = sourceEndpointInput.value.trim();
  const updateSourceType = () => {
    const type = sourceTypeInput.value;
    const previousType = sourceTypeInput.dataset.previousType;
    if (previousType && previousType !== type) {
      sourceEndpointByType[previousType] = sourceEndpointInput.value.trim();
      sourceEndpointInput.value = sourceEndpointByType[type] || '';
    }
    sourceTypeInput.dataset.previousType = type;
    const disabled = type === 'none';
    sourceFields.classList.toggle('disabled', disabled);
    sourceInputs.forEach(input => { input.disabled = disabled; });
    byId('nodeSourceEndpointLabel').textContent = type === 'capture' ? 'V4L2 设备' : '网络流地址';
    sourceEndpointInput.placeholder = type === 'capture' ? '/dev/video0' : 'rtsp://...';
    if (type !== 'capture') byId('nodeSourcePixelFormat').value = 'auto';
  };
  sourceForm.addEventListener('input', markDirty);
  sourceForm.addEventListener('change', markDirty);
  sourceTypeInput.addEventListener('change', updateSourceType);
  updateSourceType();
  sourceForm.addEventListener('submit', async event => {
    event.preventDefault();
    const type = sourceTypeInput.value;
    const payload = type === 'none'
      ? { nodeId: node.nodeId, sourceType: 'none', endpoint: '', width: 0, height: 0,
          framerateNumerator: 0, framerateDenominator: 1, pixelFormat: 'auto' }
      : { nodeId: node.nodeId, sourceType: type,
          endpoint: byId('nodeSourceEndpoint').value.trim(),
          width: Number(byId('nodeSourceWidth').value),
          height: Number(byId('nodeSourceHeight').value),
          framerateNumerator: 0,
          framerateDenominator: 1,
          pixelFormat: type === 'capture' ? byId('nodeSourcePixelFormat').value : 'auto' };
    if (type !== 'none' && !payload.endpoint) {
      toast(type === 'capture' ? '请填写 V4L2 设备路径' : '请填写网络流地址', 'error');
      byId('nodeSourceEndpoint').focus();
      return;
    }
    if ((type === 'stream' || type === 'network_camera') &&
        !/^(?:rtsp|https?|udp|tcp):\/\/\S+$/.test(payload.endpoint)) {
      toast('网络流地址必须以 rtsp://、http://、https://、udp:// 或 tcp:// 开头', 'error');
      byId('nodeSourceEndpoint').focus();
      return;
    }
    if (!Number.isInteger(payload.width) || !Number.isInteger(payload.height) ||
        (payload.width === 0) !== (payload.height === 0) ||
        payload.width < 0 || payload.width > 65535 ||
        payload.height < 0 || payload.height > 65535) {
      toast('信号宽高必须同时为 0，或填写 1–65535 的整数', 'error');
      return;
    }
    try {
      await requestApi('setNodeSource', payload);
      Object.assign(node, {
        signalSourceType: payload.sourceType,
        signalSourceConfigured: payload.sourceType !== 'none',
        signalSourceEndpoint: payload.endpoint,
        signalSourceWidth: payload.width,
        signalSourceHeight: payload.height,
        signalSourceFramerateNumerator: payload.framerateNumerator,
        signalSourceFramerateDenominator: payload.framerateDenominator,
        signalSourcePixelFormat: payload.pixelFormat
      });
      panel.dataset.dirty = 'false';
      addLog(`节点 ${formatNodeId(node.nodeId)} 信号来源已更新：${payload.sourceType}`);
      toast(payload.sourceType === 'none' ? '已切换为默认背景' : '信号来源已保存');
      renderDevices();
    } catch (error) {
      addLog(`节点 ${formatNodeId(node.nodeId)} 信号来源保存失败：${error.message}`, true);
      toast(error.message, 'error');
    }
  });

  const networkForm = byId('nodeNetworkForm');
  const networkModes = Array.from(networkForm.querySelectorAll('input[name="nodeNetworkMode"]'));
  const networkInputs = [
    byId('nodeNetworkAddress'), byId('nodeNetworkPrefix'), byId('nodeNetworkGateway'),
    byId('nodeNetworkDns1'), byId('nodeNetworkDns2')
  ];
  const networkModeNote = byId('nodeNetworkModeNote');
  const selectedNetworkMode = () => networkModes.find(input => input.checked)?.value || 'auto';
  const readNetworkPayload = () => {
    const mode = selectedNetworkMode();
    if (mode === 'auto') {
      return { nodeId: node.nodeId, mode, address: '', prefixLength: 0, gateway: '', dnsServers: [] };
    }
    return {
      nodeId: node.nodeId,
      mode,
      address: byId('nodeNetworkAddress').value.trim(),
      prefixLength: subnetMaskToPrefixLength(byId('nodeNetworkPrefix').value),
      gateway: byId('nodeNetworkGateway').value.trim(),
      dnsServers: [byId('nodeNetworkDns1').value.trim(), byId('nodeNetworkDns2').value.trim()]
        .filter(Boolean)
    };
  };
  networkForm.addEventListener('input', markDirty);
  networkForm.addEventListener('change', markDirty);
  const updateNetworkMode = () => {
    const manual = selectedNetworkMode() === 'manual';
    networkModeNote.textContent = manual
      ? '保存后将应用固定 IPv4、子网掩码、网关和 DNS。'
      : '自动获取模式下网络参数仅供查看；切换到手动设置后可编辑。';
    networkInputs.forEach(input => {
      input.disabled = !manual;
      if (manual) input.removeAttribute('disabled');
      else input.setAttribute('disabled', '');
    });
    networkModes.forEach(input => {
      input.closest('.node-network-mode-option')?.classList.toggle('selected', input.checked);
    });
  };
  networkModes.forEach(input => input.addEventListener('change', updateNetworkMode));
  updateNetworkMode();
  let savedNetworkPayload = JSON.stringify(readNetworkPayload());
  networkForm.addEventListener('submit', async event => {
    event.preventDefault();
    const payload = readNetworkPayload();
    if (JSON.stringify(payload) === savedNetworkPayload) {
      toast('网络设置没有修改');
      return;
    }
    if (payload.mode === 'manual' && (!isIpv4(payload.address) ||
        !Number.isInteger(payload.prefixLength) || payload.prefixLength < 1 ||
        payload.prefixLength > 32 || !isIpv4(payload.gateway) ||
        payload.dnsServers.length === 0 || payload.dnsServers.some(item => !isIpv4(item)))) {
      toast('请填写有效的 IPv4、子网掩码、网关和至少一个 DNS', 'error');
      return;
    }
    const modeChanged = payload.mode !== (node.ipMode === 'manual' ? 'manual' : 'auto');
    try {
      await requestApi('setNodeNetwork', payload);
      node.ipMode = payload.mode;
      savedNetworkPayload = JSON.stringify(payload);
      if (modeChanged) {
        addLog(`节点 ${formatNodeId(node.nodeId)} 已切换为${payload.mode === 'auto' ? '自动获取' : '手动'}模式，播放服务将重启`);
        toast('IP 分配方式已切换，节点重新联网后请使用新 IP 连接');
      } else {
        addLog(`节点 ${formatNodeId(node.nodeId)} 的手动网络参数已更新`);
        toast('网络参数已保存');
      }
    } catch (error) {
      addLog(`节点 ${formatNodeId(node.nodeId)} 网络设置失败：${error.message}`, true);
      toast(error.message, 'error');
    }
  });
}

const NODE_ASSIGNMENT_PAGES = Object.freeze({
  encode: {
    role: 'encode',
    kind: '输入',
    prefix: 'input',
    unassignedListId: 'inputUnassignedNodes',
    unassignedCountId: 'inputUnassignedCount',
    assignedListId: 'inputAssignedNodes',
    assignedCountId: 'inputAssignedCount',
    propertyId: 'inputNodeProperties'
  },
  decode: {
    role: 'decode',
    kind: '输出',
    prefix: 'output',
    unassignedListId: 'outputUnassignedNodes',
    unassignedCountId: 'outputUnassignedCount',
    assignedListId: 'outputAssignedNodes',
    assignedCountId: 'outputAssignedCount',
    propertyId: 'outputNodeProperties'
  }
});

function regionBindingForNode(nodeId) {
  const numericId = Number(nodeId);
  for (const region of readSavedRegions()) {
    if (region.inputNodeIds.includes(numericId)) return { name: region.name, kind: '输入' };
    if (region.outputNodeIds.includes(numericId)) return { name: region.name, kind: '输出' };
  }
  return null;
}

async function setNodeAssignmentRole(node, role) {
  const binding = regionBindingForNode(node.nodeId);
  if (role === 'unassigned' && binding) {
    throw new Error(`节点已绑定到区域“${binding.name}”，请先解除区域绑定`);
  }

  await requestApi('setNodeRole', { nodeId: Number(node.nodeId), role });
  node.nodeRole = role;
  if (role === 'unassigned') {
    appState.assignmentSelection.encode = appState.assignmentSelection.encode === node.nodeId
      ? null : appState.assignmentSelection.encode;
    appState.assignmentSelection.decode = appState.assignmentSelection.decode === node.nodeId
      ? null : appState.assignmentSelection.decode;
  } else {
    appState.assignmentSelection[role] = node.nodeId;
  }
  renderDevices();
  renderStatusTable();
  renderNodeAssignments();
  if (!byId('zoneModal').hidden && zoneDraft) renderZoneNodes();
  const action = role === 'unassigned' ? '取消分类' : `分配为${nodeRoleLabel(role)}节点`;
  addLog(`节点 ${formatNodeId(node.nodeId)} 已${action}`);
  toast(`节点 ${formatNodeId(node.nodeId)} 已${action}`);
}

function assignmentNodeCard(node, config, assigned) {
  const button = document.createElement('button');
  const selected = assigned && appState.assignmentSelection[config.role] === node.nodeId;
  const binding = regionBindingForNode(node.nodeId);
  button.type = 'button';
  button.className = `node-assignment-card${selected ? ' selected' : ''}`;
  button.innerHTML = `
    <span class="assignment-node-avatar">${escapeHtml(formatNodeId(node.nodeId))}</span>
    <span class="assignment-node-copy">
      <strong>${escapeHtml(nodeDisplayName(node))}</strong>
      <small>${escapeHtml(node.ip || '--')} · ${node.status === 'online' ? '在线' : '离线'}</small>
      ${assigned && binding ? `<em>已绑定：${escapeHtml(binding.name)}（${binding.kind}）</em>` : ''}
    </span>
    <span class="assignment-node-action">${assigned ? '查看' : `分配为${config.kind}`}</span>`;

  button.addEventListener('click', async () => {
    if (assigned) {
      appState.assignmentSelection[config.role] = node.nodeId;
      byId(config.propertyId).dataset.dirty = 'false';
      renderNodeAssignments();
      return;
    }
    button.disabled = true;
    try {
      await setNodeAssignmentRole(node, config.role);
    } catch (error) {
      addLog(error.message, true);
      toast(error.message, 'error');
    } finally {
      button.disabled = false;
    }
  });
  return button;
}

function renderInputSourceProperties(container, node) {
  const sourceType = node.signalSourceType || 'none';
  const sourceConfigured = node.signalSourceConfigured === true;
  const sourceEndpoint = sourceConfigured ? (node.signalSourceEndpoint || '') : '';
  const sourceWidth = Number(node.signalSourceWidth) || 0;
  const sourceHeight = Number(node.signalSourceHeight) || 0;
  const sourceNumerator = Number(node.signalSourceFramerateNumerator) || 0;
  const sourceDenominator = Number(node.signalSourceFramerateDenominator) || 1;
  const sourceFramerate = sourceNumerator === 0 ? '自动匹配输入信号' : `${sourceNumerator}/${sourceDenominator} fps`;
  const sourcePixelFormat = node.signalSourcePixelFormat || 'auto';

  container.insertAdjacentHTML('beforeend', `
    <form class="node-source-form assignment-source-form">
      <div class="node-profile-title"><strong>输入信号属性</strong><span>${sourceConfigured ? '已配置' : '未配置'}</span></div>
      <label class="node-profile-field full"><span>信号类型</span><select name="sourceType"><option value="none"${sourceType === 'none' ? ' selected' : ''}>无信号源</option><option value="capture"${sourceType === 'capture' ? ' selected' : ''}>HDMI / V4L2 采集</option><option value="stream"${sourceType === 'stream' ? ' selected' : ''}>流媒体</option><option value="network_camera"${sourceType === 'network_camera' ? ' selected' : ''}>网络摄像头</option></select></label>
      <div class="node-source-fields full" data-source-fields>
        <label class="node-profile-field full"><span data-endpoint-label>设备或流地址</span><input name="endpoint" type="text" value="${escapeHtml(sourceEndpoint)}" autocomplete="off"></label>
        <label class="node-profile-field"><span>信号宽度</span><input name="width" type="number" min="0" max="65535" value="${escapeHtml(sourceWidth)}"></label>
        <label class="node-profile-field"><span>信号高度</span><input name="height" type="number" min="0" max="65535" value="${escapeHtml(sourceHeight)}"></label>
        <div class="node-profile-field full"><span>帧率</span><output class="node-source-framerate">${escapeHtml(sourceFramerate)}</output></div>
        <label class="node-profile-field full"><span>像素格式</span><select name="pixelFormat"><option value="auto"${sourcePixelFormat === 'auto' ? ' selected' : ''}>自动</option><option value="NV61"${sourcePixelFormat === 'NV61' ? ' selected' : ''}>NV61</option><option value="NV16"${sourcePixelFormat === 'NV16' ? ' selected' : ''}>NV16</option><option value="NV12"${sourcePixelFormat === 'NV12' ? ' selected' : ''}>NV12</option><option value="YUY2"${sourcePixelFormat === 'YUY2' ? ' selected' : ''}>YUY2</option></select></label>
      </div>
      <button class="button button-primary node-source-save full" type="submit">保存输入属性</button>
    </form>`);

  const form = container.querySelector('.assignment-source-form');
  const typeInput = form.elements.sourceType;
  const endpointInput = form.elements.endpoint;
  const pixelFormatInput = form.elements.pixelFormat;
  const sourceFields = form.querySelector('[data-source-fields]');
  const sourceInputs = Array.from(sourceFields.querySelectorAll('input, select'));
  const updateSourceType = () => {
    const type = typeInput.value;
    const disabled = type === 'none';
    sourceFields.classList.toggle('disabled', disabled);
    sourceInputs.forEach(input => { input.disabled = disabled; });
    form.querySelector('[data-endpoint-label]').textContent = type === 'capture' ? 'V4L2 设备' : '网络流地址';
    endpointInput.placeholder = type === 'capture' ? '/dev/video0' : 'rtsp://...';
    if (type !== 'capture') pixelFormatInput.value = 'auto';
  };
  form.addEventListener('input', () => { container.dataset.dirty = 'true'; });
  form.addEventListener('change', () => { container.dataset.dirty = 'true'; });
  typeInput.addEventListener('change', updateSourceType);
  updateSourceType();

  form.addEventListener('submit', async event => {
    event.preventDefault();
    const type = typeInput.value;
    const payload = type === 'none'
      ? { nodeId: node.nodeId, sourceType: 'none', endpoint: '', width: 0, height: 0,
          framerateNumerator: 0, framerateDenominator: 1, pixelFormat: 'auto' }
      : { nodeId: node.nodeId, sourceType: type, endpoint: endpointInput.value.trim(),
          width: Number(form.elements.width.value), height: Number(form.elements.height.value),
          framerateNumerator: 0, framerateDenominator: 1,
          pixelFormat: type === 'capture' ? pixelFormatInput.value : 'auto' };
    if (type !== 'none' && !payload.endpoint) {
      toast(type === 'capture' ? '请填写 V4L2 设备路径' : '请填写网络流地址', 'error');
      endpointInput.focus();
      return;
    }
    if ((type === 'stream' || type === 'network_camera') &&
        !/^(?:rtsp|https?|udp|tcp):\/\/\S+$/.test(payload.endpoint)) {
      toast('网络流地址格式不正确', 'error');
      endpointInput.focus();
      return;
    }
    if (!Number.isInteger(payload.width) || !Number.isInteger(payload.height) ||
        (payload.width === 0) !== (payload.height === 0) || payload.width < 0 ||
        payload.width > 65535 || payload.height < 0 || payload.height > 65535) {
      toast('信号宽高必须同时为 0，或填写 1–65535 的整数', 'error');
      return;
    }

    const submit = form.querySelector('button[type="submit"]');
    submit.disabled = true;
    try {
      await requestApi('setNodeSource', payload);
      Object.assign(node, {
        signalSourceType: payload.sourceType,
        signalSourceConfigured: payload.sourceType !== 'none',
        signalSourceEndpoint: payload.endpoint,
        signalSourceWidth: payload.width,
        signalSourceHeight: payload.height,
        signalSourceFramerateNumerator: payload.framerateNumerator,
        signalSourceFramerateDenominator: payload.framerateDenominator,
        signalSourcePixelFormat: payload.pixelFormat
      });
      container.dataset.dirty = 'false';
      addLog(`输入节点 ${formatNodeId(node.nodeId)} 的信号属性已更新`);
      toast('输入属性已保存');
      renderNodeAssignments();
    } catch (error) {
      addLog(error.message, true);
      toast(error.message, 'error');
    } finally {
      submit.disabled = false;
    }
  });
}

function renderAssignmentProperties(config, node) {
  const container = byId(config.propertyId);
  if (container.dataset.dirty === 'true' && container.contains(document.activeElement)) return;
  container.dataset.dirty = 'false';
  if (!node) {
    container.innerHTML = `<div class="assignment-empty"><span>${config.kind === '输入' ? 'IN' : 'OUT'}</span><strong>选择${config.kind}节点</strong><small>节点属性将显示在这里</small></div>`;
    return;
  }

  const binding = regionBindingForNode(node.nodeId);
  const boundMessage = binding
    ? `<div class="assignment-binding-note"><strong>已绑定区域</strong><span>${escapeHtml(binding.name)} · ${binding.kind}节点</span></div>`
    : '';
  container.innerHTML = `
    <div class="assignment-property-head">
      <div class="assignment-node-avatar">${escapeHtml(formatNodeId(node.nodeId))}</div>
      <div><h4>${escapeHtml(nodeDisplayName(node))}</h4><p>${escapeHtml(node.ip || '--')} · ID ${escapeHtml(formatNodeId(node.nodeId))}</p></div>
      <button class="button button-secondary assignment-unassign" type="button"${binding ? ' disabled' : ''}>取消分配</button>
    </div>
    ${boundMessage}
    <dl class="detail-list assignment-detail-list">
      <dt>分类</dt><dd>${escapeHtml(config.kind)}节点</dd>
      <dt>连接状态</dt><dd><span class="status-badge ${node.status === 'online' ? 'online' : 'offline'}">${node.status === 'online' ? '在线' : '离线'}</span></dd>
      <dt>设备型号</dt><dd>${nodeFoundationValue(node, 'deviceModel')}</dd>
      <dt>设备 IP</dt><dd>${escapeHtml(node.ip || '--')}</dd>
      <dt>MAC</dt><dd>${nodeFoundationValue(node, 'macAddress')}</dd>
      <dt>显示分辨率</dt><dd>${escapeHtml(nodeDisplayResolution(node))}</dd>
    </dl>`;

  const unassign = container.querySelector('.assignment-unassign');
  unassign.title = binding ? `已绑定到区域“${binding.name}”，请先解除区域绑定` : `取消${config.kind}分类`;
  unassign.addEventListener('click', async () => {
    unassign.disabled = true;
    try {
      await setNodeAssignmentRole(node, 'unassigned');
    } catch (error) {
      addLog(error.message, true);
      toast(error.message, 'error');
      unassign.disabled = false;
    }
  });

  if (config.role === 'encode') {
    renderInputSourceProperties(container, node);
  } else {
    container.insertAdjacentHTML('beforeend', `
      <section class="assignment-output-properties">
        <div class="node-profile-title"><strong>输出运行属性</strong><span>节点实时上报</span></div>
        <dl class="detail-list assignment-detail-list">
          <dt>播放状态</dt><dd>${escapeHtml(node.playState || '--')}</dd>
          <dt>音频输出</dt><dd>${escapeHtml(node.audioOutputMode || '--')}</dd>
          <dt>CPU 使用率</dt><dd>${escapeHtml(node.cpu)}%</dd>
          <dt>内存使用率</dt><dd>${escapeHtml(node.memory)}%</dd>
          <dt>最后心跳</dt><dd>${escapeHtml(formatDate(node.lastSeen))}</dd>
        </dl>
      </section>`);
  }
}

function renderNodeAssignmentPage(config) {
  const unassigned = appState.nodes.filter(node => nodeProfile(node).role === 'unassigned');
  const assigned = appState.nodes.filter(node => nodeProfile(node).role === config.role || nodeProfile(node).role === 'codec');
  const unassignedList = byId(config.unassignedListId);
  const assignedList = byId(config.assignedListId);
  if (!unassignedList || !assignedList) return;

  byId(config.unassignedCountId).textContent = String(unassigned.length);
  byId(config.assignedCountId).textContent = String(assigned.length);
  unassignedList.replaceChildren();
  assignedList.replaceChildren();
  if (!unassigned.length) {
    unassignedList.innerHTML = '<div class="assignment-list-empty">没有未分配节点</div>';
  } else {
    unassigned.forEach(node => unassignedList.append(assignmentNodeCard(node, config, false)));
  }
  if (!assigned.length) {
    assignedList.innerHTML = `<div class="assignment-list-empty">没有${config.kind}节点</div>`;
  } else {
    if (!assigned.some(node => node.nodeId === appState.assignmentSelection[config.role])) {
      appState.assignmentSelection[config.role] = assigned[0].nodeId;
    }
    assigned.forEach(node => assignedList.append(assignmentNodeCard(node, config, true)));
  }
  const selected = assigned.find(node => node.nodeId === appState.assignmentSelection[config.role]) || null;
  renderAssignmentProperties(config, selected);
}

function renderNodeAssignments() {
  Object.values(NODE_ASSIGNMENT_PAGES).forEach(renderNodeAssignmentPage);
}

function renderStatusTable() {
  const body = byId('statusRows');
  body.replaceChildren();

  appState.nodes.forEach(node => {
    const row = document.createElement('tr');
    const profile = nodeProfile(node);
    const isOnline = node.status === 'online';
    row.innerHTML = `
      <td><strong class="status-node-name">${escapeHtml(profile.name)}</strong><small class="status-node-id">ID ${escapeHtml(formatNodeId(node.nodeId))} · ${escapeHtml(nodeRoleLabel(profile.role))}</small></td>
      <td>${escapeHtml(node.ip)}</td>
      <td>${escapeHtml(nodeDisplayResolution(node))}</td>
      <td><span class="status-badge ${isOnline ? 'online' : 'offline'}">${escapeHtml(node.status)}</span></td>
      <td>${escapeHtml(node.playState)}</td>
      <td>${escapeHtml(node.cpu)}%</td>
      <td>${escapeHtml(node.memory)}%</td>
      <td>${escapeHtml(formatDate(node.lastSeen))}</td>`;
    body.append(row);
  });

  if (!appState.nodes.length) {
    body.innerHTML = '<tr><td colspan="8" class="empty-cell">没有设备状态</td></tr>';
  }
  byId('onlineCount').textContent = String(appState.nodes.filter(node => node.status === 'online').length);
}

function renderMasterStatus() {
  const status = appState.status;
  if (!status) return;

  byId('masterRunning').textContent = status.running ? '运行中' : '已停止';
  byId('masterState').textContent = text(status.state);
  byId('requestCount').textContent = text(status.http?.totalRequests);
  byId('serviceState').textContent = status.running ? '运行中' : '已停止';
  byId('syncTimestamp').textContent = status.syncTimestamp ? formatDate(status.syncTimestamp) : '未同步';
  byId('updatedAt').textContent = formatDate(status.updatedAt);
}

function renderAudioOutput() {
  const mode = appState.audioOutput?.mode || 'both';
  document.querySelectorAll('[data-audio-output]').forEach(button => {
    const active = button.dataset.audioOutput === mode;
    button.classList.toggle('active', active);
    button.setAttribute('aria-pressed', String(active));
  });
  const label = { hdmi: 'HDMI', analog: '3.5mm', both: 'HDMI + 3.5mm' }[mode] || mode;
  byId('audioOutputState').textContent = label;
}

function renderAudioVolume() {
  const volume = Math.max(0, Math.min(100, Number(appState.audioVolume?.volumePercent ?? 100)));
  const input = byId('audioVolume');
  const output = byId('audioVolumeValue');
  if (input) input.value = String(volume);
  if (output) output.textContent = `${volume}%`;
}

const DEFAULT_OUTPUT_LAYOUT = Object.freeze({
  rows: 1,
  cols: 1,
  screenWidth: 1920,
  screenHeight: 1080
});
let zoneDraft = null;

function zoneNode(nodeId) {
  return appState.nodes.find(node => String(node.nodeId) === String(nodeId));
}

function isOutputNode(node) {
  const role = String(node?.nodeRole || '').toLowerCase();
  if (role) return role === 'decode' || role === 'codec';
  const type = String(node?.deviceType || '').toLowerCase();
  if (type) return type === 'output' || type === 'input/output';
  return false;
}

function isInputNode(node) {
  const role = String(node?.nodeRole || '').toLowerCase();
  if (role) return role === 'encode' || role === 'codec';
  const type = String(node?.deviceType || '').toLowerCase();
  if (type) return type === 'input' || type === 'input/output';
  return false;
}

function regionNodeIds(region) {
  if (Array.isArray(region?.placements)) {
    return region.placements.map(item => item?.nodeId);
  }
  return [];
}

function normalizeRegionPlacement(value) {
  const source = value && typeof value === 'object' ? value : {};
  const nodeId = Number(source.nodeId);
  const x = Number(source.x);
  const y = Number(source.y);
  const width = Number(source.width);
  const height = Number(source.height);
  if (![nodeId, x, y, width, height].every(Number.isFinite) ||
      !Number.isInteger(nodeId) || nodeId <= 0 ||
      x < 0 || y < 0 || width <= 0 || height <= 0 ||
      x + width > 1.000001 || y + height > 1.000001) return null;
  return { nodeId, x, y, width, height };
}

function regionPlacements(region) {
  if (!Array.isArray(region?.placements)) return [];
  return region.placements.map(normalizeRegionPlacement).filter(Boolean);
}

function gridRegionPlacements(nodeIds, layout) {
  const rows = Math.max(1, Number(layout.rows) || 1);
  const cols = Math.max(1, Number(layout.cols) || 1);
  return nodeIds.slice(0, rows * cols).map((nodeId, index) => ({
    nodeId,
    x: (index % cols) / cols,
    y: Math.floor(index / cols) / rows,
    width: 1 / cols,
    height: 1 / rows
  }));
}

function normalizeRegionLayout(source = {}) {
  const fallback = appState.layout || DEFAULT_OUTPUT_LAYOUT;
  return {
    rows: Number(source.rows) || Number(fallback.rows),
    cols: Number(source.cols) || Number(fallback.cols),
    screenWidth: Number(source.screenWidth) || Number(fallback.screenWidth),
    screenHeight: Number(source.screenHeight) || Number(fallback.screenHeight)
  };
}

function normalizeSavedRegion(region) {
  if (typeof region !== 'object' || region === null || !Array.isArray(region.inputNodeIds)) {
    return null;
  }
  const source = region;
  const name = String(source.name || '').trim();
  if (!name) return null;

  const outputNodeIds = [...new Set(
    regionNodeIds(region)
      .map(value => Number(value))
      .filter(value => Number.isInteger(value) && value > 0)
  )];
  const inputNodeIds = [...new Set(
    source.inputNodeIds
      .map(value => Number(value))
      .filter(value => Number.isInteger(value) && value > 0)
  )].filter(nodeId => !outputNodeIds.includes(nodeId));

  return {
    name,
    inputNodeIds,
    outputNodeIds,
    layout: normalizeRegionLayout(source.layout || source),
    placements: regionPlacements(source),
    savedAt: Number(source.savedAt || source.createdAt) || 0
  };
}

function writeSavedRegions(regions) {
  const unique = [];
  const knownNames = new Set();

  regions.map(normalizeSavedRegion).filter(Boolean).forEach(region => {
    if (knownNames.has(region.name)) return;
    knownNames.add(region.name);
    unique.push(region);
  });

  appState.regions = unique.slice(0, 48);
}

function readSavedRegions() {
  return Array.isArray(appState.regions) ? appState.regions : [];
}

function regionDraftFromCurrent() {
  return {
    regionName: '',
    originalRegionName: '',
    inputNodeIds: [],
    outputNodeIds: [],
    layout: normalizeRegionLayout(appState.layout || {}),
    placements: []
  };
}

function regionDraftFromSavedRegion(region) {
  return {
    regionName: region.name,
    originalRegionName: region.name,
    inputNodeIds: [...region.inputNodeIds],
    outputNodeIds: [...region.outputNodeIds],
    layout: { ...region.layout },
    placements: regionPlacements(region)
  };
}

function syncZoneDraftFields() {
  if (!zoneDraft) return;
  byId('zoneRegionName').value = zoneDraft.regionName;
  byId('zoneRows').value = zoneDraft.layout.rows;
  byId('zoneCols').value = zoneDraft.layout.cols;
  byId('zoneScreenWidth').value = zoneDraft.layout.screenWidth;
  byId('zoneScreenHeight').value = zoneDraft.layout.screenHeight;
}

function updateZoneHint() {
  if (!zoneDraft) return;
  const inputCount = zoneDraft.inputNodeIds.length;
  const outputCount = zoneDraft.outputNodeIds.length;
  byId('zoneSelectedCount').textContent = String(inputCount + outputCount);
  byId('zoneSaveHint').textContent = `已选输入 ${inputCount} · 输出 ${outputCount}`;
}

function renderZoneNodes() {
  if (!zoneDraft) return;
  const bindings = new Map();
  readSavedRegions().forEach(region => {
    const regionName = region.name;
    region.inputNodeIds.forEach(nodeId => bindings.set(Number(nodeId), {
      name: regionName,
      kind: 'input'
    }));
    region.outputNodeIds.forEach(nodeId => bindings.set(Number(nodeId), {
      name: regionName,
      kind: 'output'
    }));
  });

  renderZoneNodeList(
    'zoneInputNodeList',
    'zoneInputNodeCount',
    zoneNodesForKind(isInputNode, zoneDraft.inputNodeIds, 'encode'),
    'input',
    bindings
  );
  renderZoneNodeList(
    'zoneOutputNodeList',
    'zoneOutputNodeCount',
    zoneNodesForKind(isOutputNode, zoneDraft.outputNodeIds, 'decode'),
    'output',
    bindings
  );
  renderZoneSelectedNodes();
}

function zoneNodesForKind(predicate, selectedIds, fallbackRole) {
  const nodes = appState.nodes.filter(node =>
    predicate(node) || selectedIds.includes(Number(node.nodeId))
  );
  const knownIds = new Set(nodes.map(node => Number(node.nodeId)));
  selectedIds.forEach(nodeId => {
    if (knownIds.has(nodeId)) return;
    nodes.push({ nodeId, ip: '--', status: 'offline', nodeRole: fallbackRole });
  });
  return nodes;
}

function renderZoneNodeList(listId, countId, nodes, kind, bindings) {
  const list = byId(listId);
  if (!list) return;
  list.replaceChildren();
  byId(countId).textContent = String(nodes.length);
  const selectedIds = kind === 'input' ? zoneDraft.inputNodeIds : zoneDraft.outputNodeIds;
  const otherKind = kind === 'input' ? 'output' : 'input';
  const otherSelectedIds = kind === 'input' ? zoneDraft.outputNodeIds : zoneDraft.inputNodeIds;

  if (!nodes.length) {
    list.innerHTML = `<div class="zone-empty">暂无${kind === 'input' ? '输入' : '输出'}节点</div>`;
    byId(kind === 'input' ? 'zoneInputSelectionHint' : 'zoneOutputSelectionHint').textContent =
      `暂无可分配的${kind === 'input' ? '输入' : '输出'}节点`;
    return;
  }

  nodes.forEach(node => {
    const nodeId = Number(node.nodeId);
    const selected = selectedIds.includes(nodeId);
    const binding = bindings.get(nodeId);
    const boundToCurrent = binding?.name === zoneDraft.originalRegionName &&
      binding?.kind === kind;
    const selectedInOtherList = otherSelectedIds.includes(nodeId);
    const unavailable = (binding && binding.name !== zoneDraft.originalRegionName) ||
      selectedInOtherList ||
      (binding?.name === zoneDraft.originalRegionName && binding.kind !== kind);
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'zone-node-item' +
      (selected ? ' selected assigned' : '') + (unavailable ? ' unavailable' : '');
    button.disabled = unavailable;
    const status = selected
      ? (boundToCurrent ? '当前区域' : '已选择')
      : binding && binding.name !== zoneDraft.originalRegionName
        ? `已绑定：${binding.name}`
        : binding?.name === zoneDraft.originalRegionName
          ? `当前区域（${binding.kind === 'input' ? '输入' : '输出'}）`
          : selectedInOtherList
            ? `当前区域（${otherKind === 'input' ? '输入' : '输出'}）`
            : '可选择';
    button.title = unavailable ? status : '';
    button.innerHTML =
      '<span class="zone-node-avatar">' + escapeHtml(formatNodeId(node.nodeId)) + '</span>' +
      '<span class="zone-node-copy"><strong>' + escapeHtml(nodeDisplayName(node)) +
      '</strong><small>' + escapeHtml(node.ip) + ' · ' +
      escapeHtml(nodeRoleLabel(nodeProfile(node).role)) + ' · ' +
      (node.status === 'online' ? '在线' : '离线') + '</small></span>' +
      '<em>' + escapeHtml(status) + '</em>';

    if (!unavailable) {
      button.addEventListener('click', () => {
        const target = kind === 'input' ? zoneDraft.inputNodeIds : zoneDraft.outputNodeIds;
        zoneDraft[kind === 'input' ? 'inputNodeIds' : 'outputNodeIds'] = selected
          ? target.filter(id => id !== nodeId)
          : [...target, nodeId];
        updateZoneHint();
        renderZoneNodes();
      });
    }
    list.append(button);
  });
  byId(kind === 'input' ? 'zoneInputSelectionHint' : 'zoneOutputSelectionHint').textContent =
    selectedIds.length
      ? `已选择 ${selectedIds.length} 个${kind === 'input' ? '输入' : '输出'}节点`
      : `点击节点加入${kind === 'input' ? '客户端输入开窗' : '输出墙'}`;
}

function renderZoneSelectedNodes() {
  const list = byId('zoneSelectedNodeList');
  if (!list || !zoneDraft) return;
  list.replaceChildren();

  const selected = [
    ...zoneDraft.inputNodeIds.map(nodeId => ({ nodeId, kind: '输入' })),
    ...zoneDraft.outputNodeIds.map(nodeId => ({ nodeId, kind: '输出' }))
  ];
  if (!selected.length) {
    list.innerHTML = '<div class="zone-empty">还没有选择节点<br><small>从输入或输出列表中选择节点</small></div>';
    return;
  }

  selected.forEach(({ nodeId, kind }) => {
    const node = zoneNode(nodeId) || {
      nodeId,
      ip: '--',
      status: 'offline',
      nodeRole: kind === '输入' ? 'encode' : 'decode'
    };

    const item = document.createElement('article');
    item.className = 'zone-selected-node';
    item.innerHTML =
      '<span class="zone-node-avatar">' + escapeHtml(formatNodeId(node.nodeId)) + '</span>' +
      '<span class="zone-node-copy"><strong>' + escapeHtml(nodeDisplayName(node)) +
      '</strong><small>' + escapeHtml(node.ip) + ' · ' +
      kind + ' · ' + (node.status === 'online' ? '在线' : '离线') + '</small></span>' +
      '<button type="button" aria-label="移出节点">×</button>';

    item.querySelector('button').addEventListener('click', () => {
      const field = kind === '输入' ? 'inputNodeIds' : 'outputNodeIds';
      zoneDraft[field] = zoneDraft[field].filter(id => id !== Number(node.nodeId));
      updateZoneHint();
      renderZoneNodes();
    });
    list.append(item);
  });
}

function setZoneModalMode(isEditing) {
  byId('zoneModalKicker').textContent = isEditing ? '区域 / 编辑' : '区域 / 创建';
  byId('zoneModalTitle').textContent = isEditing ? '编辑区域' : '创建区域';
  byId('zoneEditorTitle').textContent = '已选节点';
  byId('zoneModalDescription').textContent = '设置区域名称、输出墙布局，并分别绑定输入与输出节点。';
}

function resetZoneForm() {
  zoneDraft = regionDraftFromCurrent();
  syncZoneDraftFields();
  setZoneModalMode(false);
  updateZoneHint();
  renderZoneNodes();
}

function regionApiPayload(region) {
  const layout = normalizeRegionLayout(region.layout || {});
  const inputNodeIds = [...new Set(region.inputNodeIds
    .map(value => Number(value))
    .filter(value => Number.isInteger(value) && value > 0))];
  const outputNodeIds = [...new Set(region.outputNodeIds
    .map(value => Number(value))
    .filter(value => Number.isInteger(value) && value > 0))]
    .filter(nodeId => !inputNodeIds.includes(nodeId));
  const placements = regionPlacements(region).filter(placement =>
    outputNodeIds.includes(placement.nodeId)
  );
  return {
    name: region.name,
    inputNodeIds,
    layout,
    placements: placements.length === outputNodeIds.length
      ? placements
      : gridRegionPlacements(outputNodeIds, layout),
    savedAt: region.savedAt || Date.now()
  };
}

async function removeSavedRegion(regionName) {
  const name = String(regionName || '').trim();
  if (!name) return;

  const regions = readSavedRegions();
  if (!regions.some(region => region.name === name)) return;

  const remaining = regions.filter(region => region.name !== name);
  const result = await requestApi('setRegions', {
    regions: remaining.map(regionApiPayload)
  });
  const savedRegions = Array.isArray(result.regions)
    ? result.regions.map(normalizeSavedRegion).filter(Boolean)
    : remaining;

  writeSavedRegions(savedRegions);
  if (zoneDraft?.originalRegionName === name) {
    resetZoneForm();
  }
  renderZoneManagementPage();
  toast('区域“' + name + '”已删除');
}

function renderZoneManagementPage() {
  const rows = byId('zoneManagementRows');
  if (!rows) return;

  const regions = readSavedRegions();
  byId('zoneManagementTotal').textContent = regions.length + ' 个区域';
  rows.replaceChildren();

  if (!regions.length) {
    rows.innerHTML =
      '<tr class="zone-management-empty"><td colspan="8"><div><strong>还没有区域</strong>' +
      '<span>点击“创建区域”绑定输入/输出节点并设置输出墙布局。</span></div></td></tr>';
    return;
  }

  regions.forEach(region => {
    const row = document.createElement('tr');
    const outputNames = region.outputNodeIds
      .map(nodeId => nodeDisplayName(zoneNode(nodeId) || { nodeId }))
      .join('、') || '未选择节点';
    const inputNames = region.inputNodeIds
      .map(nodeId => nodeDisplayName(zoneNode(nodeId) || { nodeId }))
      .join('、') || '未选择节点';
    row.innerHTML =
      '<td><span class="zone-table-region"><svg><use href="#icon-zone"></use></svg>' +
      escapeHtml(region.name) + '</span></td>' +
      '<td><span class="zone-node-window-count">' + region.inputNodeIds.length + '</span></td>' +
      '<td><span class="zone-table-layout">' + escapeHtml(inputNames) + '</span></td>' +
      '<td><span class="zone-node-window-count">' + region.outputNodeIds.length + '</span></td>' +
      '<td><span class="zone-table-layout">' + escapeHtml(outputNames) + '</span></td>' +
      '<td><span class="zone-table-layout">' + escapeHtml(`${region.layout.rows}×${region.layout.cols} · ${region.layout.screenWidth}×${region.layout.screenHeight}`) + '</span></td>' +
      '<td><span class="zone-table-time">' +
      (region.savedAt ? escapeHtml(formatDate(region.savedAt)) : '--') + '</span></td>' +
      '<td><div class="zone-row-actions"><button type="button" class="zone-row-button zone-row-edit">编辑</button>' +
      '<button type="button" class="zone-row-button zone-row-delete">删除</button></div></td>';

    row.querySelector('.zone-row-edit').addEventListener('click', () => openZoneModal(region));
    row.querySelector('.zone-row-delete').addEventListener('click', () => {
      removeSavedRegion(region.name).catch(error => toast(error.message, 'error'));
    });
    rows.append(row);
  });
}

function openZoneModal(savedRegion = null) {
  zoneDraft = savedRegion
    ? regionDraftFromSavedRegion(savedRegion)
    : regionDraftFromCurrent();
  syncZoneDraftFields();
  setZoneModalMode(Boolean(savedRegion));
  byId('zoneModal').hidden = false;
  byId('zoneModal').setAttribute('aria-hidden', 'false');
  document.body.classList.add('zone-modal-open');
  document.body.classList.remove('nav-open');
  updateZoneHint();
  renderZoneNodes();
  setTimeout(() => byId('zoneRegionName').focus(), 0);
}

function closeZoneModal() {
  const modal = byId('zoneModal');
  modal.hidden = true;
  modal.setAttribute('aria-hidden', 'true');
  document.body.classList.remove('zone-modal-open');
  zoneDraft = null;
}

async function saveRegion() {
  const regionName = byId('zoneRegionName').value.trim();
  if (!regionName) throw new Error('请填写区域名称');

  const current = zoneDraft || regionDraftFromCurrent();
  const layout = {
    rows: numberValue('zoneRows'),
    cols: numberValue('zoneCols'),
    screenWidth: numberValue('zoneScreenWidth'),
    screenHeight: numberValue('zoneScreenHeight')
  };
  if (!Object.values(layout).every(Number.isInteger) ||
      layout.rows < 1 || layout.rows > 8 || layout.cols < 1 || layout.cols > 8 ||
      layout.rows * layout.cols > 64 || layout.screenWidth < 1 || layout.screenWidth > 16384 ||
      layout.screenHeight < 1 || layout.screenHeight > 16384) {
    throw new Error('输出墙布局参数无效：行列为 1–8，单屏宽高为 1–16384');
  }
  if (current.outputNodeIds.length > layout.rows * layout.cols) {
    throw new Error('选择的节点数量不能超过输出墙屏幕数量');
  }
  const inputNodeIds = [...new Set(current.inputNodeIds.map(Number))];
  const outputNodeIds = [...new Set(current.outputNodeIds.map(Number))];
  if (inputNodeIds.some(nodeId => outputNodeIds.includes(nodeId))) {
    throw new Error('同一节点不能同时绑定为输入和输出');
  }
  const regions = readSavedRegions().filter(region => (
    region.name !== regionName &&
    region.name !== current.originalRegionName
  ));
  const next = {
    name: regionName,
    inputNodeIds,
    outputNodeIds,
    layout,
    placements: gridRegionPlacements(outputNodeIds, layout),
    savedAt: Date.now()
  };

  byId('zoneSaveHint').textContent = '正在保存区域布局与节点...';
  const result = await requestApi('setRegions', {
    regions: [next, ...regions].map(regionApiPayload)
  });
  const activeOutput = regionApiPayload(next);
  await requestApi('setScreens', {
    ...activeOutput.layout,
    placements: activeOutput.placements
  });
  const savedRegions = Array.isArray(result.regions)
    ? result.regions.map(normalizeSavedRegion).filter(Boolean)
    : [next, ...regions];

  writeSavedRegions(savedRegions);
  renderZoneManagementPage();
  appState.layout = {
    ...activeOutput.layout,
    totalWidth: activeOutput.layout.cols * activeOutput.layout.screenWidth,
    totalHeight: activeOutput.layout.rows * activeOutput.layout.screenHeight
  };
  renderNodeAssignments();
  addLog('区域已保存并启用输出墙：' + regionName + '（输入 ' + next.inputNodeIds.length + ' · 输出 ' + next.outputNodeIds.length + '）');
  toast('区域“' + regionName + '”及输出墙布局已启用');
  resetZoneForm();
  setTimeout(() => byId('zoneRegionName').focus(), 0);
}

async function refreshAll(showToast = false) {
  if (appState.refreshing) return;
  appState.refreshing = true;

  try {
    const [nodeResult, regionResult, screenResult, statusResult, audioOutputResult, audioVolumeResult] = await Promise.all([
      requestApi('nodes'),
      requestApi('regions'),
      requestApi('screens'),
      requestApi('status'),
      requestApi('audioOutput'),
      requestApi('audioVolume')
    ]);

    appState.nodes = Array.isArray(nodeResult.nodes) ? nodeResult.nodes : [];
    appState.regions = Array.isArray(regionResult.regions)
      ? regionResult.regions.map(normalizeSavedRegion).filter(Boolean)
      : [];
    appState.layout = screenResult.layout || null;
    appState.status = statusResult;
    appState.audioOutput = audioOutputResult;
    appState.audioVolume = audioVolumeResult;

    renderDevices();
    renderStatusTable();
    renderNodeAssignments();
    renderMasterStatus();
    renderAudioOutput();
    renderAudioVolume();
    renderZoneManagementPage();
    if (!appState.initialized || showToast) {
      const layoutText = appState.layout ? `${appState.layout.rows}×${appState.layout.cols}` : '未配置';
      addLog(`刷新完成：${appState.nodes.length} 个设备，布局 ${layoutText}`);
    }
    if (showToast) toast('设备与系统状态已刷新');
    appState.initialized = true;
  } catch (error) {
    appState.nodes = [];
    appState.regions = [];
    appState.layout = null;
    appState.status = null;
    appState.audioOutput = null;
    appState.audioVolume = null;
    appState.selectedNodeId = null;
    renderDevices();
    renderStatusTable();
    renderNodeAssignments();
    renderZoneManagementPage();
    addLog(error.message, true);
    if (showToast) toast(error.message, 'error');
  } finally {
    appState.refreshing = false;
  }
}

async function discoverNodes() {
  const label = byId('discoverDevicesLabel');
  label.textContent = '正在搜索…';
  addLog('正在搜索路由可达网段中的设备节点…');
  try {
    const result = await requestApi('discoverNodes', {});
    await refreshAll(false);
    const found = Number(result.nodesFound) || 0;
    const probed = Number(result.addressesProbed) || 0;
    const networks = Array.isArray(result.networks) ? result.networks.join('、') : '--';
    addLog(`设备搜索完成：${networks}，发现 ${found} 个节点，探测 ${probed} 个地址`);
    toast(`设备搜索完成，发现 ${found} 个节点`);
  } finally {
    label.textContent = '搜索设备';
  }
}

async function setAudioOutput(mode) {
  const result = await requestApi('setAudioOutput', { mode });
  appState.audioOutput = result;
  renderAudioOutput();
  const label = { hdmi: 'HDMI', analog: '3.5mm', both: 'HDMI + 3.5mm' }[mode];
  addLog(`Audio output switched to ${label}`);
  toast(`Audio output: ${label}`);
}

async function setAudioVolume(volumePercent) {
  const value = Math.max(0, Math.min(100, Number(volumePercent)));
  const result = await requestApi('setAudioVolume', { volumePercent: Math.round(value) });
  appState.audioVolume = result;
  renderAudioVolume();
  addLog(`Audio volume set to ${Math.round(value)}%`);
}

function bindAsync(element, action) {
  element.addEventListener('click', async () => {
    element.disabled = true;
    try {
      await action();
    } catch (error) {
      addLog(error.message, true);
      toast(error.message, 'error');
    } finally {
      element.disabled = false;
      if (element.id === 'acquireKvm' || element.id === 'releaseKvm') renderKvm();
    }
  });
}

function initialize() {
  preparePageActions();
  const address = `${location.protocol}//${location.host}/`;
  const host = location.hostname || '127.0.0.1';
  byId('adapterIp').value = host;
  byId('serverAddress').textContent = address;
  byId('adapterAddress').textContent = host;

  document.querySelectorAll('.nav-item').forEach(button => {
    button.addEventListener('click', () => openPage(button.dataset.page));
  });
  document.querySelectorAll('[data-page-link]').forEach(button => {
    button.addEventListener('click', () => openPage(button.dataset.pageLink));
  });
  document.querySelectorAll('[data-audio-output]').forEach(button => {
    bindAsync(button, () => setAudioOutput(button.dataset.audioOutput));
  });
  const audioVolume = byId('audioVolume');
  if (audioVolume) {
    audioVolume.addEventListener('input', () => {
      const value = Math.round(Number(audioVolume.value));
      byId('audioVolumeValue').textContent = `${value}%`;
    });
    audioVolume.addEventListener('change', async () => {
      audioVolume.disabled = true;
      try {
        await setAudioVolume(audioVolume.value);
      } catch (error) {
        addLog(error.message, true);
        toast(error.message, 'error');
        renderAudioVolume();
      } finally {
        audioVolume.disabled = false;
      }
    });
  }

  byId('createZone').addEventListener('click', () => openZoneModal());
  byId('createZoneFromManager').addEventListener('click', () => openZoneModal());
  byId('refreshZoneManager').addEventListener('click', () => {
    renderZoneManagementPage();
    toast('区域列表已刷新');
  });
  document.querySelectorAll('[data-close-zone]').forEach(element => element.addEventListener('click', closeZoneModal));
  bindAsync(byId('saveRegion'), saveRegion);
  document.addEventListener('keydown', event => {
    if (event.key !== 'Escape') return;
    if (!byId('zoneModal').hidden) closeZoneModal();
  });

  bindAsync(byId('discoverDevices'), discoverNodes);
  bindAsync(byId('refreshStatus'), () => refreshAll(true));
  bindAsync(byId('refreshSettings'), () => refreshAll(true));
  bindAsync(byId('refreshInputNodes'), () => refreshAll(true));
  bindAsync(byId('refreshOutputNodes'), () => refreshAll(true));

  byId('clearLogs').addEventListener('click', () => {
    byId('logList').replaceChildren();
    addLog('日志显示已清空');
  });
  byId('menuToggle').addEventListener('click', () => document.body.classList.toggle('nav-open'));
  byId('sidebarBackdrop').addEventListener('click', () => document.body.classList.remove('nav-open'));

  openPage(location.hash.slice(1) || 'devices');
  window.addEventListener('hashchange', () => openPage(location.hash.slice(1)));

  const updateClock = () => {
    byId('clock').textContent = new Date().toLocaleString('zh-CN', { hour12: false });
  };
  updateClock();
  setInterval(updateClock, 1000);

  refreshAll();
  setInterval(() => refreshAll(false), 5000);
}

document.addEventListener('DOMContentLoaded', initialize);
