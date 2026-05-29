// SMS Forwarder - Frontend Application
const API = '/api';

// Navigation
document.addEventListener('DOMContentLoaded', () => {
    initNavigation();
    loadStatus();
    loadConfig();
    loadKeywords();
    loadAutostartStatus();
    loadPlatformInfo();

    setInterval(() => {
        if (document.getElementById('page-dashboard').classList.contains('active')) {
            loadStatus();
        }
    }, 5000);

    setInterval(() => {
        if (document.getElementById('page-logs').classList.contains('active')) {
            loadLogs();
        }
    }, 3000);
});

function initNavigation() {
    document.querySelectorAll('.nav-item').forEach(item => {
        item.addEventListener('click', (e) => {
            e.preventDefault();
            navigateTo(item.dataset.page);
        });
    });
    const hash = window.location.hash.slice(1) || 'dashboard';
    navigateTo(hash);
}

function navigateTo(page) {
    document.querySelectorAll('.nav-item').forEach(item => {
        item.classList.toggle('active', item.dataset.page === page);
    });
    document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
    const target = document.getElementById('page-' + page);
    if (target) target.classList.add('active');
    window.location.hash = page;
    switch (page) {
        case 'dashboard': loadStatus(); refreshRecentSMS(); loadSMSUsage(); break;
        case 'forward': loadKeywords(); loadForwardLogs(); break;
        case 'sms': loadSMS(); loadSMSUsage(); break;
        case 'calls': loadCalls(); break;
        case 'phonebook': loadPhonebook(); break;
        case 'diagnostics': refreshDevices(); loadPlatformInfo(); break;
        case 'settings': loadConfigToForm(); loadAutoCleanConfig(); break;
        case 'autostart': loadAutostartStatus(); break;
        case 'logs': loadLogs(); break;
    }
}

async function apiGet(path) {
    try {
        const res = await fetch(API + path);
        if (!res.ok) throw new Error(await res.text());
        return await res.json();
    } catch (err) { console.error('API:', err); return null; }
}

async function apiPost(path, data) {
    try {
        const res = await fetch(API + path, {
            method: 'POST', headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data)
        });
        if (!res.ok) { const e = await res.json().catch(()=>({})); throw new Error(e.error||res.statusText); }
        return await res.json();
    } catch (err) { console.error('API:', err); throw err; }
}

// ── Status / Dashboard ──────────────────────────────────────────────────────

async function loadStatus() {
    // Use server-injected initial data for instant display
    let data = window.__INIT__;
    if (!data) {
        data = await apiGet('/status');
        if (!data) return;
    }

    const dot = document.getElementById('statusDot');
    const statusText = document.getElementById('statusText');
    const cardStatus = document.getElementById('cardStatus');
    const signalFill = document.getElementById('signalFill');
    const cardOperator = document.getElementById('cardOperator');
    const cardNetwork = document.getElementById('cardNetwork');
    const cardIMEI = document.getElementById('cardIMEI');

    if (data.running && data.connected !== false) {
        dot.innerHTML = '<span class="dot online"></span>';
        statusText.textContent = '已连接';
        cardStatus.innerHTML = data.polling
            ? '<span class="status-led led-green"></span> 运行中'
            : '<span class="status-led led-yellow"></span> 已连接';
    } else if (data.running && data.connected === false) {
        dot.innerHTML = '<span class="dot offline"></span>';
        statusText.textContent = '模组断开';
        cardStatus.innerHTML = '<span class="status-led led-red"></span> 模组断开';
    } else {
        dot.innerHTML = '<span class="dot offline"></span>';
        statusText.textContent = '未连接';
        cardStatus.innerHTML = '<span class="status-led led-red"></span> 未连接';
    }

    if (data.signal !== undefined) {
        const pct = Math.min(100, (data.signal / 31) * 100);
        signalFill.style.width = pct + '%';
        signalFill.nextElementSibling.textContent = data.signal + '/31';
    }

    cardOperator.textContent = data.operator || '-';
    cardNetwork.textContent = data.network || '-';
    cardIMEI.textContent = data.imei || '-';

    // Show recovery banner if not connected or modem disconnected
    const banner = document.getElementById('recoverBanner');
    if (banner) {
        banner.style.display = (!data.running || data.connected === false) ? '' : 'none';
    }
}

