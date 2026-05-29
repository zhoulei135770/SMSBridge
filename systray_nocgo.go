//go:build !cgo

package main

// No-CGO fallback: no system tray, use Ctrl+C to exit.
// Used for cross-compiled or platforms without CGO dependencies.

func runSystray(onQuit func()) {
	runSystrayNoCGO(onQuit)
}
