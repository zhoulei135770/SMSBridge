//go:build windows

package main

// BindML307ADriver is a no-op on Windows (no USB serial driver binding needed).
func BindML307ADriver() error {
	return nil
}