// ── SMS ─────────────────────────────────────────────────────────────────────

async function loadSMS() {
    const data = await apiGet('/sms');
    if (!data) return;
    renderSMSList('allSMSList', data);
    filterSMS();
}

async function refreshRecentSMS() {
    const data = await apiGet('/sms');
    if (!data) return;
    renderSMSList('recentSMSList', data.slice(0, 10));
    document.getElementById('cardSMSCount').textContent = data.length;
}

function renderSMSList(containerId, messages) {
    const container = document.getElementById(containerId);
    if (!messages || messages.length === 0) {
        container.innerHTML = '<div class="empty-state">暂无短信</div>';
        return;
    }
    container.innerHTML = messages.map(msg => `
        <div class="sms-item" data-phone="${escHtml(msg.phone)}" data-content="${escHtml(msg.content||'')}">
            <div class="sms-header">
                <span class="sms-phone"><span class="sms-arrow-in">&#8592;</span> ${escHtml(msg.phone)}</span>
                <span class="sms-time">${formatTime(msg.timestamp)}</span>
            </div>
            <div class="sms-content">${escHtml(msg.content||'')}</div>
            <button class="btn-delete-sms" onclick="deleteSMS('${escAttr(msg.phone)}')" title="删除">&times;</button>
        </div>
    `).join('');
}

function filterSMS() {
    const search = document.getElementById('smsSearch').value.toLowerCase();
    document.querySelectorAll('#allSMSList .sms-item').forEach(item => {
        const phone = item.dataset.phone?.toLowerCase() || '';
        const content = item.dataset.content?.toLowerCase() || '';
        item.style.display = (phone.includes(search) || content.includes(search)) ? '' : 'none';
    });
}

async function deleteSMS(phone) {
    if (!confirm('确定删除来自 ' + phone + ' 的短信？')) return;
    try {
        await apiPost('/sms/delete', { phone, index: 0 });
        showToast('已删除');
        loadSMS();
        refreshRecentSMS();
        loadSMSUsage();
    } catch (err) { alert('删除失败: ' + err.message); }
}

// ── Batch Clear ─────────────────────────────────────────────────────────────

async function clearAllSMS() {
    const usage = await apiGet('/sms/usage');
    const count = usage?.used || '?';
    if (!confirm(`确定清空模块中全部 ${count} 条短信？\n此操作不可恢复！`)) return;
    try {
        const result = await apiPost('/sms/clear', {});
        showToast(`已清空 ${result.deleted || 0} 条短信`);
        loadSMS();
        refreshRecentSMS();
        loadSMSUsage();
    } catch (err) { alert('清空失败: ' + err.message); }
}

// ── SMS Storage Usage ───────────────────────────────────────────────────────

async function loadSMSUsage() {
    const data = await apiGet('/sms/usage');
    if (!data) return;
    const used = data.used || 0;
    const total = data.total || 180;
    const pct = total > 0 ? Math.round((used / total) * 100) : 0;
    
    const el = document.getElementById('storageUsed');
    if (el) el.textContent = used;
    const totalEl = document.getElementById('storageTotal');
    if (totalEl) totalEl.textContent = total;
    const pctEl = document.getElementById('storagePct');
    if (pctEl) {
        pctEl.textContent = `${pct}%`;
        pctEl.style.color = pct > 80 ? 'var(--danger)' : pct > 60 ? 'var(--warning)' : 'var(--text-muted)';
    }
    const fillEl = document.getElementById('storageBarFill');
    if (fillEl) {
        fillEl.style.width = `${pct}%`;
        fillEl.style.background = pct > 80 ? 'var(--danger)' : pct > 60 ? 'var(--warning)' : 'var(--accent)';
    }
    // Also update dashboard card
    const countEl = document.getElementById('cardSMSCount');
    if (countEl) countEl.textContent = `${used}/${total}`;
}

