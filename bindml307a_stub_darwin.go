//go:build darwin

package main

import "fmt"

// BindML307ADriver is a no-op on macOS (no USB serial driver binding needed).
func BindML307ADriver() error {
	return nil
}

// RecoverML307A is a no-op on macOS.
func RecoverML307A() error {
	return fmt.Errorf("macOS 不支持 USB 重置，请重新插拔设备")
}
