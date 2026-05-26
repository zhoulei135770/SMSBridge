# SMS Forwarder (SMSBridge) v2.2

ML307A 短信转发器 — Go 语言重写版，轻量高性能，跨平台开箱即用。

## 功能

- 📡 自动检测 ML307A 4G 模块，读取/发送短信
- 🌐 Web 管理界面 (http://localhost:18923)，纯 CSS 图标，零 emoji 依赖
- 📤 验证码自动转发到钉钉/企业微信，支持三种模板（默认/精简/仅验证码）
- 🔽 系统托盘图标 (Linux GTK3，右键菜单：打开/自启动/重启/退出)
- 📞 拨号盘 + 通话记录
- 📖 SIM 卡电话本管理
- 🚀 跨平台开机自启动 (Linux .desktop / macOS LaunchAgent / Windows Registry)
- ❓ 帮助页面（全部功能文档）
- 🗑️ 短信删除（内存 + 模组存储）

## 快速开始

```bash
# 下载对应平台二进制，直接运行
./sms-forwarder-linux-amd64

# 浏览器访问
http://localhost:18923
```

## 构建

```bash
# 全平台构建（需要 Go 1.22+）
GO=/usr/local/go/bin/go GOPROXY=https://goproxy.cn,direct ./build.sh

# 仅 Linux (带系统托盘)
CGO_ENABLED=1 go build -ldflags="-s -w" -o sms-forwarder .

# 其他平台（无托盘，纯 Go）
CGO_ENABLED=0 GOOS=darwin go build -o sms-forwarder-darwin .
CGO_ENABLED=0 GOOS=windows go build -o sms-forwarder.exe .
```

## 平台支持

| 平台 | 系统托盘 | 自启动 |
|------|---------|--------|
| Linux amd64 | ✅ CGO+GTK3 | ✅ XDG |
| Linux arm64 | ❌ 命令行 | ✅ XDG |
| macOS amd64/arm64 | ❌ 命令行 | ✅ LaunchAgent |
| Windows amd64/arm64 | ❌ 命令行 | ✅ Registry |
| macOS/Windows 原生编译 | ✅ CGO | ✅ |

## 更新日志

### v2.2 (2026-05-26)
- 服务端渲染仪表盘（HTML 直接注入实时数据，不依赖 JS）
- 短信转发加速（1s 轮询 + 仅查未读 + 5s Webhook 超时）
- 转发模板：默认/精简/仅验证码 三种模式
- 自动提取验证码正则
- 短信删除功能
- 系统托盘「重启应用」
- 串口端口下拉选择器 + 自动扫描
- 强制不缓存响应头
- 帮助页面（全部功能文档）
- 拨号反馈 Toast + 通话记录 API
- 关键词增删 API
- SMS 时间戳 ISO 8601 转换
- 网络状态 ML307A CREG 适配

### v2.1 (2026-05-26)
- 系统托盘图标 (CGO+systray)
- 纯 CSS 图标（零 emoji 依赖）
- 跨平台开机自启动
- 串口并发锁修复
- SMS 正文乱码清洗
- 重复启动保护

## API

详见 `main.go` 路由注册。

| 端点 | 方法 | 说明 |
|------|------|------|
| /api/status | GET | 模组状态 |
| /api/sms | GET | 短信记录 |
| /api/sms/send | POST | 发送短信 |
| /api/sms/delete | POST | 删除短信 |
| /api/calls | GET | 通话记录 |
| /api/call/dial | POST | 拨号 |
| /api/call/hangup | POST | 挂断 |
| /api/phonebook | GET/POST | 电话本 |
| /api/phonebook/add | POST | 添加联系人 |
| /api/keywords | GET/POST | 关键词 |
| /api/keywords/add | POST | 添加关键词 |
| /api/keywords/del | POST | 删除关键词 |
| /api/config | GET/POST | 配置管理 |
| /api/devices | GET | 设备信息 |
| /api/platform | GET | 平台信息 |
| /api/autostart | GET/POST | 自启动 |
| /api/logs | GET | 系统日志 |
