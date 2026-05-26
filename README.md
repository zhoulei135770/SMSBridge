# SMS Forwarder (SMSBridge) v2.3

ML307A 短信转发器 — Go 语言重写版，轻量高性能，跨平台开箱即用。

## 功能

- 📡 自动检测 ML307A 4G 模块，读取/发送短信
- 🌐 Web 管理界面 (http://localhost:18923)，纯 CSS 图标
- 📤 验证码自动转发到钉钉/企业微信，支持三种模板
- 🔽 系统托盘图标 (Linux)，右键菜单
- 🔄 模组断连自动恢复（USB 重置 + 驱动重绑）
- ⏱️ 开机自启动 + 自动重试连接（最多 10 次，间隔递增）
- 📞 拨号盘 + 通话记录
- 📖 SIM 卡电话本管理
- ❓ 帮助页面（全部功能文档）

## 快速开始

```bash
./sms-forwarder-linux-amd64    # 下载即运行
# 浏览器打开 http://localhost:18923
```

## 构建

```bash
# 全平台
GO=/usr/local/go/bin/go GOPROXY=https://goproxy.cn,direct ./build.sh

# 仅 Linux（带托盘）
CGO_ENABLED=1 go build -ldflags="-s -w"
```

## 平台

| 平台 | 托盘 | 自启动 | 自动恢复 |
|------|------|--------|----------|
| Linux amd64 | ✅ | ✅ XDG | ✅ USB 重置 |
| Linux arm64 | ❌ | ✅ | ✅ |
| macOS | ❌ | ✅ plist | ❌ |
| Windows | ❌ | ✅ 注册表 | ❌ |

> macOS/Windows 原生 CGO 编译可启用托盘

## 更新日志

### v2.3
- 🔄 模组断连一键恢复（USB 重置 + 驱动重绑）
- ⏱️ 开机自动重试连接（最多 10 次，间隔递增）
- 📊 服务端渲染仪表盘（HTML 直接注入实时数据，不依赖 JS）
- ⚡ 短信转发加速（1s 轮询 + 仅查未读）
- 🎨 转发模板：默认/精简/仅验证码 + 自动提取验证码
- ❓ 帮助页面
- 🗑️ 短信删除 + 关键词增删
- 🔧 串口端口下拉选择器

### v2.2
- 系统托盘 + 纯 CSS 图标 + 跨平台自启动
- 串口并发锁 + SMS 乱码清洗 + 重复启动保护
