# SMS Forwarder (SMSBridge)

ML307A 短信转发器 — Go 语言重写版，轻量高性能。

## 功能

- 📡 自动检测 ML307A 4G 模块，读取/发送短信
- 🌐 Web 管理界面 (http://localhost:18923)
- 📤 验证码自动转发到钉钉/企业微信
- 🔽 系统托盘图标 (Linux CGO+systray)
- 📞 拨号盘 + 通话记录
- 📖 电话本管理
- 🚀 跨平台开机自启动

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
GO=/usr/local/go/bin/go ./build.sh

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
| macOS | ❌ 命令行 | ✅ LaunchAgent |
| Windows | ❌ 命令行 | ✅ Registry |
| macOS/Windows 原生编译 | ✅ CGO | ✅ |

## API

详见 `main.go` 中的路由注册。
