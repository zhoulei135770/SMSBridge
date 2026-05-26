package main

import (
	"bytes"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
		"time"
	"unicode/utf16"
)

// ── SerialPort (shared definition used by all platform files) ───────────────

type SerialPort struct {
	f    *os.File
	path string
	fd   int           // cached fd, used by linux serial config
	mu   sync.Mutex    // low-level send/recv serialisation
}

func (sp *SerialPort) Close() error {
	sp.mu.Lock()
	defer sp.mu.Unlock()
	return sp.f.Close()
}

func (sp *SerialPort) Write(d []byte) (int, error) {
	return sp.f.Write(d)
}

func (sp *SerialPort) Read(buf []byte) (int, error) {
	return sp.f.Read(buf)
}

func (sp *SerialPort) Fd() int {
	return int(sp.f.Fd())
}

// ── Internal helpers ─────────────────────────────────────────────────────────

// readChunk reads from f into buf, returning when data arrives or deadline passes.
// Uses non-blocking syscall.Read to avoid goroutine leaks (TTY fds don't support deadlines).


// drain reads and discards any pending data from the serial port.
// Uses a temporary O_NONBLOCK switch to avoid leaking goroutines.


// ── AT Command Helpers ───────────────────────────────────────────────────────

// SendAT sends an AT command (high-level, acquires its own lock) and reads the
// response until OK/ERROR or timeout.
func SendAT(sp *SerialPort, cmd string, timeout time.Duration) (string, error) {
	sp.mu.Lock()
	defer sp.mu.Unlock()
	return sendATLocked(sp, cmd, timeout)
}

// sendATLocked sends an AT command without acquiring sp.mu (caller must hold
// either sp.mu or a higher-level lock that serialises access).
func sendATLocked(sp *SerialPort, cmd string, timeout time.Duration) (string, error) {
	drain(sp)

	cmdLine := cmd + "\r\n"
	if _, err := sp.f.Write([]byte(cmdLine)); err != nil {
		return "", fmt.Errorf("write cmd: %w", err)
	}

	deadline := time.Now().Add(timeout)
	var result bytes.Buffer
	readBuf := make([]byte, 256)

	for time.Now().Before(deadline) {
		n, err := readChunk(sp.f, readBuf, time.Now().Add(200*time.Millisecond))
		if n > 0 {
			result.Write(readBuf[:n])
			s := result.String()
			if strings.Contains(s, "OK\r\n") || strings.Contains(s, "ERROR\r\n") || strings.Contains(s, "> ") {
				break
			}
			continue
		}
		if err != nil {
			break
		}
	}

	resp := result.String()
	// Strip echo
	if strings.HasPrefix(resp, cmdLine) {
		resp = resp[len(cmdLine):]
	}
	resp = strings.TrimRight(resp, "\r\n ")
	return resp, nil
}

// ── Modem ────────────────────────────────────────────────────────────────────

type Modem struct {
	sp          *SerialPort
	mu          sync.Mutex
	initialized bool
}

func NewModem() *Modem {
	return &Modem{}
}

func (m *Modem) Open(port string, baud int) error {
	// Open the serial port outside the lock so HTTP handlers don't deadlock
	sp, err := openSerialPort(port, baud)
	if err != nil {
		return err
	}

	m.mu.Lock()
	defer m.mu.Unlock()
	if m.sp != nil {
		m.sp.Close()
	}
	m.sp = sp
	return nil
}

func (m *Modem) Close() {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.sp != nil {
		m.sp.Close()
		m.sp = nil
	}
	m.initialized = false
}

func (m *Modem) IsOpen() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.sp != nil
}

// IsOpenNonBlocking checks if modem is open without locking (safe for status checks).
func (m *Modem) IsOpenNonBlocking() bool {
	return m.sp != nil
}

func (m *Modem) Init() error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.sp == nil {
		return fmt.Errorf("modem not open")
	}

	resp, err := sendATLocked(m.sp, "AT", 600*time.Millisecond)
	if err != nil {
		return fmt.Errorf("AT probe failed: %w", err)
	}
	if !strings.Contains(resp, "OK") {
		return fmt.Errorf("AT probe failed: %s", resp)
	}

	cmds := []string{
		"ATE0",
		"AT+CMGF=1",
		"AT+CSCS=\"GSM\"",
		"AT+CNMI=2,1",
		"AT+CPMS=\"ME\",\"ME\",\"ME\"",
	}
	for _, cmd := range cmds {
		_, err := sendATLocked(m.sp, cmd, 500*time.Millisecond)
		if err != nil {
			continue // non-fatal
		}
	}

	m.initialized = true
	return nil
}

