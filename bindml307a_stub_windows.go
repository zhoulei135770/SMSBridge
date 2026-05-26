//go:build windows

package main

import "fmt"

// BindML307ADriver is a no-op on Windows (no USB serial driver binding needed).
func BindML307ADriver() error {
	return nil
}

// RecoverML307A is a no-op on Windows.  
func RecoverML307A() error {
	return fmt.Errorf("Windows 不支持自动恢复，请重新插拔设备")
}
