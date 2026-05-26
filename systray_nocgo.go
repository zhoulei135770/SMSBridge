//go:build !cgo

package main

import (
	"log"
	"os"
	"os/signal"
	"syscall"
)

// No-CGO fallback: no system tray, use Ctrl+C to exit.
// Used for cross-compiled or platforms without CGO dependencies.

func runSystray(onQuit func()) {
	log.Printf("运行中 | http://localhost:%s | Ctrl+C 退出（未启用系统托盘）", serverPort)

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	<-sigCh

	if onQuit != nil {
		onQuit()
	}
}
