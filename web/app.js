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

    // Auto-refresh dashboard every 5 seconds
    setInterval(() => {
        if (document.getElementById('page-dashboard').classList.contains('active')) {
            loadStatus();
        }
    }, 5000);

    // Auto-refresh logs every 3 seconds when on logs page
    setInterval(() => {
        if (document.getElementById('page-logs').classList.contains('active')) {
            loadLogs();
        }
    }, 3000);
});

// Navigation
function initNavigation() {
    document.querySelectorAll('.nav-item').forEach(item => {
        item.addEventListener('click', (e) => {
            e.preventDefault();
            const page = item.dataset.page;
            navigateTo(page);
        });
    });

    // Load default page from hash
    const hash = window.location.hash.slice(1) || 'dashboard';
    navigateTo(hash);
}

function navigateTo(page) {
    // Update nav
    document.querySelectorAll('.nav-item').forEach(item => {
        item.classList.toggle('active', item.dataset.page === page);
    });

    // Update page visibility
    document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
    const target = document.getElementById('page-' + page);
    if (target) target.classList.add('active');

    window.location.hash = page;

    // Load page-specific data
    switch (page) {
        case 'dashboard': loadStatus(); refreshRecentSMS(); break;
        case 'forward': loadKeywords(); loadForwardLogs(); break;
        case 'sms': loadSMS(); break;
        case 'calls': loadCalls(); break;
        case 'phonebook': loadPhonebook(); break;
        case 'diagnostics': refreshDevices(); loadPlatformInfo(); break;
        case 'settings': loadConfigToForm(); break;
        case 'autostart': loadAutostartStatus(); break;
        case 'logs': loadLogs(); break;
    }
}

// API Helpers
async function apiGet(path) {
    try {
        const res = await fetch(API + path);
        if (!res.ok) throw new Error(await res.text());
        return await res.json();
    } catch (err) {
        console.error('API Error:', err);
        return null;
    }
}

async function apiPost(path, data) {
    try {
        const res = await fetch(API + path, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data)
        });
        if (!res.ok) {
            const errData = await res.json().catch(() => ({}));
            throw new Error(errData.error || res.statusText);
        }
        return await res.json();
    } catch (err) {
        console.error('API Error:', err);
        throw err;
    }
}

// Status
async function loadStatus() {
    const data = await apiGet('/status');
    if (!data) return;

    const dot = document.getElementById('statusDot');
    const statusText = document.getElementById('statusText');
    const cardStatus = document.getElementById('cardStatus');
    const signalFill = document.getElementById('signalFill');
    const cardOperator = document.getElementById('cardOperator');
    const cardNetwork = document.getElementById('cardNetwork');
    const cardIMEI = document.getElementById('cardIMEI');

    if (data.running) {
        dot.innerHTML = '<span class="dot online"></span>';
        statusText.textContent = '已连接';
        cardStatus.textContent = data.polling ? '🟢 运行中' : '🟡 已连接';
    } else {
        dot.innerHTML = '<span class="dot offline"></span>';
        statusText.textContent = '未连接';
        cardStatus.textContent = '🔴 未连接';
    }

    if (data.signal !== undefined) {
        const pct = Math.min(100, (data.signal / 31) * 100);
        signalFill.style.width = pct + '%';
        signalFill.nextElementSibling.textContent = data.signal + '/31';
    }

    cardOperator.textContent = data.operator || '-';
    cardNetwork.textContent = data.network || '-';
    cardIMEI.textContent = data.imei || '-';
}

// Service Control
async function startService() {
    try {
        await apiPost('/start', {});
        loadStatus();
    } catch (err) {
        alert('启动失败: ' + err.message);
    }
}

async function stopService() {
    try {
        await apiPost('/stop', {});
        loadStatus();
    } catch (err) {
        alert('停止失败: ' + err.message);
    }
}

// SMS
async function loadSMS() {
    const data = await apiGet('/sms');
    if (!data) return;

    renderSMSList('allSMSList', data);
    filterSMS();
}

