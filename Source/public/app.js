/* 文件快转 —— 前端交互逻辑 */
(function () {
  'use strict';

  // ---------------- 基础工具 ----------------
  const $ = (sel) => document.querySelector(sel);
  const $$ = (sel) => Array.from(document.querySelectorAll(sel));

  function fmtSize(n) {
    if (n == null) return '-';
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
    if (n < 1024 * 1024 * 1024) return (n / (1024 * 1024)).toFixed(1) + ' MB';
    return (n / (1024 * 1024 * 1024)).toFixed(2) + ' GB';
  }

  function fmtTime(ts) {
    const d = new Date(ts);
    const p = (x) => String(x).padStart(2, '0');
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
  }

  let toastTimer = null;
  function toast(msg, ms) {
    const el = $('#toast');
    el.textContent = msg;
    el.classList.remove('hidden');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => el.classList.add('hidden'), ms || 2600);
  }

  // ---------------- 设备身份 ----------------
  const DEVICE_ID_KEY = 'fqt_device_id';
  const DEVICE_NAME_KEY = 'fqt_device_name';
  let deviceId = Array.from(crypto.getRandomValues(new Uint8Array(16)), byte =>
    byte.toString(16).padStart(2, '0')).join('');
  let deviceName = '我的设备-' + Math.floor(1000 + Math.random() * 9000);
  try {
    const savedId = localStorage.getItem(DEVICE_ID_KEY);
    if (savedId) deviceId = savedId;
    else localStorage.setItem(DEVICE_ID_KEY, deviceId);
    deviceName = localStorage.getItem(DEVICE_NAME_KEY) || deviceName;
  } catch (e) { /* 存储不可用 */ }

  const nameInput = $('#device-name');
  nameInput.value = deviceName;
  nameInput.addEventListener('change', () => {
    deviceName = nameInput.value.trim() || deviceName;
    try { localStorage.setItem(DEVICE_NAME_KEY, deviceName); } catch (e) { /* 存储不可用 */ }
    sendHello();
    toast('设备名已更新');
  });

  // ---------------- 局域网地址 ----------------
  const API = location.origin;
  const isHostLoopback = location.hostname === '127.0.0.1' || location.hostname === 'localhost';
  const firewallControls = $('#firewall-controls');
  if (firewallControls && isHostLoopback) firewallControls.classList.remove('hidden');
  $('#firewall-allow')?.addEventListener('click', async (event) => {
    if (!isHostLoopback) return;
    const button = event.currentTarget;
    const status = $('#firewall-status');
    button.disabled = true;
    status.textContent = '正在请求系统确认…';
    try {
      const response = await fetch(API + '/api/host/firewall', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({}) });
      const data = await response.json();
      if (!response.ok || !data.ok) throw new Error(data.error || '设置防火墙规则失败');
      status.textContent = '已允许局域网访问';
    } catch (error) {
      status.textContent = error.message || '请求失败';
    } finally { button.disabled = false; }
  });
  const adapterSelect = $('#network-adapter');
  const networkLink = $('#network-link');
  const networkStatus = $('#network-status');
  const qrImage = $('#qr-img');
  let selectedAdapterUrl = '';
  let manualAdapterUrl = '';
  let networkRequestInProgress = false;

  function parseAdapterUrl(value) {
    try {
      const url = new URL(value);
      if (url.protocol === 'http:' && /^\d+(\.\d+){3}$/.test(url.hostname) &&
          !url.hostname.startsWith('127.') && url.hostname !== '0.0.0.0') return url;
    } catch (e) { /* 忽略无效地址 */ }
    return null;
  }

  function selectAdapter(url) {
    adapterSelect.value = url;
    networkLink.href = url;
    networkLink.textContent = url;
    if (url === selectedAdapterUrl && !qrImage.hidden) return;
    selectedAdapterUrl = url;
    qrImage.hidden = true;
    qrImage.src = '/api/qr?address=' + encodeURIComponent(new URL(url).hostname);
  }

  qrImage.addEventListener('load', () => {
    qrImage.hidden = false;
    networkStatus.textContent = '';
  });
  qrImage.addEventListener('error', () => {
    if (!selectedAdapterUrl) return;
    qrImage.hidden = true;
    networkStatus.textContent = '暂时无法加载二维码，可以复制链接访问';
  });
  adapterSelect.addEventListener('change', () => {
    manualAdapterUrl = adapterSelect.value;
    selectAdapter(manualAdapterUrl);
  });

  async function refreshNetwork() {
    if (networkRequestInProgress) return;
    networkRequestInProgress = true;
    try {
      const response = await fetch(API + '/api/network', { cache: 'no-store' });
      const data = await response.json();
      if (!response.ok || !data.ok || !Array.isArray(data.urls)) throw new Error();
      const adapters = new Map();
      for (const adapter of data.urls) {
        const url = parseAdapterUrl(adapter.url);
        if (url) adapters.set(url.origin, { url, name: adapter.name });
      }
      if (!adapters.has(manualAdapterUrl)) manualAdapterUrl = '';
      if (!adapters.size) {
        selectedAdapterUrl = '';
        qrImage.hidden = true;
        qrImage.removeAttribute('src');
        $('#network-controls').hidden = true;
        networkStatus.textContent = '未发现可共享的局域网地址';
        return;
      }

      const hostAdapter = [...adapters.values()].find(({ url }) => url.hostname === location.hostname);
      const preferred = parseAdapterUrl(data.preferredUrl);
      const defaultUrl = hostAdapter ? hostAdapter.url.origin : preferred && preferred.origin;
      const selected = manualAdapterUrl || (adapters.has(defaultUrl) ? defaultUrl : adapters.keys().next().value);
      const entries = [...adapters.values()].map(({ url, name }) => [url.origin, name ? `${name} · ${url.host}` : url.host]);
      if (entries.length !== adapterSelect.options.length ||
          entries.some(([url, label], index) => adapterSelect.options[index].value !== url || adapterSelect.options[index].text !== label)) {
        adapterSelect.replaceChildren(...entries.map(([url, label]) => new Option(label, url)));
      }
      $('#network-controls').hidden = false;
      networkStatus.textContent = '';
      selectAdapter(selected);
    } catch (e) {
      networkStatus.textContent = '暂时无法更新局域网地址';
    } finally {
      networkRequestInProgress = false;
    }
  }

  $('#network-copy').addEventListener('click', async () => {
    if (!selectedAdapterUrl) return;
    const url = selectedAdapterUrl;
    if (navigator.clipboard) {
      try {
        await navigator.clipboard.writeText(url);
        toast('已复制链接');
        return;
      } catch (e) { /* 改用普通 HTTP 可用的方法 */ }
    }
    const field = document.createElement('textarea');
    field.value = url;
    field.readOnly = true;
    field.className = 'copy-field';
    const previousFocus = document.activeElement;
    document.body.appendChild(field);
    field.select();
    field.setSelectionRange(0, field.value.length);
    let copied = false;
    try { copied = document.execCommand('copy'); } catch (e) { /* 手动复制 */ }
    field.remove();
    previousFocus.focus();
    if (!copied) window.prompt('请手动复制链接', url);
    else toast('已复制链接');
  });

  // ---------------- WebSocket ----------------
  let ws = null;
  const pendingRoomJoins = [];

  function wsUrl() {
    return (location.protocol === 'https:' ? 'wss:' : 'ws:') + '//' + location.host + '/ws';
  }

  function sendHello() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'hello', deviceId, deviceName }));
    }
  }

  function subscribeRoom(code) {
    if (!code || !ws || ws.readyState !== WebSocket.OPEN || pendingRoomJoins.includes(code)) return false;
    ws.send(JSON.stringify({ type: 'join-room', code }));
    pendingRoomJoins.push(code);
    return true;
  }

  function connectWs() {
    try { ws = new WebSocket(wsUrl()); } catch (e) { setNetStatus(false); scheduleReconnect(); return; }
    const socket = ws;
    socket.onopen = () => {
      setNetStatus(true);
      sendHello();
      subscribeRoom(myRoomCode);
      subscribeRoom(currentRoomCode);
      refreshBroadcastList();
      refreshNetwork();
      refreshBroadcastList();
      if (currentRoomCode) refreshRoomList(currentRoomCode);
      if (myRoomCode) refreshRoomList(myRoomCode);
    };
    socket.onmessage = (ev) => {
      let msg;
      try { msg = JSON.parse(ev.data); } catch (e) { return; }
      handleWs(msg);
    };
    socket.onclose = () => {
      pendingRoomJoins.length = 0;
      setNetStatus(false);
      updateDevices([]);
      refreshBroadcastList();
      if (currentRoomCode) refreshRoomList(currentRoomCode);
      if (myRoomCode) refreshRoomList(myRoomCode);
      if (currentRoomCode) $('#recv-room-hint').textContent = '连接断开，正在重试；列表仍每几秒自动刷新。';
      scheduleReconnect();
    };
    socket.onerror = () => { try { socket.close(); } catch (e) { /* 忽略 */ } };
  }

  let reconnectTimer = null;
  function scheduleReconnect() {
    clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(connectWs, 2000);
  }

  function setNetStatus(ok) {
    const el = $('#net-status');
    const txt = $('#net-status-text');
    if (ok) {
      el.classList.remove('offline');
      txt.textContent = '已连接服务';
      const st = $('#recv-live-status');
      if (st) { st.classList.remove('offline'); st.lastElementChild.textContent = '已连接，列表会自动保持同步'; }
      $('#recv-room-hint').textContent = currentRoomCode ? '连接已恢复，正在同步接收码 ' + currentRoomCode + ' 的文件…' : $('#recv-room-hint').textContent;
    } else {
      el.classList.add('offline');
      txt.textContent = '连接断开，正在重连…';
      const st = $('#recv-live-status');
      if (st) { st.classList.add('offline'); st.lastElementChild.textContent = '连接断开；文件列表仍会自动刷新'; }
      $('#recv-room-hint').textContent = currentRoomCode ? '连接断开；接收码文件列表仍会自动刷新。' : '连接断开；可以先加入接收码，列表仍可手动同步。';
    }
  }

  // ---------------- 状态 ----------------
  // Shared lists contain broadcast data only; room data is kept separately by code.
  const liveBatches = new Map();
  // Each room code owns an independent batch map. Sender and receiver can
  // therefore display different rooms without overwriting each other's view.
  const roomLists = new Map();
  function roomMap(code) {
    if (!code) return new Map();
    if (!roomLists.has(code)) roomLists.set(code, new Map());
    return roomLists.get(code);
  }
  let currentRoomCode = null;
  let broadcastRevision = 0;
  let latestBroadcastRequest = 0;

  function showRoomFiles(code, files) {
    const batches = new Map((files || []).map(batch => [batch.id, batch]));
    roomLists.set(code, batches);
    if (code === currentRoomCode || code === myRoomCode) {
      renderSharedList('room');
      updateDeleteButtons();
    }
    if (code === currentRoomCode) {
      $('#recv-room-hint').textContent = '已加入接收码 ' + code + '，对方新上传的文件将实时出现。';
    }
  }

  function handleRoomJoined(msg) {
    const code = pendingRoomJoins.shift();
    if (!code) return;
    if (msg.ok) {
      if (msg.code !== code) return;
      roomLists.set(code, new Map((msg.files || []).map(batch => [batch.id, batch])));
      if (code === currentRoomCode || code === myRoomCode) {
        renderSharedList('room');
        updateDeleteButtons();
      }
      return;
    }
    const error = msg.error || '接收码已失效';
    if (code === currentRoomCode) {
      currentRoomCode = null;
      renderRoomRecv();
      $('#recv-room-hint').textContent = error;
    }
    if (code === myRoomCode) {
      myRoomCode = null;
      $('#room-start').classList.remove('hidden');
      $('#room-active').classList.add('hidden');
      $('#room-done').replaceChildren();
    }
    toast(error);
  }

  function handleWs(msg) {
    switch (msg.type) {
      case 'welcome':
        updateDevices(msg.devices);
        break;
      case 'devices':
        updateDevices(msg.devices);
        break;
      case 'file-arrived': {
        const b = msg.batch;
        if (!b) break;
        if (b.scope === 'broadcast') {
          broadcastRevision++;
          for (const file of b.files || []) hostSelectedFiles.delete(file.id);
          const isNew = !liveBatches.has(b.id);
          liveBatches.set(b.id, b);
          renderSharedList('broadcast');
          updateDeleteButtons();
        } else if (b.scope === 'room') {
          if (b.roomCode === currentRoomCode || b.roomCode === myRoomCode) {
            for (const file of b.files || []) hostSelectedFiles.delete(file.id);
            const room = roomMap(b.roomCode);
            const isNew = !room.has(b.id);
            room.set(b.id, b);
            renderSharedList('room');
            updateDeleteButtons();
            if (isNew) notifyArrived(b);
          }
          if (b.roomCode === myRoomCode) {
            renderSharedList('room');
            updateDeleteButtons();
          }
        }
        break;
      }
      case 'file-removed': {
        broadcastRevision++;
        const update = (map, render) => {
          const batch = map.get(msg.batchId);
          if (!batch) return;
          if (!msg.fileId) map.delete(msg.batchId);
          else {
            batch.files = (batch.files || []).filter(file => file.id !== msg.fileId);
            batch.fileCount = batch.files.length;
            batch.totalSize = batch.files.reduce((sum, file) => sum + (file.size || 0), 0);
            if (!batch.fileCount) map.delete(msg.batchId);
          }
          render();
        };
        update(liveBatches, () => renderSharedList('broadcast'));
        for (const map of roomLists.values()) update(map, () => {});
        renderSharedList('room');
        updateDeleteButtons();
        break;
      }
      case 'room-joined':
        handleRoomJoined(msg);
        break;
    }
  }

  function updateDevices(devices) {
    const n = Array.isArray(devices) ? devices.length : 0;
    $('#online-count').textContent = n;
    if (n === 0) {
      $('#recv-devices').innerHTML = '<span style="color:var(--text-3)">暂无其他在线设备</span>';
    } else {
      $('#recv-devices').textContent = devices.map(d => d.name).join('、');
    }
  }

  // ---------------- 视图切换 ----------------
  const views = { home: $('#view-home'), send: $('#view-send'), receive: $('#view-receive') };
  const viewStates = { send: null, receive: 'broadcast' };
  function go(view) {
    Object.entries(views).forEach(([k, el]) => el.classList.toggle('hidden', k !== view));
    if (isHostLoopback) {
      activeSharedScope = viewStates.receive || 'broadcast';
      renderSharedList('broadcast');
      renderSharedList('room');
      updateDeleteButtons();
      if (view === 'send') {
        const scope = viewStates.send;
        if (scope === 'broadcast') { renderSharedList('broadcast'); refreshBroadcastList(); }
        if (scope === 'room' && myRoomCode) { renderSharedList('room'); refreshRoomList(myRoomCode); }
      }
    }
    if (view === 'receive') {
      activeSharedScope = viewStates.receive || 'broadcast';
      refreshBroadcastList();
      if (activeSharedScope === 'room' && currentRoomCode) refreshRoomList(currentRoomCode);
    }
  }
  $$('[data-go]').forEach(btn => btn.addEventListener('click', () => go(btn.dataset.go)));

  // 标签切换
  $$('.tabs').forEach(tabs => {
    tabs.addEventListener('click', (e) => {
      const tab = e.target.closest('.tab');
      if (!tab) return;
      if (isHostLoopback) {
        hostSelectedFiles.clear();
        if (tabs.closest('.view').id === 'view-receive') {
          activeSharedScope = tab.dataset.tab === 'recv-room' ? 'room' : 'broadcast';
          viewStates.receive = activeSharedScope;
          if (activeSharedScope === 'broadcast') refreshBroadcastList();
          else if (currentRoomCode) refreshRoomList(currentRoomCode);
        } else if (tabs.closest('.view').id === 'view-send') {
          const sendScope = tab.dataset.tab === 'send-room' ? 'room' : 'broadcast';
          if (sendScope === 'room' && myRoomCode) refreshRoomList(myRoomCode);
          if (sendScope === 'broadcast') refreshBroadcastList();
        }
        updateDeleteButtons();
      }
      tabs.querySelectorAll('.tab').forEach(t => t.classList.toggle('active', t === tab));
      const panelId = tab.dataset.tab;
      $$('#' + tabs.closest('.view').id + ' .tab-panel').forEach(p => {
        p.classList.toggle('hidden', p.id !== panelId);
      });
    });
  });

  // ---------------- 文件选择 ----------------
  // 递归读取拖拽进来的文件夹结构（Chrome/Edge/桌面 Safari 支持）
  async function readEntry(entry, base) {
    if (entry.isFile) {
      return await new Promise((resolve) => {
        entry.file((file) => {
          resolve([{ file, relPath: base ? base + '/' + entry.name : entry.name, display: base ? base + '/' + entry.name : entry.name }]);
        }, () => resolve([]));
      });
    }
    if (entry.isDirectory) {
      const reader = entry.createReader();
      let batch = [];
      while (true) {
        const chunk = await new Promise((resolve) => reader.readEntries(resolve, () => resolve([])));
        if (!chunk.length) break;
        batch = batch.concat(chunk);
      }
      const results = [];
      for (const child of batch) {
        results.push(...await readEntry(child, base ? base + '/' + entry.name : entry.name));
      }
      return results;
    }
    return [];
  }

  function makePicker(pickFilesBtn, pickDirBtn, fileInput, dirInput, selectedEl, onClear) {
    let items = [];
    const state = { items };

    function render() {
      if (!items.length) { selectedEl.innerHTML = ''; return; }
      selectedEl.innerHTML = '';
      items.forEach((it, idx) => {
        const div = document.createElement('div');
        div.className = 'file-item';
        const isDir = it.relPath.includes('/');
        div.innerHTML = `
          <div class="fi-ico">${isDir ? '📁' : /\.zip$/i.test(it.relPath) ? '🗜️' : '📄'}</div>
          <div class="fi-main">
            <div class="fi-name">${escapeHtml(it.display)}</div>
            <div class="fi-meta">${fmtSize(it.size != null ? it.size : it.file.size)}${it.children ? ' · 共 ' + it.children + ' 项' : ''}</div>
          </div>
          <button class="fi-del" data-idx="${idx}" aria-label="移除">×</button>`;
        selectedEl.appendChild(div);
      });
      selectedEl.querySelectorAll('.fi-del').forEach(btn => {
        btn.addEventListener('click', () => {
          items.splice(Number(btn.dataset.idx), 1);
          render();
          onClear && onClear();
        });
      });
    }

    function addFiles(fileList, basePath, isDirMode) {
      const map = new Map();
      for (const f of fileList) {
        const rel = isDirMode ? f.webkitRelativePath : f.name;
        map.set(rel, f);
      }
      // 文件夹模式：汇总子项数
      for (const [rel, f] of map) {
        const item = { file: f, relPath: rel, display: rel };
        items.push(item);
      }
      render();
      onClear && onClear();
    }

    fileInput.addEventListener('change', () => {
      addFiles(fileInput.files, '', false);
      fileInput.value = '';
    });
    dirInput.addEventListener('change', () => {
      addFiles(dirInput.files, '', true);
      dirInput.value = '';
    });

    async function pickHostItems(folder) {
      try {
        const response = await fetch(API + '/api/host/select', {
          method: 'POST', headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ folder }),
        });
        const data = await response.json();
        if (!response.ok || !data.ok || !Array.isArray(data.files)) throw new Error(data.error || '无法选择文件');
        for (const file of data.files) items.push({ token: file.token, name: file.name, relPath: file.relPath || file.name, display: file.relPath || file.name, size: file.size });
        render();
        onClear && onClear();
      } catch (error) { toast('❌ ' + error.message); }
    }

    function supportsDir() {
      const i = document.createElement('input');
      return 'webkitdirectory' in i;
    }

    pickFilesBtn.addEventListener('click', () => {
      if (isHostLoopback) { pickHostItems(false); return; }
      fileInput.click();
    });
    pickDirBtn.addEventListener('click', () => {
      if (isHostLoopback) { pickHostItems(true); return; }
      if (!supportsDir()) {
        toast('当前浏览器（如 iOS Safari）不支持选择文件夹，请改用多选文件或使用电脑浏览器');
        return;
      }
      dirInput.click();
    });

    // 拖拽上传：支持文件与文件夹（文件夹通过 FileSystemEntry 递归读取）
    async function addFromDataTransfer(dt) {
      let newItems = [];
      const dtItems = dt && dt.items;
      if (dtItems && dtItems.length && typeof dtItems[0].webkitGetAsEntry === 'function') {
        const tasks = [];
        for (const it of dtItems) {
          if (it.kind !== 'file') continue;
          const entry = it.webkitGetAsEntry();
          if (entry) tasks.push(readEntry(entry, ''));
          else {
            const f = it.getAsFile();
            if (f) newItems.push({ file: f, relPath: f.name, display: f.name });
          }
        }
        const nested = await Promise.all(tasks);
        for (const arr of nested) newItems.push(...arr);
      } else if (dt && dt.files) {
        for (const f of dt.files) newItems.push({ file: f, relPath: f.name, display: f.name });
      }
      if (!newItems.length) return;
      items.push(...newItems);
      render();
      onClear && onClear();
    }

    return {
      get items() { return items; },
      addHostItems(files) { for (const file of files) items.push({ token: file.token, name: file.name, relPath: file.relPath || file.name, display: file.relPath || file.name, size: file.size }); render(); onClear && onClear(); },
      clear() { items = []; render(); },
      render,
      isEmpty() { return items.length === 0; },
      addFromDataTransfer,
    };
  }

  // 绑定拖拽区域
  function bindDropZone(zone, picker) {
    if (isHostLoopback) {
      zone.addEventListener('drop', e => { e.preventDefault(); toast('本机发送请使用原生文件/文件夹选择器；原文件由本机直接读取'); });
      zone.addEventListener('dragover', e => e.preventDefault());
      return;
    }
    zone.addEventListener('dragenter', (e) => {
      e.preventDefault();
      zone.classList.add('dragging');
    });
    zone.addEventListener('dragover', (e) => {
      e.preventDefault();
    });
    zone.addEventListener('dragleave', (e) => {
      if (!zone.contains(e.relatedTarget)) zone.classList.remove('dragging');
    });
    zone.addEventListener('drop', (e) => {
      e.preventDefault();
      zone.classList.remove('dragging');
      picker.addFromDataTransfer(e.dataTransfer).then(() => {
        if (!picker.isEmpty()) toast('已添加拖入的文件');
      });
    });
  }

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  // ---------------- 上传 ----------------
  function uploadFiles(items, url, progressFill, pctEl, progressBox, onDone, onError) {
    const fd = new FormData();
    // 文件夹场景：所有文件共享同一顶层目录
    const firstRel = items[0] ? items[0].relPath : '';
    let topName = '';
    if (firstRel.includes('/')) {
      const top = firstRel.split('/')[0];
      if (items.every(it => it.relPath.startsWith(top + '/'))) topName = top;
    }
    for (const it of items) {
      fd.append('files', it.file, it.file.name);
      fd.append('relativePath', it.relPath);
    }
    fd.append('topName', topName);
    fd.append('senderName', deviceName);

    const xhr = new XMLHttpRequest();
    xhr.open('POST', url);
    xhr.upload.onprogress = (e) => {
      if (!e.lengthComputable) return;
      const pct = Math.round(e.loaded / e.total * 100);
      progressFill.style.width = pct + '%';
      pctEl.textContent = pct + '%';
    };
    xhr.onload = () => {
      progressBox.classList.add('hidden');
      let data = null;
      try { data = JSON.parse(xhr.responseText); } catch (e) { /* 忽略 */ }
      if (xhr.status === 200 && data && data.ok) {
        onDone && onDone(data);
      } else {
        onError && onError(data && data.error ? data.error : '上传失败（' + xhr.status + '）');
      }
    };
    xhr.onerror = () => {
      progressBox.classList.add('hidden');
      onError && onError('网络错误，上传失败');
    };
    progressBox.classList.remove('hidden');
    progressFill.style.width = '0%';
    pctEl.textContent = '0%';
    xhr.send(fd);
  }

  // ---------------- 批次卡片 ----------------
  function batchCard(b, extraClass) {
    const files = Array.isArray(b.files) ? b.files : [];
    const div = document.createElement('div');
    div.className = 'recv-item' + (extraClass ? ' ' + extraClass : '');
    div.dataset.batchId = b.id;
    div._batch = b;
    div.innerHTML = `
      <div class="ri-head">
        <div class="ri-ico ${files.length <= 1 ? '' : 'batch'}">${files.length <= 1 ? '📄' : '📁'}</div>
        <div class="ri-main">
          <div class="ri-name">${escapeHtml(b.displayName)}</div>
          <div class="ri-meta"></div>
        </div>
      </div><div class="batch-files"></div>`;
    div.querySelector('.ri-meta').textContent = `${b.fileCount} 个文件 · ${fmtSize(b.totalSize)} · ${b.senderName} · ${fmtTime(b.uploadedAt)}`;
    const list = div.querySelector('.batch-files');
    files.forEach(file => {
      const row = document.createElement('div');
      row.className = 'batch-file';
      row.innerHTML = `<span class="batch-file-name"></span><span class="batch-file-size"></span><a class="dl-btn small">下载</a>`;
      row.querySelector('.batch-file-name').textContent = file.relPath || file.name;
      row.querySelector('.batch-file-size').textContent = fmtSize(file.size);
      row.querySelector('a').href = API + '/api/download/' + encodeURIComponent(file.id);
      row.querySelector('a').download = file.name || '';
      list.appendChild(row);
    });
    return div;
  }

  function emptyTip(text) {
    const tip = document.createElement('div');
    tip.className = 'empty-tip';
    tip.textContent = text;
    return tip;
  }

  // ---------------- 广播：接收 & 发送 ----------------
  // One canonical list per scope is rendered in both sender and receiver views.
  const hostSelectedFiles = new Set();
  let hostDeleting = false;
  let activeSharedScope = 'broadcast';

  function sharedBox(scope, view) {
    if (view === 'send') return scope === 'room' ? $('#room-done') : $('#live-done');
    return scope === 'room' ? $('#recv-room-files') : $('#recv-live-files');
  }

  function getSharedBatches(scope, view) {
    return scope === 'room' ? roomMap(view === 'send' ? myRoomCode : currentRoomCode) : liveBatches;
  }

  function selectedInScope(scope, view) {
    const batches = getSharedBatches(scope, view);
    const ids = new Set([...batches.values()].flatMap(batch => (batch.files || []).map(file => file.id)));
    return [...hostSelectedFiles].some(id => ids.has(id));
  }

  function updateDeleteButtons() {
    $$('.host-delete-selected').forEach(button => {
      button.disabled = hostDeleting || !selectedInScope(button.dataset.scope, button.dataset.view);
    });
  }

  function bulkDeleteButton(scope, view) {
    if (!isHostLoopback) return null;
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'ghost danger host-delete-selected';
    button.dataset.scope = scope;
    button.dataset.view = view;
    button.textContent = '删除选中文件';
    button.addEventListener('click', () => deleteHostSelection(scope, view));
    return button;
  }

  function renderSharedList(scope) {
    const noFiles = scope === 'room' ? '该接收码下暂无文件，等待发送方上传…' : '暂无文件。等待同一网络下的发送端上传…';
    for (const view of ['send', 'receive']) {
      const batches = getSharedBatches(scope, view);
      const box = sharedBox(scope, view);
      if (!box) continue;
      const ordered = [...batches.values()].sort((a, b) => a.uploadedAt - b.uploadedAt);
      box.replaceChildren(...(ordered.length ? ordered.map(batch => {
        const card = batchCard(batch, view === 'send' ? 'done-item' : '');
        if (isHostLoopback) {
          for (const [index, file] of (batch.files || []).entries()) {
            const row = card.querySelectorAll('.batch-file')[index];
            if (!row) continue;
            const checkbox = document.createElement('input');
            checkbox.type = 'checkbox';
            checkbox.className = 'host-select-file';
            checkbox.checked = hostSelectedFiles.has(file.id);
            checkbox.setAttribute('aria-label', '选择 ' + (file.relPath || file.name));
            checkbox.addEventListener('change', () => {
              if (checkbox.checked) hostSelectedFiles.add(file.id);
              else hostSelectedFiles.delete(file.id);
              updateDeleteButtons();
            });
            row.appendChild(checkbox);
          }
        }
        return card;
      }) : [emptyTip(noFiles)]));
      const remove = bulkDeleteButton(scope, view);
      if (remove) { remove.disabled = hostDeleting || !selectedInScope(scope, view); box.appendChild(remove); }
    }
  }

  async function deleteHostSelection(scope, view) {
    if (!isHostLoopback || hostDeleting || !selectedInScope(scope, view)) return;
    const batches = getSharedBatches(scope, view);
    const available = new Set([...batches.values()].flatMap(batch => (batch.files || []).map(file => file.id)));
    const ids = [...hostSelectedFiles].filter(id => available.has(id));
    if (!ids.length || !confirm(`确定从列表移除选中的 ${ids.length} 个文件？本机原文件只取消分享，不会删除；其他设备上传的副本会从主机删除。`)) return;
    hostDeleting = true; updateDeleteButtons();
    try {
      const response = await fetch(API + '/api/host/uploads/delete', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({ids}) });
      const data = await response.json();
      if (!response.ok || !data.ok) throw new Error(data.error || '删除失败');
      ids.forEach(id => hostSelectedFiles.delete(id));
      toast(`已从列表移除 ${data.deleted} 个文件${data.failed ? `，${data.failed} 个副本删除失败` : ''}`);
      refreshBroadcastList();
      if (currentRoomCode) refreshRoomList(currentRoomCode);
      if (myRoomCode && myRoomCode !== currentRoomCode) refreshRoomList(myRoomCode);
    } catch (error) { toast('❌ ' + error.message); }
    finally { hostDeleting = false; updateDeleteButtons(); }
  }

  function renderLiveRecv() { renderSharedList('broadcast'); }

  function renderLiveDone(b) { renderSharedList('broadcast'); }

  function notifyArrived(b) {
    toast('📥 收到新文件：' + b.displayName, 3200);
  }

  function refreshRoomList(code) {
    if (!code) return;
    fetch(API + '/api/room/files?code=' + encodeURIComponent(code), { cache: 'no-store' })
      .then(async response => { const data = await response.json(); if (!response.ok || !data.ok) throw new Error(data.error || '列表不可用'); return data; })
      .then(data => {
        if (!data.ok || (code !== currentRoomCode && code !== myRoomCode)) return;
        const batches = data.files || [];
        roomLists.set(code, new Map(batches.map(batch => [batch.id, batch])));
        if (code === currentRoomCode || code === myRoomCode) {
          renderSharedList('room');
          updateDeleteButtons();
        }
      }).catch(() => {});
  }

  function refreshBroadcastList() {
    const request = ++latestBroadcastRequest;
    const revision = broadcastRevision;
    fetch(API + '/api/broadcast/files', { cache: 'no-store' }).then(async response => {
      const data = await response.json();
      if (!response.ok || !data.ok) throw new Error(data.error || '列表不可用');
      if (request !== latestBroadcastRequest) return;
      if (!ws || ws.readyState !== 1) {
        $('#net-status').classList.remove('offline');
        $('#net-status-text').textContent = '已连接服务，列表定时刷新';
      }
      if (revision !== broadcastRevision) { refreshBroadcastList(); return; }
      liveBatches.clear();
      (data.files || []).forEach(batch => liveBatches.set(batch.id, batch));
      renderSharedList('broadcast');
      updateDeleteButtons();
      for (const card of $$('#live-done [data-batch-id]')) {
        if (!liveBatches.has(card.dataset.batchId)) card.remove();
      }
    }).catch(() => {});
  }
  const listRefreshTimer = setInterval(() => {
    refreshBroadcastList();
    if (currentRoomCode) refreshRoomList(currentRoomCode);
    if (myRoomCode && myRoomCode !== currentRoomCode) refreshRoomList(myRoomCode);
  }, 4000);
  window.addEventListener('focus', () => {
    refreshBroadcastList();
    if (currentRoomCode) refreshRoomList(currentRoomCode);
  });

  function sendHostItems(items, mode, code) {
    const first = items[0] || {};
    const firstRel = first.relPath || first.name || '';
    const top = firstRel.includes('/') ? firstRel.split('/')[0] : '';
    const topName = top && items.every(it => (it.relPath || it.name || '').startsWith(top + '/')) ? top : '';
    const url = API + '/api/host/share?mode=' + mode + (mode === 'room' ? '&code=' + encodeURIComponent(code) : '');
    fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ tokens: items.map(it => it.token), senderName: deviceName, topName }) })
      .then(async r => { const data = await r.json(); if (!r.ok || !data.ok) throw new Error(data.error || '发送失败'); return data; })
      .then(data => {
        items.length = 0;
        if (mode === 'broadcast') { livePicker.clear(); refreshBroadcastList(); toast('✅ 已发送：' + (data.displayName || '文件')); }
        else { roomPicker.clear(); refreshRoomList(code); toast('✅ 已发送到接收码 ' + code); }
      }).catch(error => toast('❌ ' + error.message)).finally(() => {
        $('#live-send').disabled = livePicker.isEmpty();
        $('#room-send').disabled = roomPicker.isEmpty() || !myRoomCode;
      });
  }

  // 广播发送
  const livePicker = makePicker(
    $('#live-pick-files'), $('#live-pick-dir'), $('#live-files'), $('#live-dir'),
    $('#live-selected'),
    () => $('#live-send').disabled = livePicker.isEmpty()
  );
  bindDropZone($('#send-live'), livePicker);
  $('#live-send').addEventListener('click', () => {
    if (livePicker.isEmpty()) return;
    if (isHostLoopback) { $('#live-send').disabled = true; sendHostItems(livePicker.items, 'broadcast'); return; }
    $('#live-send').disabled = true;
    uploadFiles(livePicker.items, API + '/api/upload?mode=broadcast',
      $('#live-progress .prog-fill'), $('#live-progress .pct'), $('#live-progress'),
      (data) => {
        livePicker.clear();
        refreshBroadcastList();
        $('#live-send').disabled = false;
        toast('✅ 已发送：' + data.displayName);
      },
      (err) => { $('#live-send').disabled = false; toast('❌ ' + err); }
    );
  });

  // ---------------- 点对点：发送 ----------------
  let myRoomCode = null;

  async function createRoom(excludeCode) {
    const url = API + '/api/room/create' + (excludeCode ? '?exclude=' + encodeURIComponent(excludeCode) : '');
    const r = await fetch(url, { method: 'POST' });
    const data = await r.json();
    if (!r.ok || !data.ok || !/^\d{4}$/.test(data.code || '')) throw new Error(data.error || '创建失败');
    return data.code;
  }

  async function switchSenderRoom(code) {
    const previousCode = myRoomCode;
    myRoomCode = code;
    $('#room-code-text').textContent = myRoomCode;
    $('#room-start').classList.add('hidden');
    $('#room-active').classList.remove('hidden');
    subscribeRoom(myRoomCode);
    if (!ws || ws.readyState !== WebSocket.OPEN) refreshRoomList(myRoomCode);
    renderSharedList('room');
    updateDeleteButtons();
    $('#room-send').disabled = roomPicker.isEmpty();
    toast(previousCode ? '接收码已刷新：' + myRoomCode : '接收码已生成：' + myRoomCode);
  }

  $('#room-create').addEventListener('click', async () => {
    try { await switchSenderRoom(await createRoom()); }
    catch (e) { toast('❌ ' + e.message); }
  });
  $('#room-refresh').addEventListener('click', async (event) => {
    if (!myRoomCode) return;
    const button = event.currentTarget;
    button.disabled = true;
    const oldCode = myRoomCode;
    try { await switchSenderRoom(await createRoom(oldCode)); }
    catch (e) { toast('❌ 刷新失败：' + e.message); }
    finally { button.disabled = false; }
  });

  const roomPicker = makePicker(
    $('#room-pick-files'), $('#room-pick-dir'), $('#room-files'), $('#room-dir'),
    $('#room-selected'),
    () => $('#room-send').disabled = roomPicker.isEmpty()
  );
  bindDropZone($('#send-room'), roomPicker);
  $('#room-send').addEventListener('click', () => {
    if (roomPicker.isEmpty() || !myRoomCode) return;
    if (isHostLoopback) { $('#room-send').disabled = true; sendHostItems(roomPicker.items, 'room', myRoomCode); return; }
    $('#room-send').disabled = true;
    const targetCode = myRoomCode;
    uploadFiles(roomPicker.items, API + '/api/upload?mode=room&code=' + targetCode,
      $('#room-progress .prog-fill'), $('#room-progress .pct'), $('#room-progress'),
      () => {
        roomPicker.clear();
        refreshRoomList(targetCode);
        $('#room-send').disabled = false;
        toast('✅ 已上传到接收码 ' + targetCode);
      },
      (err) => { $('#room-send').disabled = false; toast('❌ ' + err); }
    );
  });

  function renderRoomDone(b) { renderSharedList('room'); }

  // ---------------- 点对点：接收 ----------------
  $('#recv-room-join').addEventListener('click', async () => {
    const code = $('#recv-room-code').value.trim();
    if (!/^\d{4}$/.test(code)) { toast('请输入 4 位数字接收码'); return; }
    const joinButton = $('#recv-room-join');
    joinButton.disabled = true;
    try {
      const r = await fetch(API + '/api/room/join', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ code }),
      });
      const data = await r.json();
      if (!data.ok) {
        toast(data.error || '加入失败');
        return;
      }
      currentRoomCode = code;
      viewStates.receive = 'room';
      activeSharedScope = 'room';
      hostSelectedFiles.clear();
      showRoomFiles(code, data.files);
      renderSharedList('room');
      if (subscribeRoom(code) || pendingRoomJoins.includes(code)) {
        toast('已加入接收码 ' + code);
      } else {
        $('#recv-room-hint').textContent = '已加入接收码 ' + code + '，连接恢复后将自动接收新文件。';
        toast('已加入接收码 ' + code + '（实时更新需等服务恢复）');
      }
    } catch (e) {
      toast('❌ ' + e.message);
    } finally {
      joinButton.disabled = false;
    }
  });

  $('#recv-room-code').addEventListener('input', (e) => {
    e.target.value = e.target.value.replace(/\D/g, '').slice(0, 4);
  });

  function renderRoomRecv() { renderSharedList('room'); }

  // ---------------- 启动 ----------------
  connectWs();
  refreshBroadcastList();
  refreshNetwork();
  window.addEventListener('focus', refreshNetwork);
  setInterval(refreshNetwork, 15000);
})();