// atCommand sends an AT command and returns the response.
// It serializes access to the serial port to prevent concurrent AT commands.
func (m *Modem) atCommand(cmd string) (string, error) {
	if m.sp == nil {
		return "", fmt.Errorf("modem not open")
	}
	return SendAT(m.sp, cmd, 3*time.Second)
}

// ── Signal / Device Info ─────────────────────────────────────────────────────

func (m *Modem) GetSignal() (int, error) {
	resp, err := m.atCommand("AT+CSQ")
	if err != nil {
		return 0, err
	}
	re := regexp.MustCompile(`\+CSQ:\s*(\d+)`)
	match := re.FindStringSubmatch(resp)
	if len(match) > 1 {
		var v int
		fmt.Sscanf(match[1], "%d", &v)
		return v, nil
	}
	return 0, fmt.Errorf("CSQ parse error: %s", resp)
}

func (m *Modem) GetIMEI() (string, error) {
	// Try AT+CGSN first, then AT+GSN as fallback
	for _, cmd := range []string{"AT+CGSN", "AT+GSN"} {
		resp, err := m.atCommand(cmd)
		if err != nil {
			continue
		}
		// ML307A may return IMEI on a single line or with extra whitespace
		for _, line := range strings.Split(resp, "\r\n") {
			line = strings.TrimSpace(line)
			if line == "" || line == "OK" || strings.HasPrefix(line, "AT+") {
				continue
			}
			// Strip any non-digit characters and check length
			digits := strings.Map(func(r rune) rune {
				if r >= '0' && r <= '9' {
					return r
				}
				return -1
			}, line)
			if len(digits) >= 14 {
				return digits, nil
			}
		}
	}
	return "", fmt.Errorf("IMEI not found")
}

func (m *Modem) GetIMSI() (string, error) {
	resp, err := m.atCommand("AT+CIMI")
	if err != nil {
		return "", err
	}
	lines := strings.Split(resp, "\r\n")
	for _, line := range lines {
		line = strings.TrimSpace(line)
		if line != "" && line != "OK" {
			return line, nil
		}
	}
	return "", fmt.Errorf("IMSI parse error: %s", resp)
}

func (m *Modem) GetOperator() (string, error) {
	resp, err := m.atCommand("AT+COPS?")
	if err != nil {
		return "", err
	}
	re := regexp.MustCompile(`\+COPS:\s*\d+,\d+,"([^"]*)"`)
	match := re.FindStringSubmatch(resp)
	if len(match) > 1 {
		return match[1], nil
	}
	// Try numeric format
	re2 := regexp.MustCompile(`\+COPS:\s*\d+,\d+,"(\d+)"`)
	match2 := re2.FindStringSubmatch(resp)
	if len(match2) > 1 {
		return match2[1], nil
	}
	return "", fmt.Errorf("COPS parse error: %s", resp)
}

func (m *Modem) GetTime() (string, error) {
	resp, err := m.atCommand("AT+CCLK?")
	if err != nil {
		return "", err
	}
	re := regexp.MustCompile(`\+CCLK:\s*"([^"]*)"`)
	match := re.FindStringSubmatch(resp)
	if len(match) > 1 {
		return match[1], nil
	}
	return "", fmt.Errorf("CCLK parse error: %s", resp)
}