async function refreshRecentSMS() {
    const data = await apiGet('/sms');
    if (!data) return;

    const recent = data.slice(0, 10);
    renderSMSList('recentSMSList', recent);
    document.getElementById('cardSMSCount').textContent = data.length;
}

function renderSMSList(containerId, messages) {
    const container = document.getElementById(containerId);
    if (!messages || messages.length === 0) {
        container.innerHTML = '<div class="empty-state">暂无短信</div>';
        return;
    }

    container.innerHTML = messages.map(msg => `
        <div class="sms-item" data-phone="${escHtml(msg.phone)}" data-content="${escHtml(msg.content)}">
            <div class="sms-header">
                <span class="sms-phone">${msg.direction === 'out' ? '→ ' : '← '}${escHtml(msg.phone)}</span>
                <span class="sms-time">${formatTime(msg.timestamp)}</span>
            </div>
            <div class="sms-content">${escHtml(msg.content)}</div>
            <div class="sms-meta">${msg.status} ${msg.is_read ? '✓' : '●'}</div>
        </div>
    `).join('');
}

function filterSMS() {
    const search = document.getElementById('smsSearch').value.toLowerCase();
    const items = document.querySelectorAll('#allSMSList .sms-item');
    items.forEach(item => {
        const phone = item.dataset.phone?.toLowerCase() || '';
        const content = item.dataset.content?.toLowerCase() || '';
        const match = phone.includes(search) || content.includes(search);
        item.style.display = match ? '' : 'none';
    });
}

async function sendSMS() {
    const number = document.getElementById('sendNumber').value.trim();
    const content = document.getElementById('sendContent').value.trim();

    if (!number || !content) {
        alert('请填写号码和内容');
        return;
    }

    try {
        await apiPost('/sms/send', { number, content });
        alert('发送成功');
        document.getElementById('sendContent').value = '';
        updateCharCount();
    } catch (err) {
        alert('发送失败: ' + err.message);
    }
}

function updateCharCount() {
    const len = document.getElementById('sendContent').value.length;
    document.getElementById('charCount').textContent = len;
}

// Keywords
async function loadKeywords() {
    const data = await apiGet('/keywords');
    if (!data) return;

    const list = document.getElementById('keywordList');
    if (data.length === 0) {
        list.innerHTML = '<span style="color:var(--text-muted);font-size:13px">暂无关键词</span>';
        return;
    }

    list.innerHTML = data.map(kw => `
        <span class="tag">
            ${escHtml(kw)}
            <span class="tag-remove" onclick="removeKeyword('${escAttr(kw)}')">×</span>
        </span>
    `).join('');
}

async function addKeyword() {
    const input = document.getElementById('keywordInput');
    const keyword = input.value.trim();
    if (!keyword) return;

    try {
        await apiPost('/keywords/add', { keyword });
        input.value = '';
        loadKeywords();
    } catch (err) {
        alert('添加失败: ' + err.message);
    }
}

async function removeKeyword(keyword) {
    try {
        await apiPost('/keywords/del', { keyword });
        loadKeywords();
    } catch (err) {
        alert('删除失败: ' + err.message);
    }
}

// Config
async function loadConfig() {
    const data = await apiGet('/config');
    if (!data) return;

    // Update filter config
    document.getElementById('filterMode').value = data.filter?.mode || 'whitelist';
    document.getElementById('forwardLinks').checked = data.filter?.forward_links || false;
    document.getElementById('forwardLinksOnly').checked = data.filter?.forward_links_only || false;

    // Update webhook config
    document.getElementById('dingEnabled').checked = data.dingtalk?.enabled || false;
    document.getElementById('dingMode').value = data.dingtalk?.mode || 'keyword';
    document.getElementById('dingToken').value = data.dingtalk?.token || '';
    document.getElementById('dingSecret').value = data.dingtalk?.secret || '';
    document.getElementById('wechatEnabled').checked = data.wechat?.enabled || false;
    document.getElementById('wechatKey').value = data.wechat?.key || '';
}

