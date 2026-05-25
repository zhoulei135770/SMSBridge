//go:build darwin

package main

// BindML307ADriver is a no-op on macOS (no USB serial driver binding needed).
func BindML307ADriver() error {
	return nil
}
