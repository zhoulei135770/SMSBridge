#!/bin/bash
# Build SMS Forwarder for all platforms
# Usage: ./build.sh
#
# Platform notes:
#   - linux/amd64: built with CGO for GTK3/ayatana systray support
#   - All others: built without CGO (no systray, but web UI + autostart work)
#
# For macOS/Windows systray support, build natively on those platforms with CGO.

set -e

GO=${GO:-go}
DIST=dist
APP=sms-forwarder
GOPROXY=${GOPROXY:-https://goproxy.cn,direct}
LDFLAGS="-s -w"

rm -rf "$DIST"
mkdir -p "$DIST"

echo "=== Building SMS Forwarder v2.0 ==="
echo ""

# Linux amd64 - with CGO (full systray support)
echo "[1/6] linux/amd64 (CGO+systray)..."
CGO_ENABLED=1 GOOS=linux GOARCH=amd64 GOPROXY="$GOPROXY" \
    $GO build -ldflags="$LDFLAGS" -o "$DIST/$APP-linux-amd64" .
echo "  ✅ $DIST/$APP-linux-amd64"

# Linux arm64 - no CGO (static, headless)
echo "[2/6] linux/arm64..."
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 GOPROXY="$GOPROXY" \
    $GO build -ldflags="$LDFLAGS" -o "$DIST/$APP-linux-arm64" .
echo "  ✅ $DIST/$APP-linux-arm64"

# macOS amd64 - no CGO
echo "[3/6] darwin/amd64..."
CGO_ENABLED=0 GOOS=darwin GOARCH=amd64 GOPROXY="$GOPROXY" \
    $GO build -ldflags="$LDFLAGS" -o "$DIST/$APP-darwin-amd64" .
echo "  ✅ $DIST/$APP-darwin-amd64"

# macOS arm64 - no CGO
echo "[4/6] darwin/arm64..."
CGO_ENABLED=0 GOOS=darwin GOARCH=arm64 GOPROXY="$GOPROXY" \
    $GO build -ldflags="$LDFLAGS" -o "$DIST/$APP-darwin-arm64" .
echo "  ✅ $DIST/$APP-darwin-arm64"

# Windows amd64 - no CGO
echo "[5/6] windows/amd64..."
CGO_ENABLED=0 GOOS=windows GOARCH=amd64 GOPROXY="$GOPROXY" \
    $GO build -ldflags="$LDFLAGS" -o "$DIST/$APP-windows-amd64.exe" .
echo "  ✅ $DIST/$APP-windows-amd64.exe"

# Windows arm64 - no CGO
echo "[6/6] windows/arm64..."
CGO_ENABLED=0 GOOS=windows GOARCH=arm64 GOPROXY="$GOPROXY" \
    $GO build -ldflags="$LDFLAGS" -o "$DIST/$APP-windows-arm64.exe" .
echo "  ✅ $DIST/$APP-windows-arm64.exe"

echo ""
echo "=== Build Complete ==="
ls -lah "$DIST/"
echo ""
echo "Platform notes:"
echo "  linux/amd64   – Full systray (CGO + GTK3)"
echo "  others        – Headless (Ctrl+C to exit, web UI works fully)"
echo ""
echo "For macOS/Windows systray: build natively with CGO enabled."