function loadConfigToForm() {
    apiGet('/config').then(data => {
        if (!data) return;
        document.getElementById('cfgSerialPort').value = data.serial?.port || 'auto';
        document.getElementById('cfgBaudrate').value = data.serial?.baudrate || 115200;
        document.getElementById('cfgPolling').value = data.polling_sec || 2;
    });
}

async function saveAllSettings() {
    const config = {
        serial: {
            port: document.getElementById('cfgSerialPort').value.trim() || 'auto',
            baudrate: parseInt(document.getElementById('cfgBaudrate').value) || 115200
        },
        polling_sec: parseInt(document.getElementById('cfgPolling').value) || 2
    };

    // Merge with current config
    const current = await apiGet('/config');
    if (current) {
        Object.assign(current, config);
        await apiPost('/config', current);
        alert('设置已保存');
    }
}

async function updateFilterMode() {
    const current = await apiGet('/config');
    if (!current) return;
    current.filter.mode = document.getElementById('filterMode').value;
    await apiPost('/config', current);
}

async function updateFilterOptions() {
    const current = await apiGet('/config');
    if (!current) return;
    current.filter.forward_links = document.getElementById('forwardLinks').checked;
    current.filter.forward_links_only = document.getElementById('forwardLinksOnly').checked;
    await apiPost('/config', current);
}

async function updateWebhookConfig() {
    const current = await apiGet('/config');
    if (!current) return;
    current.dingtalk.enabled = document.getElementById('dingEnabled').checked;
    current.dingtalk.mode = document.getElementById('dingMode').value;
    current.dingtalk.token = document.getElementById('dingToken').value.trim();
    current.dingtalk.secret = document.getElementById('dingSecret').value.trim();
    current.wechat.enabled = document.getElementById('wechatEnabled').checked;
    current.wechat.key = document.getElementById('wechatKey').value.trim();
    await apiPost('/config', current);
}

async function exportConfig() {
    try {
        const res = await fetch(API + '/config/export');
        const blob = await res.blob();
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = 'sms-forwarder-config.json';
        a.click();
        URL.revokeObjectURL(url);
    } catch (err) {
        alert('导出失败: ' + err.message);
    }
}

function importConfig() {
    document.getElementById('configFileInput').click();
}

async function handleConfigFile(input) {
    const file = input.files[0];
    if (!file) return;

    try {
        const text = await file.text();
        await apiPost('/config/import', { data: text });
        alert('配置已导入');
        loadConfig();
        loadKeywords();
        input.value = '';
    } catch (err) {
        alert('导入失败: ' + err.message);
    }
}

async function resetConfig() {
    if (!confirm('确定要重置所有配置为默认值吗？此操作不可撤销。')) return;
    try {
        await apiPost('/config/reset', {});
        alert('配置已重置');
        loadConfig();
        loadKeywords();
    } catch (err) {
        alert('重置失败: ' + err.message);
    }
}

// Webhook Test
async function testWebhook(platform) {
    try {
        await apiPost('/webhook/test', { platform });
        alert('测试消息已发送');
    } catch (err) {
        alert('测试失败: ' + err.message);
    }
}

// Dialer
function dialDigit(digit) {
    const input = document.getElementById('dialNumber');
    input.value += digit;
}

function dialClear() {
    document.getElementById('dialNumber').value = '';
}

async function dialCall() {
    const number = document.getElementById('dialNumber').value.trim();
    if (!number) {
        alert('请输入号码');
        return;
    }
    try {
        await apiPost('/call/dial', { number });
        loadCalls();
    } catch (err) {
        alert('拨号失败: ' + err.message);
    }
}

async function dialHangup() {
    try {
        await apiPost('/call/hangup', {});
    } catch (err) {
        alert('挂断失败: ' + err.message);
    }
}

// Call History
async function loadCalls() {
    const container = document.getElementById('callHistoryList');
    // For now, calls are stored in memory only
    // In a full implementation this would come from API
    const data = await apiGet('/status');
    container.innerHTML = '<div class="empty-state">通话记录存储在本地内存中，重启后清空</div>';
}