// ── Auto Cleanup Config ─────────────────────────────────────────────────────

function loadAutoCleanConfig() {
    apiGet('/config').then(data => {
        if (!data) return;
        const ac = data.auto_cleanup || {};
        document.getElementById('cfgAutoCleanEnabled').checked = ac.enabled !== false;
        document.getElementById('cfgAutoCleanThreshold').value = ac.threshold || 160;
        updateAutoCleanUI();
    });
}

function updateAutoCleanUI() {
    const enabled = document.getElementById('cfgAutoCleanEnabled').checked;
    document.getElementById('cfgAutoCleanThreshold').disabled = !enabled;
}

async function saveAutoCleanConfig() {
    const current = await apiGet('/config');
    if (!current) return;
    current.auto_cleanup = {
        enabled: document.getElementById('cfgAutoCleanEnabled').checked,
        threshold: parseInt(document.getElementById('cfgAutoCleanThreshold').value) || 160,
        max_age_hours: 24,
        max_count: 150
    };
    await apiPost('/config', current);
    showToast('自动清理设置已保存');
}

async function sendSMS() {
    const number = document.getElementById('sendNumber').value.trim();
    const content = document.getElementById('sendContent').value.trim();
    if (!number || !content) { alert('请填写号码和内容'); return; }
    try {
        await apiPost('/sms/send', { number, content });
        showToast('短信已发送');
        document.getElementById('sendContent').value = '';
        updateCharCount();
    } catch (err) { alert('发送失败: ' + err.message); }
}

function updateCharCount() {
    document.getElementById('charCount').textContent = document.getElementById('sendContent').value.length;
}

// ── Keywords ────────────────────────────────────────────────────────────────

async function loadKeywords() {
    const data = await apiGet('/keywords');
    if (!data) return;
    const list = document.getElementById('keywordList');
    const kws = data.keywords || data;
    if (!kws || kws.length === 0) {
        list.innerHTML = '<span style="color:var(--text-muted);font-size:13px">暂无关键词</span>';
        return;
    }
    list.innerHTML = kws.map(kw => `
        <span class="tag">${escHtml(kw)}<span class="tag-remove" onclick="removeKeyword('${escAttr(kw)}')">&times;</span></span>
    `).join('');
}

async function addKeyword() {
    const input = document.getElementById('keywordInput');
    const keyword = input.value.trim();
    if (!keyword) return;
    try { await apiPost('/keywords/add', { keyword }); input.value = ''; loadKeywords(); }
    catch (err) { alert('添加失败: ' + err.message); }
}

async function removeKeyword(keyword) {
    try { await apiPost('/keywords/del', { keyword }); loadKeywords(); }
    catch (err) { alert('删除失败: ' + err.message); }
}

// ── Config ──────────────────────────────────────────────────────────────────

async function loadConfig() {
    const data = await apiGet('/config');
    if (!data) return;
    document.getElementById('filterMode').value = data.filter?.mode || 'whitelist';
    document.getElementById('forwardLinks').checked = data.filter?.forward_links || false;
    document.getElementById('forwardLinksOnly').checked = data.filter?.forward_links_only || false;
    document.getElementById('forwardTemplate').value = data.filter?.forward_template || 'default';
    document.getElementById('dingEnabled').checked = data.dingtalk?.enabled || false;
    document.getElementById('dingMode').value = data.dingtalk?.mode || 'keyword';
    document.getElementById('dingToken').value = data.dingtalk?.token || '';
    document.getElementById('dingSecret').value = data.dingtalk?.secret || '';
    document.getElementById('dingKeyword').value = data.dingtalk?.keyword || '短信';
    toggleDingKeyword(data.dingtalk?.mode || 'keyword');
    document.getElementById('wechatEnabled').checked = data.wechat?.enabled || false;
    document.getElementById('wechatKey').value = data.wechat?.key || '';
}