func (m *Modem) GetNetwork() (string, error) {
	resp, err := m.atCommand("AT+CREG?")
	if err != nil {
		return "", err
	}
	// Handle both basic (n=1) and extended (n=2) CREG formats
	// ML307A LTE response: +CREG: 2,1,"XXXX","XXXX",7
	re := regexp.MustCompile(`\+CREG:\s*(\d+),(\d+)`)
	match := re.FindStringSubmatch(resp)
	if len(match) > 2 {
		stat := match[2]
		switch stat {
		case "0":
			return "未注册", nil
		case "1":
			return "已注册(本地)", nil
		case "2":
			return "搜索中", nil
		case "3":
			return "注册被拒绝", nil
		case "4":
			return "未知", nil
		case "5":
			return "已注册(漫游)", nil
		default:
			// If it's a 2+ digit number like "11", it's likely the first two
			// comma-separated values concatenated (n=1,stat=1 → "11")
			// This means n=1, stat=1 → registered
			if stat == "11" || stat == "15" {
				return "已注册(本地)", nil
			}
			// ML307A LTE registration: n=5 means registered+roaming
			if stat == "51" || stat == "55" {
				return "已注册(漫游)", nil
			}
			return stat, nil
		}
	}
	return "", fmt.Errorf("CREG parse error: %s", resp)
}

// ── SMS ──────────────────────────────────────────────────────────────────────

type SMSMessage struct {
	Index  int    `json:"index"`
	Status string `json:"status"` // "REC UNREAD", "REC READ", "STO UNSENT", "STO SENT"
	From   string `json:"from"`
	Time   string `json:"time"`
	Body   string `json:"body"`
	RawHex string `json:"raw_hex,omitempty"`
}

func (m *Modem) ReadSMS() ([]SMSMessage, error) {
	resp, err := m.atCommand("AT+CMGL=\"ALL\"")
	if err != nil {
		return nil, err
	}
	return parseCMGL(resp), nil
}

// ReadNewSMS reads only unread SMS (faster for polling).
func (m *Modem) ReadNewSMS() ([]SMSMessage, error) {
	resp, err := m.atCommand("AT+CMGL=\"REC UNREAD\"")
	if err != nil {
		return nil, err
	}
	return parseCMGL(resp), nil
}

func (m *Modem) DeleteSMS(index int) error {
	_, err := m.atCommand(fmt.Sprintf("AT+CMGD=%d", index))
	return err
}

func (m *Modem) SendSMS(number, content string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.sp == nil {
		return fmt.Errorf("modem not open")
	}

	// Ensure text mode (use SendAT for proper locking)
	SendAT(m.sp, "AT+CMGF=1", 500*time.Millisecond)

	drain(m.sp)

	_, err := m.sp.f.Write([]byte(fmt.Sprintf("AT+CMGS=\"%s\"\r\n", number)))
	if err != nil {
		return err
	}

	// Wait for "> " prompt
	deadline := time.Now().Add(5 * time.Second)
	var resp bytes.Buffer
	readBuf := make([]byte, 256)
	gotPrompt := false
	for time.Now().Before(deadline) {
		n, err := readChunk(m.sp.f, readBuf, deadline)
		if n > 0 {
			resp.Write(readBuf[:n])
			if strings.Contains(resp.String(), "> ") {
				gotPrompt = true
				break
			}
		}
		if err != nil {
			break
		}
	}

	if !gotPrompt {
		return fmt.Errorf("no prompt for SMS body")
	}

	// Send body + Ctrl+Z
	_, err = m.sp.f.Write([]byte(content + "\x1A"))
	if err != nil {
		return err
	}

	// Wait for OK/ERROR
	deadline = time.Now().Add(30 * time.Second)
	resp.Reset()
	for time.Now().Before(deadline) {
		n, err := readChunk(m.sp.f, readBuf, deadline)
		if n > 0 {
			resp.Write(readBuf[:n])
			if strings.Contains(resp.String(), "OK\r\n") || strings.Contains(resp.String(), "ERROR\r\n") {
				break
			}
		}
		if err != nil {
			break
		}
	}

	if !strings.Contains(resp.String(), "OK") {
		return fmt.Errorf("send SMS failed: %s", resp.String())
	}
	return nil
}

// ── SMS Parsing ──────────────────────────────────────────────────────────────