// Phonebook
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
            <span class="contact-number">${escHtml(entry.number)}</span>
        </div>
    `).join('');
}

async function addPhonebookEntry() {
    const name = document.getElementById('pbName').value.trim();
    const number = document.getElementById('pbNumber').value.trim();

    if (!name || !number) {
        alert('请填写姓名和号码');
        return;
    }

    try {
        await apiPost('/phonebook/add', { name, number });
        document.getElementById('pbName').value = '';
        document.getElementById('pbNumber').value = '';
        loadPhonebook();
    } catch (err) {
        alert('添加失败: ' + err.message);
    }
}

// Devices
async function refreshDevices() {
    const data = await apiGet('/devices');
    if (!data) return;

    const grid = document.getElementById('deviceInfoGrid');
    if (data.device) {
        grid.innerHTML = `
            <div class="info-item"><span class="label">信号:</span><span class="value">${data.device.signal || '-'}</span></div>
            <div class="info-item"><span class="label">网络:</span><span class="value">${data.device.network || '-'}</span></div>
            <div class="info-item"><span class="label">IMEI:</span><span class="value">${data.device.imei || '-'}</span></div>
            <div class="info-item"><span class="label">运营商:</span><span class="value">${data.device.operator || '-'}</span></div>
            <div class="info-item"><span class="label">SIM:</span><span class="value">${data.device.sim || '-'}</span></div>
            <div class="info-item"><span class="label">时间:</span><span class="value">${data.device.time || '-'}</span></div>
        `;
    }
}

async function loadPlatformInfo() {
    const data = await apiGet('/platform');
    const container = document.getElementById('platformInfo');
    if (data) {
        container.textContent = `操作系统: ${data.os} | 架构: ${data.arch}`;
    }
}

async function bindDriver() {
    try {
        await apiPost('/driver/bind', {});
        alert('驱动绑定成功！请重新拔插设备。');
    } catch (err) {
        alert('绑定失败: ' + err.message);
    }
}

// Autostart
async function loadAutostartStatus() {
    const data = await apiGet('/autostart');
    if (!data) return;

    const badge = document.getElementById('autostartBadge');
    const platform = document.getElementById('autostartPlatform');
    const toggle = document.getElementById('autostartToggle');

    if (data.enabled) {
        badge.textContent = '✅ 已启用';
        badge.className = 'status-badge enabled';
        toggle.textContent = '禁用自启动';
    } else {
        badge.textContent = '❌ 已禁用';
        badge.className = 'status-badge disabled';
        toggle.textContent = '启用自启动';
    }

    platform.textContent = '平台: ' + data.platform;
}

async function toggleAutostart() {
    try {
        const result = await apiPost('/autostart', {});
        loadAutostartStatus();
        alert(result.enabled ? '自启动已启用' : '自启动已禁用');
    } catch (err) {
        alert('操作失败: ' + err.message);
    }
}

// Logs
async function loadLogs() {
    const data = await apiGet('/logs');
    const container = document.getElementById('logContainer');

    if (!data || data.length === 0) {
        container.innerHTML = '<div class="empty-state">暂无日志</div>';
        return;
    }

    container.innerHTML = data.slice(0, 100).map(log => `
        <div class="log-entry">
            <span class="log-time">${log.time}</span>
            <span class="log-level ${log.level}">${log.level}</span>
            <span class="log-message">${escHtml(log.message)}</span>
        </div>
    `).join('');
}

// Forward Logs
function loadForwardLogs() {
    // This would come from API in full implementation
    const container = document.getElementById('forwardLog');
    container.innerHTML = '<div class="empty-state">转发日志存储在内存中</div>';
}

// Utility Functions
function formatTime(timestamp) {
    if (!timestamp) return '-';
    try {
        const d = new Date(timestamp);
        const now = new Date();
        const diff = now - d;
        if (diff < 60000) return '刚刚';
        if (diff < 3600000) return Math.floor(diff / 60000) + '分钟前';
        if (diff < 86400000) return Math.floor(diff / 3600000) + '小时前';
        return d.toLocaleString('zh-CN');
    } catch {
        return timestamp;
    }
}

function escHtml(str) {
    if (!str) return '';
    return str.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function escAttr(str) {
    if (!str) return '';
    return str.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