function loadConfigToForm() {
    apiGet('/config').then(data => {
        if (!data) return;
        const portEl = document.getElementById('cfgSerialPort');
        const savedPort = data.serial?.port || 'auto';
        // Ensure the saved port is in the options
        let found = false;
        for (const opt of portEl.options) {
            if (opt.value === savedPort) { found = true; break; }
        }
        if (!found) {
            portEl.appendChild(new Option(savedPort, savedPort));
        }
        portEl.value = savedPort;
        document.getElementById('cfgBaudrate').value = data.serial?.baudrate || 115200;
        document.getElementById('cfgPolling').value = data.polling_sec || 1;
    });
    // Scan for available ports
    refreshSerialPorts();
}

async function refreshSerialPorts() {
    const data = await apiGet('/devices');
    if (!data || !data.available_ports) return;
    const portEl = document.getElementById('cfgSerialPort');
    const current = portEl.value;
    // Rebuild options
    portEl.innerHTML = '<option value="auto">auto（自动检测）</option>';
    for (const p of data.available_ports) {
        portEl.appendChild(new Option(p, p));
    }
    // Restore current selection
    portEl.value = current || 'auto';
}

async function saveSerialConfig() {
    const config = {
        serial: {
            port: document.getElementById('cfgSerialPort').value || 'auto',
            baudrate: parseInt(document.getElementById('cfgBaudrate').value) || 115200
        }
    };
    const current = await apiGet('/config');
    if (current) {
        Object.assign(current, config);
        await apiPost('/config', current);
        showToast('串口设置已保存，需重启应用生效');
    }
}

async function saveAllSettings() {
    const config = {
        serial: { port: document.getElementById('cfgSerialPort').value.trim()||'auto',
                  baudrate: parseInt(document.getElementById('cfgBaudrate').value)||115200 },
        polling_sec: parseInt(document.getElementById('cfgPolling').value)||2
    };
    const current = await apiGet('/config');
    if (current) { Object.assign(current, config); await apiPost('/config', current); showToast('设置已保存'); }
}

async function updateFilterMode() {
    const current = await apiGet('/config');
    if (current) { current.filter.mode = document.getElementById('filterMode').value; await apiPost('/config', current); }
}
async function updateFilterOptions() {
    const current = await apiGet('/config');
    if (current) { current.filter.forward_links = document.getElementById('forwardLinks').checked;
                   current.filter.forward_links_only = document.getElementById('forwardLinksOnly').checked;
                   current.filter.forward_template = document.getElementById('forwardTemplate').value;
                   await apiPost('/config', current); }
}
async function updateWebhookConfig() {
    const current = await apiGet('/config');
    if (current) {
        current.dingtalk.enabled = document.getElementById('dingEnabled').checked;
        current.dingtalk.mode = document.getElementById('dingMode').value;
        current.dingtalk.token = document.getElementById('dingToken').value.trim();
        current.dingtalk.secret = document.getElementById('dingSecret').value.trim();
        current.dingtalk.keyword = document.getElementById('dingKeyword').value.trim() || '短信';
        current.wechat.enabled = document.getElementById('wechatEnabled').checked;
        current.wechat.key = document.getElementById('wechatKey').value.trim();
        await apiPost('/config', current);
    }
}

async function exportConfig() {
    try {
        const res = await fetch(API + '/config/export');
        const blob = await res.blob();
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob); a.download = 'sms-forwarder-config.json'; a.click();
        URL.revokeObjectURL(a.href);
    } catch (err) { alert('导出失败: ' + err.message); }
}
function importConfig() { document.getElementById('configFileInput').click(); }
async function handleConfigFile(input) {
    const file = input.files[0];
    if (!file) return;
    try {
        const text = await file.text();
        await apiPost('/config/import', { data: text });
        showToast('配置已导入'); loadConfig(); loadKeywords(); input.value = '';
    } catch (err) { alert('导入失败: ' + err.message); }
}
async function resetConfig() {
    if (!confirm('确定要重置所有配置为默认值吗？')) return;
    try { await apiPost('/config/reset', {}); showToast('配置已重置'); loadConfig(); loadKeywords(); }
    catch (err) { alert('重置失败: ' + err.message); }
}
function toggleDingKeyword(mode) {
    const kw = document.getElementById('dingKeyword');
    kw.style.display = (mode === 'keyword') ? '' : 'none';
}
// Hook dingMode change to toggle keyword visibility
document.addEventListener('DOMContentLoaded', () => {
    const dingMode = document.getElementById('dingMode');
    if (dingMode) {
        dingMode.addEventListener('change', function() {
            toggleDingKeyword(this.value);
        });
        // Also listen via onchange attribute
        const origOnChange = dingMode.onchange;
        dingMode.onchange = function(e) {
            toggleDingKeyword(this.value);
            if (typeof origOnChange === 'function') origOnChange.call(this, e);
        };
    }
});