func parseCMGL(resp string) []SMSMessage {
	var msgs []SMSMessage
	lines := strings.Split(resp, "\r\n")

	// Detect UCS2 mode
	ucs2Mode := strings.Contains(resp, "+CSCS: \"UCS2\"") || strings.Contains(resp, "UCS2")

	var current *SMSMessage
	var collectingHex bool
	var hexLines []string

	for _, line := range lines {
		line = strings.TrimSpace(line)

		// Match CMGL header: +CMGL: idx,stat,"number",,"date"
		re := regexp.MustCompile(`^\+CMGL:\s*(\d+),(\d|"[^"]*"),"([^"]*)",?[^,]*,"([^"]*)"`)
		if match := re.FindStringSubmatch(line); len(match) >= 5 {
			if current != nil {
				if ucs2Mode && len(hexLines) > 0 {
					current.Body = decodeUCS2Hex(strings.Join(hexLines, ""))
					current.RawHex = strings.Join(hexLines, "")
				}
				msgs = append(msgs, *current)
			}
			stat := match[2]
			switch stat {
			case "0":
				stat = "REC UNREAD"
			case "1":
				stat = "REC READ"
			case "2":
				stat = "STO UNSENT"
			case "3":
				stat = "STO SENT"
			}
			if strings.HasPrefix(stat, "\"") {
				stat = strings.Trim(stat, "\"")
			}
			var idx int
			fmt.Sscanf(match[1], "%d", &idx)
			current = &SMSMessage{
				Index:  idx,
				Status: stat,
				From:   match[3],
				Time:   match[4],
			}
			collectingHex = false
			hexLines = nil
			continue
		}

		if current == nil {
			continue
		}

		if line == "OK" || line == "ERROR" {
			if collectingHex && len(hexLines) > 0 {
				current.Body = decodeUCS2Hex(strings.Join(hexLines, ""))
				current.RawHex = strings.Join(hexLines, "")
			}
			msgs = append(msgs, *current)
			current = nil
			collectingHex = false
			hexLines = nil
			continue
		}

		// Check if this line looks like hex data (UCS2)
		if ucs2Mode || isHexLine(line) {
			if !ucs2Mode && !collectingHex && isHexLine(line) {
				collectingHex = true
			}
			if collectingHex || ucs2Mode {
				hexLines = append(hexLines, line)
				continue
			}
		}

		// Plain text body
		if current.Body != "" {
			current.Body += "\n" + line
		} else {
			current.Body = line
		}
	}

	// Flush any remaining
	if current != nil {
		if ucs2Mode && len(hexLines) > 0 {
			current.Body = decodeUCS2Hex(strings.Join(hexLines, ""))
			current.RawHex = strings.Join(hexLines, "")
		}
		msgs = append(msgs, *current)
	}

	if msgs == nil {
		msgs = []SMSMessage{}
	}
	return msgs
}

func isHexLine(s string) bool {
	s = strings.ReplaceAll(s, " ", "")
	s = strings.ReplaceAll(s, "\r", "")
	s = strings.ReplaceAll(s, "\n", "")
	if len(s) < 4 {
		return false
	}
	_, err := hex.DecodeString(s)
	return err == nil
}

func decodeUCS2Hex(h string) string {
	h = strings.ReplaceAll(h, " ", "")
	h = strings.ReplaceAll(h, "\r", "")
	h = strings.ReplaceAll(h, "\n", "")
	data, err := hex.DecodeString(h)
	if err != nil || len(data)%2 != 0 {
		return h
	}
	words := make([]uint16, len(data)/2)
	for i := 0; i < len(data); i += 2 {
		words[i/2] = uint16(data[i])<<8 | uint16(data[i+1])
	}
	return string(utf16.Decode(words))
}

// ── Call ─────────────────────────────────────────────────────────────────────

func (m *Modem) Dial(number string) error {
	_, err := m.atCommand(fmt.Sprintf("ATD%s;", number))
	return err
}

func (m *Modem) Hangup() error {
	_, err := m.atCommand("ATH")
	return err
}

func (m *Modem) Answer() error {
	_, err := m.atCommand("ATA")
	return err
}

func (m *Modem) CallState() (string, error) {
	resp, err := m.atCommand("AT+CLCC")
	if err != nil {
		return "idle", nil
	}
	if strings.Contains(resp, "+CLCC:") {
		return "active", nil
	}
	return "idle", nil
}

// ── Phonebook ────────────────────────────────────────────────────────────────

type Contact struct {
	Index int    `json:"index"`
	Name  string `json:"name"`
	Phone string `json:"phone"`
}

