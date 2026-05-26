package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

// ── Auto-start (Cross-Platform, no build tags) ──────────────────────────────

func getAutostartPaths() []string {
	switch runtime.GOOS {
	case "linux":
		home, _ := os.UserHomeDir()
		return []string{filepath.Join(home, ".config", "autostart", "sms-forwarder-go.desktop")}
	case "darwin":
		home, _ := os.UserHomeDir()
		return []string{filepath.Join(home, "Library", "LaunchAgents", "com.smsforwarder.plist")}
	case "windows":
		return []string{} // Registry-based, handled separately
	default:
		home, _ := os.UserHomeDir()
		return []string{filepath.Join(home, ".config", "autostart", "sms-forwarder-go.desktop")}
	}
}

func isAutostartEnabled() bool {
	paths := getAutostartPaths()
	if len(paths) == 0 {
		if runtime.GOOS == "windows" {
			return isWindowsAutostart()
		}
		return false
	}
	for _, p := range paths {
		if _, err := os.Stat(p); err == nil {
			return true
		}
	}
	return false
}

func enableAutostart() error {
	switch runtime.GOOS {
	case "linux":
		return enableLinuxAutostart()
	case "darwin":
		return enableDarwinAutostart()
	case "windows":
		return enableWindowsAutostart()
	default:
		return enableLinuxAutostart()
	}
}

func disableAutostart() error {
	switch runtime.GOOS {
	case "linux":
		return disableLinuxAutostart()
	case "darwin":
		return disableDarwinAutostart()
	case "windows":
		return disableWindowsAutostart()
	default:
		return disableLinuxAutostart()
	}
}

// ── Linux: XDG Autostart ────────────────────────────────────────────────────

func enableLinuxAutostart() error {
	home, _ := os.UserHomeDir()
	dir := filepath.Join(home, ".config", "autostart")
	if err := os.MkdirAll(dir, 0755); err != nil {
		return fmt.Errorf("创建自启动目录失败: %w", err)
	}
	exePath, _ := os.Executable()
	content := fmt.Sprintf(`[Desktop Entry]
Type=Application
Name=SMS Forwarder
Comment=SMS 短信转发器 - ML307A
Exec=%s
Terminal=false
X-GNOME-Autostart-enabled=true
`, exePath)
	p := getAutostartPaths()[0]
	if err := os.WriteFile(p, []byte(content), 0644); err != nil {
		return fmt.Errorf("写入自启动文件失败: %w", err)
	}
	addLog("info", "已设置开机自启动", p)
	return nil
}

func disableLinuxAutostart() error {
	for _, p := range getAutostartPaths() {
		if err := os.Remove(p); err != nil && !os.IsNotExist(err) {
			return fmt.Errorf("移除自启动文件失败: %w", err)
		}
	}
	addLog("info", "已取消开机自启动", "")
	return nil
}

// ── macOS: LaunchAgent ──────────────────────────────────────────────────────

func enableDarwinAutostart() error {
	home, _ := os.UserHomeDir()
	dir := filepath.Join(home, "Library", "LaunchAgents")
	if err := os.MkdirAll(dir, 0755); err != nil {
		return fmt.Errorf("创建 LaunchAgents 目录失败: %w", err)
	}
	exePath, _ := os.Executable()
	plist := fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.smsforwarder</string>
    <key>ProgramArguments</key>
    <array>
        <string>%s</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <false/>
</dict>
</plist>`, exePath)
	p := getAutostartPaths()[0]
	if err := os.WriteFile(p, []byte(plist), 0644); err != nil {
		return fmt.Errorf("写入 plist 失败: %w", err)
	}
	addLog("info", "已设置开机自启动(macOS)", p)
	return nil
}

func disableDarwinAutostart() error {
	return disableLinuxAutostart() // Same logic: delete the file
}

// ── Windows: Registry ───────────────────────────────────────────────────────

func enableWindowsAutostart() error {
	exePath, err := os.Executable()
	if err != nil {
		return err
	}
	cmd := exec.Command("reg", "add",
		`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`,
		"/v", "SMSForwarder",
		"/t", "REG_SZ",
		"/d", fmt.Sprintf(`"%s"`, exePath),
		"/f")
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("写入注册表失败: %w", err)
	}
	addLog("info", "已设置开机自启动(Windows)", "")
	return nil
}

func disableWindowsAutostart() error {
	cmd := exec.Command("reg", "delete",
		`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`,
		"/v", "SMSForwarder",
		"/f")
	cmd.Run() // Ignore error if key doesn't exist
	addLog("info", "已取消开机自启动(Windows)", "")
	return nil
}

func isWindowsAutostart() bool {
	cmd := exec.Command("reg", "query",
		`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`,
		"/v", "SMSForwarder")
	return cmd.Run() == nil
}

// ── Restart ─────────────────────────────────────────────────────────────────

func restartApp() {
	// Fork a shell that waits for us to die, then starts a new instance.
	// We can't start the new instance from here because pkill would kill us first.
	exe, err := os.Executable()
	if err != nil {
		return
	}

	switch runtime.GOOS {
	case "linux", "darwin":
		// Use anchored regex to avoid killing the shell script itself
		// (process name is truncated to 15 chars in Linux)
		cmd := exec.Command("sh", "-c",
			fmt.Sprintf("sleep 1 && pkill -f '^.*%s$' 2>/dev/null; sleep 0.5 && nohup '%s' >/dev/null 2>&1 &",
				filepath.Base(exe), exe))
		cmd.Start()
	case "windows":
		cmd := exec.Command("cmd", "/c",
			fmt.Sprintf("timeout /t 1 >nul && taskkill /F /IM %s 2>nul & timeout /t 1 >nul && start \"\" \"%s\"",
				filepath.Base(exe), exe))
		cmd.Start()
	}

	// Exit current process - the forked shell will handle the restart
	os.Exit(0)
}

func killAllInstances() {
	self := filepath.Base(os.Args[0])
	switch runtime.GOOS {
	case "linux", "darwin":
		exec.Command("pkill", "-f", "^.*"+self+"$").Run()
	case "windows":
		exec.Command("taskkill", "/F", "/IM", self).Run()
	}
}