async function testWebhook(platform) {
    try { await apiPost('/webhook/test', { platform }); showToast('测试消息已发送'); }
    catch (err) { alert('测试失败: ' + err.message); }
}

// ── Dialer ──────────────────────────────────────────────────────────────────

function dialDigit(digit) {
    document.getElementById('dialNumber').value += digit;
}
function dialClear() {
    document.getElementById('dialNumber').value = '';
}

async function dialCall() {
    const number = document.getElementById('dialNumber').value.trim();
    if (!number) { alert('请输入号码'); return; }
    try {
        const result = await apiPost('/call/dial', { number });
        showToast('正在呼叫 ' + number);
        document.getElementById('dialNumber').value = '';
        document.getElementById('lastDialStatus').textContent = '上次拨打: ' + number + ' (' + new Date().toLocaleTimeString('zh-CN') + ')';
        loadCalls();
    } catch (err) { alert('拨号失败: ' + err.message); }
}

async function dialHangup() {
    try {
        await apiPost('/call/hangup', {});
        showToast('已挂断');
    } catch (err) { alert('挂断失败: ' + err.message); }
}

// ── Call History ────────────────────────────────────────────────────────────

async function loadCalls() {
    const data = await apiGet('/calls');
    const container = document.getElementById('callHistoryList');
    if (!data || data.length === 0) {
        container.innerHTML = '<div class="empty-state">暂无通话记录</div>';
        return;
    }
    container.innerHTML = data.slice(0, 50).map(call => `
        <div class="call-item">
            <span class="call-type ${call.type}">${call.type === 'dial' ? '&#8593;' : '&#8595;'}</span>
            <span class="call-number">${escHtml(call.number)}</span>
            <span class="call-time">${call.time}</span>
            <span class="call-dur">${call.duration}</span>
        </div>
    `).join('');
}

// ── Phonebook ───────────────────────────────────────────────────────────────

async function loadPhonebook() {
    const data = await apiGet('/phonebook');
    const container = document.getElementById('phonebookList');
    if (!data || data.length === 0) {
        container.innerHTML = '<div class="empty-state">暂无联系人</div>';
        return;
    }
    container.innerHTML = data.map(entry => `
        <div class="contact-item">
            <span class="contact-name">${escHtml(entry.name)}</span>
            <span class="contact-number">${escHtml(entry.phone)}</span>
        </div>
    `).join('');
}

async function addPhonebookEntry() {
    const name = document.getElementById('pbName').value.trim();
    const number = document.getElementById('pbNumber').value.trim();
    if (!name || !number) { alert('请填写姓名和号码'); return; }
    try {
        await apiPost('/phonebook/add', { name, phone: number });
        document.getElementById('pbName').value = '';
        document.getElementById('pbNumber').value = '';
        loadPhonebook();
        showToast('联系人已添加');
    } catch (err) { alert('添加失败: ' + err.message); }
}

// ── Devices ─────────────────────────────────────────────────────────────────

async function refreshDevices() {
    const data = await apiGet('/devices');
    if (!data) return;
    const grid = document.getElementById('deviceInfoGrid');
    if (data.device) {
        grid.innerHTML = `
            <div class="info-item"><span class="label">信号:</span><span class="value">${data.device.signal||'-'}</span></div>
            <div class="info-item"><span class="label">网络:</span><span class="value">${data.device.network||'-'}</span></div>
            <div class="info-item"><span class="label">IMEI:</span><span class="value">${data.device.imei||'-'}</span></div>
            <div class="info-item"><span class="label">运营商:</span><span class="value">${data.device.operator||'-'}</span></div>
            <div class="info-item"><span class="label">SIM:</span><span class="value">${data.device.sim||'-'}</span></div>
            <div class="info-item"><span class="label">时间:</span><span class="value">${data.device.time||'-'}</span></div>`;
    }
}