func (m *Modem) ReadPhonebook() ([]Contact, error) {
	resp, err := m.atCommand("AT+CPBR=1,250")
	if err != nil {
		return nil, err
	}
	var contacts []Contact
	re := regexp.MustCompile(`\+CPBR:\s*(\d+),"([^"]*)",\d+,"([^"]*)"`)
	for _, line := range strings.Split(resp, "\r\n") {
		match := re.FindStringSubmatch(line)
		if len(match) > 0 {
			var idx int
			fmt.Sscanf(match[1], "%d", &idx)
			contacts = append(contacts, Contact{Index: idx, Name: match[2], Phone: match[3]})
		}
	}
	if contacts == nil {
		contacts = []Contact{}
	}
	return contacts, nil
}

func (m *Modem) AddContact(name, phone string) error {
	_, err := m.atCommand(fmt.Sprintf("AT+CPBW=,\"%s\",129,\"%s\"", phone, name))
	return err
}

// ── Device Discovery ─────────────────────────────────────────────────────────

// FindModemPort probes possible serial ports for a modem.
func FindModemPort() string {
	candidates := getPlatformPorts()
	for _, port := range candidates {
		if _, err := os.Stat(port); os.IsNotExist(err) {
			continue
		}
		sp, err := openSerialPort(port, 115200)
		if err != nil {
			continue
		}
		// Use goroutine+timeout to prevent hanging
		type result struct {
			resp string
			err  error
		}
		ch := make(chan result, 1)
		go func() {
			r, e := sendATLocked(sp, "AT", 1000*time.Millisecond)
			ch <- result{r, e}
		}()
		select {
		case res := <-ch:
			sp.Close()
			if res.err == nil && strings.Contains(res.resp, "OK") {
				return port
			}
		case <-time.After(3 * time.Second):
			sp.Close()
		}
	}
	return ""
}

// IsML307A checks if a ML307A USB device is present.
func IsML307A() bool {
	matches, _ := filepath.Glob("/sys/bus/usb/devices/*/idVendor")
	for _, p := range matches {
		vendor, err := os.ReadFile(p)
		if err != nil {
			continue
		}
		if strings.TrimSpace(string(vendor)) == "2ecc" {
			productPath := filepath.Join(filepath.Dir(p), "idProduct")
			product, err := os.ReadFile(productPath)
			if err != nil {
				continue
			}
			if strings.TrimSpace(string(product)) == "4d10" {
				return true
			}
		}
	}
	return false
}

// ── SMS Body Cleaning ───────────────────────────────────────────────────────

// cleanSMSBody strips garbage bytes from the beginning of an SMS body.
// Some modems (including ML307A) prepend UDH/encoding artifacts in text mode.
func cleanSMSBody(body string) string {
	if body == "" {
		return body
	}

	// Convert to runes and find the first "good" character.
	// A "good" character is: printable ASCII, CJK, or common Chinese punctuation.
	runes := []rune(body)
	for i, r := range runes {
		if isGoodSMSRune(r) {
			if i > 0 {
				return string(runes[i:])
			}
			return body
		}
	}
	return body
}

// isGoodSMSRune returns true for characters that are valid SMS content starters.
func isGoodSMSRune(r rune) bool {
	// Printable ASCII (space through ~)
	if r >= 0x20 && r <= 0x7E {
		return true
	}
	// CJK Unified Ideographs
	if r >= 0x4E00 && r <= 0x9FFF {
		return true
	}
	// CJK Extension A
	if r >= 0x3400 && r <= 0x4DBF {
		return true
	}
	// CJK Compatibility Ideographs
	if r >= 0xF900 && r <= 0xFAFF {
		return true
	}
	// Fullwidth forms (includes 【】 etc.)
	if r >= 0xFF00 && r <= 0xFFEF {
		return true
	}
	// CJK Symbols and Punctuation
	if r >= 0x3000 && r <= 0x303F {
		return true
	}
	// Halfwidth and Fullwidth Forms
	if r >= 0xFF00 && r <= 0xFFEF {
		return true
	}
	// Hiragana / Katakana
	if r >= 0x3040 && r <= 0x30FF {
		return true
	}
	// Hangul
	if r >= 0xAC00 && r <= 0xD7AF {
		return true
	}
	// Common symbols / emoji that might appear in SMS
	if r >= 0x2000 && r <= 0x27BF {
		return true
	}
	return false
}

// ── Helpers ──────────────────────────────────────────────────────────────────

func isDigits(s string) bool {
	for _, c := range s {
		if c < '0' || c > '9' {
			return false
		}
	}
	return true
}
