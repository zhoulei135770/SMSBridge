//go:build !windows && !darwin

package main

import (
	"fmt"
	"os"
	"syscall"
	"unsafe"
)

// ── Linux-specific serial port open/config ───────────────────────────────────

func openSerialPort(path string, baud int) (*SerialPort, error) {
	// Open non-blocking first to set CLOCAL, then switch to blocking.
	// This avoids the classic Linux TTY open hang when DCD is low.
	f, err := os.OpenFile(path, syscall.O_RDWR|syscall.O_NOCTTY|syscall.O_NONBLOCK, 0666)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", path, err)
	}
	fd := int(f.Fd())

	// Configure while non-blocking (sets CLOCAL)
	if err := configureLinux(fd, baud); err != nil {
		f.Close()
		return nil, err
	}

	// Now safe to switch to blocking - CLOCAL prevents DCD hang
	if err := syscall.SetNonblock(fd, false); err != nil {
		f.Close()
		return nil, fmt.Errorf("set blocking: %w", err)
	}

	return &SerialPort{fd: int(f.Fd()), f: f, path: path}, nil
}

func configureLinux(fd, baud int) error {
	t := &struct {
		Iflag, Oflag, Cflag, Lflag uint32
		Line                        uint8
		Cc                          [32]uint8
		Ispeed, Ospeed              uint32
	}{}
	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), 0x5401, uintptr(unsafe.Pointer(t))); errno != 0 {
		return fmt.Errorf("tcgetattr: %v", errno)
	}
	t.Iflag &^= syscall.IGNBRK | syscall.BRKINT | syscall.PARMRK | syscall.ISTRIP |
		syscall.INLCR | syscall.IGNCR | syscall.ICRNL | syscall.IXON
	t.Oflag &^= syscall.OPOST
	t.Lflag &^= syscall.ECHO | syscall.ECHONL | syscall.ICANON | syscall.ISIG | syscall.IEXTEN
	t.Cflag &^= syscall.CSIZE | syscall.PARENB
	t.Cflag |= syscall.CS8 | syscall.CREAD | syscall.CLOCAL
	speed := linuxBaud(baud)
	t.Ispeed = speed
	t.Ospeed = speed
	t.Cc[syscall.VMIN] = 1
	t.Cc[syscall.VTIME] = 0
	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), 0x5402, uintptr(unsafe.Pointer(t))); errno != 0 {
		return fmt.Errorf("tcsetattr: %v", errno)
	}
	return nil
}

func linuxBaud(baud int) uint32 {
	switch baud {
	case 50: return syscall.B50
	case 75: return syscall.B75
	case 110: return syscall.B110
	case 150: return syscall.B150
	case 300: return syscall.B300
	case 600: return syscall.B600
	case 1200: return syscall.B1200
	case 2400: return syscall.B2400
	case 4800: return syscall.B4800
	case 9600: return syscall.B9600
	case 19200: return syscall.B19200
	case 38400: return syscall.B38400
	case 57600: return syscall.B57600
	case 115200: return syscall.B115200
	case 230400: return syscall.B230400
	case 460800: return syscall.B460800
	case 921600: return syscall.B921600
	default: return syscall.B115200
	}
}

// ── Platform functions ───────────────────────────────────────────────────────

func getPlatformPorts() []string {
	return []string{
		"/dev/ttyUSB2", "/dev/ttyUSB1", "/dev/ttyUSB0",
		"/dev/ttyACM1", "/dev/ttyACM0", "/dev/ttyAMA0",
	}
}

func BindML307ADriver() error {
	idPath := "/sys/bus/usb-serial/drivers/option1/new_id"
	if _, err := os.Stat(idPath); os.IsNotExist(err) {
		idPath = "/sys/bus/usb/drivers/option/new_id"
		if _, err := os.Stat(idPath); os.IsNotExist(err) {
			return fmt.Errorf("option 驱动未加载，请插入 ML307A 设备")
		}
	}
	return os.WriteFile(idPath, []byte("2ecc 3012\n"), 0644)
}

func drain(sp *SerialPort) {
	fd := sp.f.Fd()
	// Set non-blocking temporarily
	syscall.SetNonblock(int(fd), true)
	defer syscall.SetNonblock(int(fd), false)

	buf := make([]byte, 1024)
	for {
		n, _ := syscall.Read(int(fd), buf)
		if n <= 0 {
			break
		}
	}
}
func readChunk(f *os.File, buf []byte, deadline time.Time) (int, error) {
	fd := int(f.Fd())
	syscall.SetNonblock(fd, true)
	defer syscall.SetNonblock(fd, false)

	d := time.Until(deadline)
	if d <= 0 {
		return 0, nil
	}

	start := time.Now()
	for {
		n, err := syscall.Read(fd, buf)
		if n > 0 {
			return n, nil
		}
		if err != nil && err != syscall.EAGAIN {
			return 0, err
		}
		if time.Since(start) >= d {
			return 0, nil
		}
		time.Sleep(10 * time.Millisecond)
	}
}
