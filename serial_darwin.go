//go:build darwin

package main

// macOS serial port — reuses Linux termios (same POSIX syscalls)
// But Darwin uses different ioctl numbers and lacks termios2

import (
	"fmt"
	"time"
	"os"
	"syscall"
	"unsafe"
)

func openSerialPort(path string, baudRate int) (*SerialPort, error) {
	f, err := os.OpenFile(path, syscall.O_RDWR|syscall.O_NOCTTY|syscall.O_NONBLOCK, 0666)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", path, err)
	}
	if err := syscall.SetNonblock(int(f.Fd()), false); err != nil {
		f.Close()
		return nil, err
	}
	sp := &SerialPort{f: f, path: path}
	if err := sp.configure(baudRate); err != nil {
		f.Close()
		return nil, err
	}
	return sp, nil
}

// Darwin termios struct
type darwinTermios struct {
	Iflag  uint64
	Oflag  uint64
	Cflag  uint64
	Lflag  uint64
	Cc     [20]uint8
	Ispeed uint64
	Ospeed uint64
}

const (
	darwinTIOCGETA = 0x40487413
	darwinTIOCSETA = 0x80487414
)

func (sp *SerialPort) configure(baud int) error {
	fd := int(sp.f.Fd())
	var t darwinTermios

	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), darwinTIOCGETA, uintptr(unsafe.Pointer(&t))); errno != 0 {
		return fmt.Errorf("tcgetattr: %v", errno)
	}

	// Raw mode: 8N1
	t.Iflag &^= syscall.IGNBRK | syscall.BRKINT | syscall.PARMRK | syscall.ISTRIP |
		syscall.INLCR | syscall.IGNCR | syscall.ICRNL | syscall.IXON
	t.Oflag &^= syscall.OPOST
	t.Lflag &^= syscall.ECHO | syscall.ECHONL | syscall.ICANON | syscall.ISIG | syscall.IEXTEN
	t.Cflag &^= uint64(syscall.CSIZE) | uint64(syscall.PARENB)
	t.Cflag |= uint64(syscall.CS8) | uint64(syscall.CREAD) | uint64(syscall.CLOCAL)

	speed := uint64(baud)
	t.Ispeed = speed
	t.Ospeed = speed

	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), darwinTIOCSETA, uintptr(unsafe.Pointer(&t))); errno != 0 {
		return fmt.Errorf("tcsetattr: %v", errno)
	}
	return nil
}


func getPlatformPorts() []string {
	return []string{
		"/dev/tty.usbmodem", "/dev/cu.usbmodem",
		"/dev/tty.usbserial", "/dev/cu.usbserial",
	}
}

func drain(sp *SerialPort) {
	buf := make([]byte, 512)
	sp.f.SetReadDeadline(time.Now().Add(100 * time.Millisecond))
	sp.f.Read(buf)
	sp.f.SetReadDeadline(time.Time{})
}

func readChunk(f *os.File, buf []byte, deadline time.Time) (int, error) {
	f.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	n, err := f.Read(buf)
	f.SetReadDeadline(time.Time{})
	return n, err
}
