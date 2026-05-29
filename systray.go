//go:build cgo

package main

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"os"

	"github.com/getlantern/systray"
)

const (
	appName = "SMS Forwarder"
	appURL  = "http://localhost:" + serverPort
)

// ── Icon Generator ───────────────────────────────────────────────────────────

func generateIcon() []byte {
	img := image.NewRGBA(image.Rect(0, 0, 32, 32))
	bg := color.RGBA{0, 0, 0, 0}
	for y := 0; y < 32; y++ {
		for x := 0; x < 32; x++ {
			img.Set(x, y, bg)
		}
	}
	darkGreen := color.RGBA{46, 125, 50, 255}
	green := color.RGBA{76, 175, 80, 255}
	lightGreen := color.RGBA{129, 199, 132, 255}
	white := color.RGBA{255, 255, 255, 255}

	for y := 1; y <= 22; y++ {
		for x := 2; x <= 29; x++ {
			cy, cx := y, x
			if cy <= 3 {
				if cx <= 4 || cx >= 27 {
					continue
				}
			}
			if cy >= 20 {
				if cx <= 4 || cx >= 27 {
					continue
				}
			}
			img.Set(x, y, green)
		}
	}
	img.Set(3, 2, darkGreen)
	img.Set(4, 1, darkGreen)
	img.Set(28, 1, darkGreen)
	img.Set(29, 2, darkGreen)
	img.Set(3, 21, darkGreen)
	img.Set(4, 22, darkGreen)
	img.Set(28, 22, darkGreen)
	img.Set(29, 21, darkGreen)

	img.Set(4, 23, darkGreen)
	img.Set(5, 23, green)
	img.Set(5, 24, green)
	img.Set(6, 24, green)
	img.Set(6, 25, darkGreen)
	img.Set(7, 25, darkGreen)

	for _, lineY := range []int{7, 12, 17} {
		for x := 6; x <= 20; x++ {
			img.Set(x, lineY, lightGreen)
		}
	}
	for x := 6; x <= 14; x++ {
		img.Set(x, 17, lightGreen)
	}

	for y := 3; y <= 8; y++ {
		for x := 23; x <= 28; x++ {
			dx := x - 25
			dy := y - 5
			if dx*dx+dy*dy <= 7 {
				img.Set(x, y, white)
			}
		}
	}
	img.Set(24, 4, green)
	img.Set(25, 4, green)
	img.Set(26, 4, green)
	img.Set(24, 5, green)
	img.Set(26, 5, green)
	img.Set(26, 6, green)
	img.Set(25, 6, green)
	img.Set(24, 6, green)
	img.Set(24, 7, green)
	img.Set(24, 8, green)
	img.Set(26, 8, green)
	img.Set(25, 8, green)

	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		return []byte{}
	}
	return buf.Bytes()
}

// ── Systray ──────────────────────────────────────────────────────────────────

func runSystray(onQuit func()) {
	// Fallback: if no display available, use no-tray mode
	if os.Getenv("DISPLAY") == "" && os.Getenv("WAYLAND_DISPLAY") == "" {
		runSystrayNoCGO(onQuit)
		return
	}
	systray.Run(func() {
		icon := generateIcon()
		systray.SetIcon(icon)
		systray.SetTitle(appName)
		systray.SetTooltip(appName + " - ML307A 短信转发器")

		mOpen := systray.AddMenuItem("打开主界面", "打开 Web 管理界面")
		systray.AddSeparator()

		autoEnabled := isAutostartEnabled()
		mAutoStart := systray.AddMenuItemCheckbox("开机自启动", "开机自动启动短信转发器", autoEnabled)
		systray.AddSeparator()

		mRestart := systray.AddMenuItem("重启应用", "结束所有进程并重新启动")
		systray.AddSeparator()

		mQuit := systray.AddMenuItem("退出程序", "退出 SMS Forwarder")

		go func() {
			for {
				select {
				case <-mOpen.ClickedCh:
					openBrowser(appURL)
				case <-mAutoStart.ClickedCh:
					if mAutoStart.Checked() {
						if err := disableAutostart(); err != nil {
							addLog("error", "取消自启动失败", err.Error())
						} else {
							mAutoStart.Uncheck()
						}
					} else {
						if err := enableAutostart(); err != nil {
							addLog("error", "设置自启动失败", err.Error())
						} else {
							mAutoStart.Check()
						}
					}
				case <-mRestart.ClickedCh:
					restartApp()
				case <-mQuit.ClickedCh:
					systray.Quit()
					return
				}
			}
		}()
	}, func() {
		if onQuit != nil {
			onQuit()
		}
	})
}
