//go:build windows

package main

// Windows serial port — uses Win32 API (CreateFile / SetCommState / ReadFile / WriteFile)

import (
	"fmt"
	"time"
	"os"
	"syscall"
	"unsafe"
)

var (
	kernel32          = syscall.NewLazyDLL("kernel32.dll")
	procSetCommState  = kernel32.NewProc("SetCommState")
	procGetCommState  = kernel32.NewProc("GetCommState")
	procPurgeComm     = kernel32.NewProc("PurgeComm")
	procSetupComm     = kernel32.NewProc("SetupComm")
)

// DCB structure
type dcb struct {
	DCBlength  uint32
	BaudRate   uint32
	Flags      uint32
	_          uint16
	XonLim     uint16
	XoffLim    uint16
	ByteSize   byte
	Parity     byte
	StopBits   byte
	XonChar    byte
	XoffChar   byte
	ErrorChar  byte
	EofChar    byte
	EvtChar    byte
	Reserved1  uint16
}

const (
	fBinary          = 1
	fParity          = 2
	fOutxCtsFlow     = 4
	fOutxDsrFlow     = 8
	fDtrControl      = 0x30
	fDsrSensitivity  = 0x40
	fTXContinueOnXoff = 0x80
	fOutX            = 0x100
	fInX             = 0x200
	fErrorChar       = 0x400
	fNull            = 0x800
	fRtsControl      = 0x3000
	fAbortOnError    = 0x4000
	dtrControlDisable = 0
	rtsControlDisable = 0
	onesStopBit      = 0
	noParity         = 0
	purgeTXCLEAR     = 4
	purgeRXCLEAR     = 1
	genericReadWrite = 0xC0000000
	openExisting     = 3
	fileFlagOverlapped = 0x40000000
	invalidHandleValue = ^uintptr(0)
)

func openSerialPort(path string, baudRate int) (*SerialPort, error) {
	path16, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return nil, err
	}

	handle, _, err := syscall.SyscallN(
		procCreateFile.Addr(),
		uintptr(unsafe.Pointer(path16)),
		genericReadWrite,
		0,
		0,
		openExisting,
		fileFlagOverlapped,
		0,
	)
	if handle == invalidHandleValue {
		return nil, fmt.Errorf("CreateFile %s: %v", path, err)
	}

	sp := &SerialPort{f: os.NewFile(handle, path), path: path}

	if err := sp.configure(baudRate); err != nil {
		sp.Close()
		return nil, err
	}
	return sp, nil
}

var procCreateFile = kernel32.NewProc("CreateFileW")

func (sp *SerialPort) configure(baud int) error {
	fd := sp.f.Fd()

	// Setup buffers
	syscall.SyscallN(procSetupComm.Addr(), fd, 4096, 4096)

	// Purge
	syscall.SyscallN(procPurgeComm.Addr(), fd, purgeTXCLEAR|purgeRXCLEAR)

	// Build DCB
	var d dcb
	d.DCBlength = uint32(unsafe.Sizeof(d))

	r, _, _ := syscall.SyscallN(procGetCommState.Addr(), fd, uintptr(unsafe.Pointer(&d)))
	if r == 0 {
		return fmt.Errorf("GetCommState failed")
	}

	d.BaudRate = uint32(baud)
	d.Flags = fBinary
	d.ByteSize = 8
	d.Parity = noParity
	d.StopBits = onesStopBit

	r, _, _ = syscall.SyscallN(procSetCommState.Addr(), fd, uintptr(unsafe.Pointer(&d)))
	if r == 0 {
		return fmt.Errorf("SetCommState failed")
	}
	return nil
}


func getPlatformPorts() []string {
	ports := make([]string, 0, 16)
	for i := 1; i <= 16; i++ {
		ports = append(ports, fmt.Sprintf("COM%d", i))
	}
	return ports
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
