# 📱 SMSBridge — ML307A 短信验证码转发器

[![Go Version](https://img.shields.io/badge/Go-1.22+-00ADD8?logo=go)](https://go.dev)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-blue)]()
[![Arch](https://img.shields.io/badge/arch-amd64%20%7C%20arm64-orange)]()
[![Size](https://img.shields.io/badge/size-~6MB-green)]()

**零依赖 · 开箱即用 · 6MB 单文件 · 全平台支持**

将 ML307A 4G 模块收到的短信验证码，自动转发到**钉钉群**和**企业微信群**。

> 🎯 场景：你的 SIM 卡插在 ML307A 模块上（比如插在路由器/工控机里），验证码短信来了自动推送到手机上的钉钉/企微，再也不用拔卡看短信。

## ✨ 功能

- 📩 **验证码自动转发** — 白名单关键词匹配，支持链接提取
- 📞 **语音通话** — 拨号/挂断/接听，通话记录
- 📇 **电话本管理** — 500 条联系人读写
- 🔧 **设备诊断** — 信号强度、网络状态、IMEI、运营商
- ⚡ **一键绑定驱动** — Linux 自动写入 udev new_id
- 🌐 **Web 管理界面** — 暗色主题，手机/PC 自适应
- 🚀 **开机自启动** — 跨平台支持（Linux desktop / Windows 启动文件夹 / macOS LaunchAgent）
- 🔄 **容错重试** — Webhook 失败自动重试 3 次

## 📦 开箱即用

从 [Releases](../../releases) 下载对应平台的二进制文件，直接运行：

| 平台 | 文件 |
|------|------|
| Linux x64 | `sms-forwarder-linux-amd64` |
| Linux ARM64 | `sms-forwarder-linux-arm64` |
| macOS Intel | `sms-forwarder-darwin-amd64` |
| macOS M1/M2/M3 | `sms-forwarder-darwin-arm64` |
| Windows x64 | `sms-forwarder-windows-amd64.exe` |
| Windows ARM64 | `sms-forwarder-windows-arm64.exe` |

```bash
# Linux / macOS
chmod +x sms-forwarder-linux-amd64
./sms-forwarder-linux-amd64

# Windows 双击 exe 即可
```

浏览器自动打开 `http://localhost:18923` → 进入管理界面。

## ⚙️ 配置

首次运行自动生成默认配置 `~/.config/sms-bridge/config.json`。

### 钉钉机器人

1. 钉钉群 → 群设置 → 智能群助手 → 添加机器人 → 自定义
2. 安全设置选**自定义关键词**，填入 `短信`
3. 复制 Webhook URL 中的 `access_token`
4. 在界面中填入 Token

### 企业微信机器人

1. 企微群 → 群设置 → 群机器人 → 添加
2. 复制 Webhook URL 中的 `key`
3. 在界面中填入 Key

### 一键导入

也可以使用 `config_template.json` 模板，在 Web 界面设置页点 **📥 导入**。

## 🔌 硬件适配

**ML307A 模组** (CMIOT, VID:2ECC PID:3012)

### Linux

```bash
# 驱动绑定（程序内已集成一键绑定）
echo 2ecc 3012 | sudo tee /sys/bus/usb-serial/drivers/option1/new_id
# 加入 dialout 组
sudo usermod -a -G dialout $USER
```

详细说明见 [Wiki — ML307A 驱动安装](https://github.com/zhoulei135770/SMSBridge/wiki)

### Windows

安装 [CMIOT ML307A USB 驱动](https://www.cmiot.com/)，设备管理器会显示 COM 端口。

### macOS

即插即用，系统自带 CDC-ACM 驱动。

## 🏗️ 从源码编译

```bash
# 需要 Go 1.22+
git clone https://github.com/zhoulei135770/SMSBridge.git
cd SMSBridge
go build -ldflags="-s -w" -o sms-bridge .

# 跨平台编译
GOOS=windows GOARCH=amd64 go build -o sms-bridge.exe .
GOOS=darwin GOARCH=arm64 go build -o sms-bridge-darwin .
GOOS=linux GOARCH=arm64 go build -o sms-bridge-arm .
```

## 🗂️ 项目结构

```
SMSBridge/
├── main.go              # HTTP 服务器 + 所有 API
├── modem.go             # 串口、AT 指令、短信/通话/电话本
├── config.go            # 配置管理
├── webhook.go           # 钉钉/企微 Webhook、过滤规则
├── serial_linux.go      # Linux 串口 (syscall+termios)
├── serial_darwin.go     # macOS 串口 (Darwin ioctl)
├── serial_windows.go    # Windows 串口 (Win32 API)
├── web/                 # Web 前端 (HTML/CSS/JS)
│   ├── index.html
│   ├── style.css
│   └── app.js
└── config_template.json # 配置模板
```

## 📡 API 端点

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/status` | 服务状态 (运行中/信号/IMEI) |
| POST | `/api/start` | 启动转发 |
| POST | `/api/stop` | 停止转发 |
| GET | `/api/sms` | 短信历史 |
| POST | `/api/sms/send` | 发送短信 |
| GET/POST | `/api/config` | 配置读写 |
| POST | `/api/config/import` | 导入配置 |
| GET | `/api/config/export` | 导出配置 |
| POST | `/api/webhook/test` | 测试 Webhook |
| GET/POST | `/api/keywords` | 关键词管理 |
| GET/POST | `/api/phonebook` | 电话本 |
| GET | `/api/devices` | 设备信息 |
| POST | `/api/driver/bind` | 驱动绑定(Linux) |
| GET/POST | `/api/autostart` | 开机自启动 |
| GET | `/api/logs` | 运行日志 |

## 📄 License

MIT License — 详见 [LICENSE](LICENSE)

## 🙏 致谢

- ML307A 模组 by 中国移动物联网 (CMIOT)
- 图标灵感来自开源社区