async function loadPlatformInfo() {
    const data = await apiGet('/platform');
    if (data) { document.getElementById('platformInfo').textContent = `操作系统: ${data.os} | 架构: ${data.arch}`; }
}

async function bindDriver() {
    try { await apiPost('/driver/bind', {}); alert('驱动绑定成功！'); }
    catch (err) { alert('绑定失败: ' + err.message); }
}

async function recoverModem() {
    if (!confirm('将重置 USB 设备并重新绑定驱动，可能需要 10 秒，确定继续？')) return;
    const btn = document.querySelector('#recoverBanner .btn');
    btn.disabled = true;
    btn.textContent = '恢复中...';
    try {
        const result = await apiPost('/recover', {});
        showToast('模组已恢复: ' + (result.port || 'OK'));
        setTimeout(loadStatus, 3000);
    } catch (err) {
        alert('恢复失败: ' + err.message);
    }
    btn.disabled = false;
    btn.textContent = '🔧 恢复连接';
}

// ── Autostart ───────────────────────────────────────────────────────────────

async function loadAutostartStatus() {
    const data = await apiGet('/autostart');
    if (!data) return;
    const badge = document.getElementById('autostartBadge');
    const toggle = document.getElementById('autostartToggle');
    if (data.enabled) {
        badge.innerHTML = '<span class="status-led led-green"></span> 已启用';
        toggle.textContent = '禁用自启动';
    } else {
        badge.innerHTML = '<span class="status-led led-red"></span> 已禁用';
        toggle.textContent = '启用自启动';
    }
    document.getElementById('autostartPlatform').textContent = '平台: ' + data.platform;
}

async function toggleAutostart() {
    try {
        const result = await apiPost('/autostart', {});
        loadAutostartStatus();
        showToast(result.enabled ? '自启动已启用' : '自启动已禁用');
    } catch (err) { alert('操作失败: ' + err.message); }
}

// ── Logs ────────────────────────────────────────────────────────────────────

async function loadLogs() {
    const data = await apiGet('/logs');
    const container = document.getElementById('logContainer');
    if (!data || data.length === 0) { container.innerHTML = '<div class="empty-state">暂无日志</div>'; return; }
    container.innerHTML = data.slice(0, 100).map(log => `
        <div class="log-entry">
            <span class="log-time">${log.time}</span>
            <span class="log-level ${log.level}">${log.level}</span>
            <span class="log-message">${escHtml(log.message)}</span>
        </div>
    `).join('');
}

function loadForwardLogs() {
    document.getElementById('forwardLog').innerHTML = '<div class="empty-state">转发日志存储在内存中</div>';
}

// ── Toast ───────────────────────────────────────────────────────────────────

function showToast(msg) {
    let el = document.getElementById('globalToast');
    if (!el) {
        el = document.createElement('div');
        el.id = 'globalToast';
        el.className = 'global-toast';
        document.body.appendChild(el);
    }
    el.textContent = msg;
    el.classList.add('show');
    clearTimeout(el._timer);
    el._timer = setTimeout(() => el.classList.remove('show'), 2500);
}

// ── Utils ───────────────────────────────────────────────────────────────────

function formatTime(timestamp) {
    if (!timestamp) return '-';
    try {
        const d = new Date(timestamp);
        const diff = Date.now() - d;
        if (diff < 60000) return '刚刚';
        if (diff < 3600000) return Math.floor(diff/60000) + '分钟前';
        if (diff < 86400000) return Math.floor(diff/3600000) + '小时前';
        return d.toLocaleString('zh-CN');
    } catch { return timestamp; }
}

function escHtml(str) { return str ? str.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;') : ''; }
function escAttr(str) { return str ? str.replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/'/g,'&#39;') : ''; }
