package main

import (
	"embed"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"runtime"
	"strings"
	"sync"
	"time"
)

//go:embed web/*
var webFS embed.FS

const (
	serverPort    = "18923"
	maxHistory    = 1000
	logMaxEntries = 500
)

var (
	modem = NewModem()

	smsHistory []HistoryEntry
	historyMu  sync.RWMutex

	callHistory []CallRecord
	callMu      sync.RWMutex

	logEntries []LogEntry
	logMu      sync.RWMutex

	running   bool
	runningMu sync.RWMutex

	stopCh     chan struct{}
	pollStopCh chan struct{}

	seenMsgs      = make(map[string]bool)
	seenMu        sync.Mutex
	smsTodayCount int
	smsSkipCount  int
	smsTotalCount int
)

type HistoryEntry struct {
	Timestamp string `json:"timestamp"`
	Phone     string `json:"phone"`
	Content   string `json:"content"`
}

type CallRecord struct {
	Time     string `json:"time"`
	Number   string `json:"number"`
	Type     string `json:"type"` // "dial" or "incoming"
	Duration string `json:"duration"`
}

type LogEntry struct {
	Time    string `json:"time"`
	Level   string `json:"level"`
	Message string `json:"message"`
	Detail  string `json:"detail"`
}

// ── Helpers ──────────────────────────────────────────────────────────────────

func addLog(level, msg, detail string) {
	logMu.Lock()
	defer logMu.Unlock()
	entry := LogEntry{
		Time:    time.Now().Format("2006-01-02 15:04:05"),
		Level:   level,
		Message: msg,
		Detail:  detail,
	}
	logEntries = append(logEntries, entry)
	if len(logEntries) > logMaxEntries {
		logEntries = logEntries[len(logEntries)-logMaxEntries:]
	}
	log.Printf("[%s] %s %s", level, msg, detail)
}

func corsMiddleware(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		if r.Method == "OPTIONS" {
			w.WriteHeader(http.StatusOK)
			return
		}
		next(w, r)
	}
}

func writeJSON(w http.ResponseWriter, data interface{}) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	json.NewEncoder(w).Encode(data)
}

// ── API Handlers ─────────────────────────────────────────────────────────────

// GET /api/status
func handleStatus(w http.ResponseWriter, r *http.Request) {
	signal := 0
	imei := ""
	operator := ""
	network := ""
	// Use non-blocking check to avoid deadlock during modem Open/Init
	if modem.IsOpenNonBlocking() {
		if s, err := modem.GetSignal(); err == nil {
			signal = s
		}
		if i, err := modem.GetIMEI(); err == nil {
			imei = i
		}
		if o, err := modem.GetOperator(); err == nil {
			operator = o
		}
		if n, err := modem.GetNetwork(); err == nil {
			network = n
		}
	}
	runningMu.RLock()
	isRunning := running
	runningMu.RUnlock()
	writeJSON(w, map[string]interface{}{
		"running":  isRunning,
		"polling":  isRunning,
		"port":     serverPort,
		"signal":   signal,
		"imei":     imei,
		"operator": operator,
		"network":  network,
	})
}

// POST /api/start
func handleStart(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeJSON(w, map[string]string{"error": "method not allowed"})
		return
	}
	if !modem.IsOpen() {
		cfg := GetConfig()
		port := cfg.Serial.Port
		if port == "auto" || port == "" {
			port = FindModemPort()
		}
		if port == "" {
			writeJSON(w, map[string]string{"error": "未找到串口设备"})
			return
		}
		baud := cfg.Serial.Baudrate
		if baud == 0 {
			baud = 115200
		}
		if err := modem.Open(port, baud); err != nil {
			writeJSON(w, map[string]string{"error": "连接失败: " + err.Error()})
			return
		}
		modem.Init()
		addLog("info", "调制解调器已连接", port)
	}
	startPolling()
	writeJSON(w, map[string]string{"status": "started"})
}

// POST /api/stop
func handleStop(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeJSON(w, map[string]string{"error": "method not allowed"})
		return
	}
	stopPolling()
	writeJSON(w, map[string]string{"status": "stopped"})
}

// GET /api/sms
func handleSMS(w http.ResponseWriter, r *http.Request) {
	historyMu.RLock()
	defer historyMu.RUnlock()
	if smsHistory == nil {
		writeJSON(w, []HistoryEntry{})
		return
	}
	writeJSON(w, smsHistory)
}

// POST /api/sms/send
func handleSMSSend(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeJSON(w, map[string]string{"error": "method not allowed"})
		return
	}
	var req struct {
		Number  string `json:"number"`
		Content string `json:"content"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, map[string]string{"error": "invalid JSON"})
		return
	}
	if req.Number == "" || req.Content == "" {
		writeJSON(w, map[string]string{"error": "号码和内容不能为空"})
		return
	}
	if err := modem.SendSMS(req.Number, req.Content); err != nil {
		writeJSON(w, map[string]string{"error": err.Error()})
		return
	}
	addLog("info", "短信已发送", req.Number)
	writeJSON(w, map[string]string{"status": "sent"})
}

// POST /api/sms/delete
func handleSMSDelete(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeJSON(w, map[string]string{"error": "method not allowed"})
		return
	}
	var req struct {
		Index int    `json:"index"`
		Phone string `json:"phone"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, map[string]string{"error": "invalid JSON"})
		return
	}
	// Delete from modem
	if req.Index > 0 {
		modem.DeleteSMS(req.Index)
	}
	// Remove from in-memory history
	historyMu.Lock()
	var filtered []HistoryEntry
	for _, e := range smsHistory {
		if e.Phone != req.Phone && req.Index == 0 {
			filtered = append(filtered, e)
		} else if req.Index > 0 {
			filtered = append(filtered, e)
		}
	}
	// Simple approach: remove by phone if no index, or keep all if index was given (modem handles it)
	if req.Phone != "" && req.Index == 0 {
		smsHistory = filtered
	}
	historyMu.Unlock()
	addLog("info", "短信已删除", req.Phone)
	writeJSON(w, map[string]string{"status": "deleted"})
}

// GET/POST /api/config
func handleConfig(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case "GET":
		writeJSON(w, GetConfig())
	case "POST":
		var update Config
		if err := json.NewDecoder(r.Body).Decode(&update); err != nil {
			writeJSON(w, map[string]string{"error": "invalid JSON"})
			return
		}
		if err := UpdateConfig(func(c *Config) {
			*c = update
		}); err != nil {
			writeJSON(w, map[string]string{"error": err.Error()})
			return
		}
		addLog("info", "配置已更新", "")
		writeJSON(w, map[string]string{"status": "saved"})
	default:
		writeJSON(w, map[string]string{"error": "method not allowed"})
	}
}

// POST /api/config/import
func handleConfigImport(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeJSON(w, map[string]string{"error": "method not allowed"})
		return
	}
	data, err := io.ReadAll(r.Body)
	if err != nil {
		writeJSON(w, map[string]string{"error": "read body: " + err.Error()})
		return
	}
	if err := ImportConfig(data); err != nil {
		writeJSON(w, map[string]string{"error": err.Error()})
		return
	}
	addLog("info", "配置已导入", "")
	writeJSON(w, map[string]string{"status": "imported"})
}

// GET /api/config/export
func handleConfigExport(w http.ResponseWriter, r *http.Request) {
	data, err := ExportConfig()
	if err != nil {
		writeJSON(w, map[string]string{"error": err.Error()})
		return
	}
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Content-Disposition", "attachment; filename=sms-forwarder-config.json")
	w.Write(data)
}

// POST /api/config/reset
func handleConfigReset(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeJSON(w, map[string]string{"error": "method not allowed"})
		return
	}
	if err := ResetConfig(); err != nil {
		writeJSON(w, map[string]string{"error": err.Error()})
		return
	}
	addLog("info", "配置已重置", "")
	writeJSON(w, map[string]string{"status": "reset"})
}

// GET/POST /api/keywords
func handleKeywords(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case "GET":
		cfg := GetConfig()
		writeJSON(w, map[string]interface{}{
			"keywords": cfg.Filter.Keywords,
		})
	case "POST":
		// /add: add single keyword, /del: remove single keyword, /keywords: replace all
		if strings.HasSuffix(r.URL.Path, "/add") {
			var req struct {
				Keyword string `json:"keyword"`
			}
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
				writeJSON(w, map[string]string{"error": "invalid JSON"})
				return
			}
			cfg := GetConfig()
			for _, kw := range cfg.Filter.Keywords {
				if kw == req.Keyword {
					writeJSON(w, map[string]string{"status": "exists"})
					return
				}
			}
			cfg.Filter.Keywords = append(cfg.Filter.Keywords, req.Keyword)
			if err := UpdateKeywords(cfg.Filter.Keywords); err != nil {
				writeJSON(w, map[string]string{"error": err.Error()})
				return
			}
			addLog("info", "关键词已添加", req.Keyword)
			writeJSON(w, map[string]string{"status": "added"})
			return
		}
		if strings.HasSuffix(r.URL.Path, "/del") {
			var req struct {
				Keyword string `json:"keyword"`
			}
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
				writeJSON(w, map[string]string{"error": "invalid JSON"})
				return
			}
			cfg := GetConfig()
			var filtered []string
			for _, kw := range cfg.Filter.Keywords {
				if kw != req.Keyword {
					filtered = append(filtered, kw)
				}
			}
			if err := UpdateKeywords(filtered); err != nil {
				writeJSON(w, map[string]string{"error": err.Error()})
				return
			}
			addLog("info", "关键词已删除", req.Keyword)
			writeJSON(w, map[string]string{"status": "deleted"})
			return
		}
		// Full replace
		var req struct {
			Keywords []string `json:"keywords"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeJSON(w, map[string]string{"error": "invalid JSON"})
			return
		}
		if err := UpdateKeywords(req.Keywords); err != nil {
			writeJSON(w, map[string]string{"error": err.Error()})
			return
		}
		addLog("info", "关键词已更新", "")
		writeJSON(w, map[string]string{"status": "saved"})
	default:
		writeJSON(w, map[string]string{"error": "method not allowed"})
	}
}

// POST /api/webhook/test
func handleWebhookTest(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeJSON(w, map[string]string{"error": "method not allowed"})
		return
	}
	cfg := GetConfig()
	if err := TestWebhook(cfg); err != nil {
		writeJSON(w, map[string]string{"error": err.Error()})
		return
	}
	addLog("info", "Webhook 测试已发送", "")
	writeJSON(w, map[string]string{"status": "sent"})
}

// GET /api/call/state
func handleCallState(w http.ResponseWriter, r *http.Request) {
	state, err := modem.CallState()
	if err != nil {
		writeJSON(w, map[string]string{"state": "idle"})
		return
	}
	writeJSON(w, map[string]string{"state": state})
}

// POST /api/call/dial
func handleCallDial(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeJSON(w, map[string]string{"error": "method not allowed"})
		return
	}
	var req struct {
		Number string `json:"number"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, map[string]string{"error": "invalid JSON"})
		return
	}
	if req.Number == "" {
		writeJSON(w, map[string]string{"error": "号码不能为空"})
		return
	}
	if err := modem.Dial(req.Number); err != nil {
		writeJSON(w, map[string]string{"error": err.Error()})
		return
	}
	addLog("info", "正在拨号", req.Number)

	// Record call in history
	addCallRecord(req.Number, "dial")

	writeJSON(w, map[string]string{"status": "dialing", "number": req.Number})
}

// POST /api/call/hangup
func handleCallHangup(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeJSON(w, map[string]string{"error": "method not allowed"})
		return
	}
	if err := modem.Hangup(); err != nil {
		writeJSON(w, map[string]string{"error": err.Error()})
		return
	}
	addLog("info", "已挂断", "")
	writeJSON(w, map[string]string{"status": "hungup"})
}

// GET/POST /api/phonebook
func handlePhonebook(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case "GET":
		contacts, err := modem.ReadPhonebook()
		if err != nil {
			writeJSON(w, []interface{}{})
			return
		}
		writeJSON(w, contacts)
	case "POST":
		var req struct {
			Name  string `json:"name"`
			Phone string `json:"phone"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeJSON(w, map[string]string{"error": "invalid JSON"})
			return
		}
		if err := modem.AddContact(req.Name, req.Phone); err != nil {
			writeJSON(w, map[string]string{"error": err.Error()})
			return
		}
		addLog("info", "联系人已添加", req.Name)
		writeJSON(w, map[string]string{"status": "added"})
	default:
		writeJSON(w, map[string]string{"error": "method not allowed"})
	}
}

// GET /api/platform
func handlePlatform(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, map[string]interface{}{
		"os":   runtime.GOOS,
		"arch": runtime.GOARCH,
	})
}

// GET /api/devices
func handleDevices(w http.ResponseWriter, r *http.Request) {
	ports := getPlatformPorts()
	available := []string{}
	for _, p := range ports {
		if _, err := os.Stat(p); err == nil {
			available = append(available, p)
		}
	}

	// Also include device info for diagnostics page
	device := map[string]interface{}{
		"signal":   "-",
		"network":  "-",
		"imei":     "-",
		"operator": "-",
		"sim":      "-",
		"time":     "-",
	}
	if modem.IsOpenNonBlocking() {
		if s, err := modem.GetSignal(); err == nil {
			device["signal"] = fmt.Sprintf("%d/31", s)
		}
		if n, err := modem.GetNetwork(); err == nil {
			device["network"] = n
		}
		if i, err := modem.GetIMEI(); err == nil {
			device["imei"] = i
		}
		if o, err := modem.GetOperator(); err == nil {
			device["operator"] = o
		}
		if sim, err := modem.GetIMSI(); err == nil {
			device["sim"] = sim
		}
		if t, err := modem.GetTime(); err == nil {
			device["time"] = t
		}
	}

	writeJSON(w, map[string]interface{}{
		"available_ports": available,
		"all_ports":       ports,
		"is_ml307a":       IsML307A(),
		"config_dir":      GetConfigDir(),
		"device":          device,
	})
}

// POST /api/driver/bind
func handleDriverBind(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeJSON(w, map[string]string{"error": "method not allowed"})
		return
	}
	if err := BindML307ADriver(); err != nil {
		writeJSON(w, map[string]string{"error": err.Error()})
		return
	}
	addLog("info", "驱动绑定成功", "")
	writeJSON(w, map[string]string{"status": "bound"})
}

// POST /api/recover
func handleRecoverModem(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeJSON(w, map[string]string{"error": "method not allowed"})
		return
	}
	// Close current modem connection first
	stopPolling()
	modem.Close()
	// Try USB recovery (Linux only, no-op elsewhere)
	RecoverML307A()
	// Re-find port and reconnect
	port := GetConfig().Serial.Port
	if port == "auto" || port == "" {
		port = FindModemPort()
	}
	if port == "" {
		writeJSON(w, map[string]string{"error": "未找到串口，请检查模块连接"})
		return
	}
	if err := modem.Open(port, 115200); err != nil {
		writeJSON(w, map[string]string{"error": "连接失败: " + err.Error()})
		return
	}
	modem.Init()
	startPolling()
	addLog("info", "模组已恢复", port)
	writeJSON(w, map[string]string{"status": "recovered", "port": port})
}

// GET/POST /api/autostart
func handleAutoStart(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case "GET":
		cfg := GetConfig()
		writeJSON(w, map[string]interface{}{
			"enabled":  cfg.AutoStart,
			"platform": runtime.GOOS,
		})
	case "POST":
		// Toggle: if body is empty or auto_start not specified, toggle the current state
		var req struct {
			AutoStart *bool `json:"auto_start"`
		}
		json.NewDecoder(r.Body).Decode(&req)
		var newVal bool
		cfg := GetConfig()
		if req.AutoStart != nil {
			newVal = *req.AutoStart
		} else {
			newVal = !cfg.AutoStart
		}
		if err := UpdateConfig(func(c *Config) {
			c.AutoStart = newVal
		}); err != nil {
			writeJSON(w, map[string]string{"error": err.Error()})
			return
		}
		addLog("info", "自启动设置已更新", fmt.Sprintf("%v", newVal))
		writeJSON(w, map[string]interface{}{"enabled": newVal})
	default:
		writeJSON(w, map[string]string{"error": "method not allowed"})
	}
}

// GET /api/logs
func handleLogs(w http.ResponseWriter, r *http.Request) {
	logMu.RLock()
	defer logMu.RUnlock()
	if logEntries == nil {
		writeJSON(w, []LogEntry{})
		return
	}
	writeJSON(w, logEntries)
}

// ── SMS Polling ──────────────────────────────────────────────────────────────

func startPolling() {
	runningMu.Lock()
	if running {
		runningMu.Unlock()
		return
	}
	running = true
	runningMu.Unlock()

	pollStopCh = make(chan struct{})
	ch := pollStopCh

	go func() {
		addLog("info", "SMS轮询已启动", "")
		defer func() {
			runningMu.Lock()
			running = false
			runningMu.Unlock()
			addLog("info", "SMS轮询已停止", "")
		}()

		for {
			cfg := GetConfig()
			interval := cfg.PollingSec
			if interval < 1 {
				interval = 2
			}

			select {
			case <-ch:
				return
			case <-time.After(time.Duration(interval) * time.Second):
			}

			pollOnce(cfg)
		}
	}()
}

func stopPolling() {
	runningMu.RLock()
	isRunning := running
	runningMu.RUnlock()
	if !isRunning || pollStopCh == nil {
		return
	}
	close(pollStopCh)
	// Wait briefly for goroutine to exit
	time.Sleep(200 * time.Millisecond)
}

func pollOnce(cfg Config) {
	if !modem.IsOpen() {
		return
	}

	msgs, err := modem.ReadNewSMS()
	if err != nil {
		return // Silent failure, retry next cycle
	}

	for _, msg := range msgs {
		dedupKey := msg.From + "|" + msg.Body
		seenMu.Lock()
		if seenMsgs[dedupKey] {
			seenMu.Unlock()
			continue
		}
		seenMsgs[dedupKey] = true
		smsTotalCount++
		seenMu.Unlock()

		// Add to history (convert SMS time to ISO format)
		entry := HistoryEntry{
			Timestamp: parseSMSTime(msg.Time),
			Phone:     msg.From,
			Content:   cleanSMSBody(msg.Body),
		}
		historyMu.Lock()
		smsHistory = append(smsHistory, entry)
		if len(smsHistory) > maxHistory {
			smsHistory = smsHistory[len(smsHistory)-maxHistory:]
		}
		historyMu.Unlock()

		// Forward
		if ShouldForward(cfg.Filter, msg.From, msg.Body) {
			errs := ForwardSMS(cfg, msg.From, msg.Body, msg.Time)
			if len(errs) > 0 {
				for _, e := range errs {
					addLog("error", "转发失败", e.Error())
				}
			} else {
				addLog("info", "短信已转发", msg.From)
				seenMu.Lock()
				smsTodayCount++
				seenMu.Unlock()
			}
		} else {
			addLog("info", "短信已过滤", msg.From)
			seenMu.Lock()
			smsSkipCount++
			seenMu.Unlock()
		}
	}
}

// ── Web Server ───────────────────────────────────────────────────────────────

func setupRoutes(mux *http.ServeMux) {
	// API routes
	mux.HandleFunc("/api/status", corsMiddleware(handleStatus))
	mux.HandleFunc("/api/start", corsMiddleware(handleStart))
	mux.HandleFunc("/api/stop", corsMiddleware(handleStop))
	mux.HandleFunc("/api/sms", corsMiddleware(handleSMS))
	mux.HandleFunc("/api/sms/send", corsMiddleware(handleSMSSend))
	mux.HandleFunc("/api/sms/delete", corsMiddleware(handleSMSDelete))
	mux.HandleFunc("/api/config", corsMiddleware(handleConfig))
	mux.HandleFunc("/api/config/import", corsMiddleware(handleConfigImport))
	mux.HandleFunc("/api/config/export", corsMiddleware(handleConfigExport))
	mux.HandleFunc("/api/config/reset", corsMiddleware(handleConfigReset))
	mux.HandleFunc("/api/keywords", corsMiddleware(handleKeywords))
	mux.HandleFunc("/api/keywords/add", corsMiddleware(handleKeywords))
	mux.HandleFunc("/api/keywords/del", corsMiddleware(handleKeywords))
	mux.HandleFunc("/api/webhook/test", corsMiddleware(handleWebhookTest))
	mux.HandleFunc("/api/call/state", corsMiddleware(handleCallState))
	mux.HandleFunc("/api/call/dial", corsMiddleware(handleCallDial))
	mux.HandleFunc("/api/call/hangup", corsMiddleware(handleCallHangup))
	mux.HandleFunc("/api/calls", corsMiddleware(handleCalls))
	mux.HandleFunc("/api/phonebook", corsMiddleware(handlePhonebook))
	mux.HandleFunc("/api/phonebook/add", corsMiddleware(handlePhonebook))
	mux.HandleFunc("/api/devices", corsMiddleware(handleDevices))
	mux.HandleFunc("/api/platform", corsMiddleware(handlePlatform))
	mux.HandleFunc("/api/driver/bind", corsMiddleware(handleDriverBind))
	mux.HandleFunc("/api/recover", corsMiddleware(handleRecoverModem))
	mux.HandleFunc("/api/autostart", corsMiddleware(handleAutoStart))
	mux.HandleFunc("/api/logs", corsMiddleware(handleLogs))

	// Static files
	webSub, _ := fs.Sub(webFS, "web")
	fileServer := http.FileServer(http.FS(webSub))

	// noCache middleware for embedded web files
	noCache := func(next http.HandlerFunc) http.HandlerFunc {
		return func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
			w.Header().Set("Pragma", "no-cache")
			w.Header().Set("Expires", "0")
			next(w, r)
		}
	}

	mux.HandleFunc("/web/", noCache(func(w http.ResponseWriter, r *http.Request) {
		r.URL.Path = strings.TrimPrefix(r.URL.Path, "/web")
		fileServer.ServeHTTP(w, r)
	}))

	mux.HandleFunc("/", noCache(func(w http.ResponseWriter, r *http.Request) {
		path := r.URL.Path
		if path == "/" || path == "/index.html" {
			data, _ := webFS.ReadFile("web/index.html")
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			// Inject initial status data so the page shows real data immediately
			html := string(data)
			html = strings.Replace(html, "</head>",
				"<script>window.__INIT__="+mustMarshal(getInitialStatus())+"</script></head>", 1)
			// Also replace default "-" placeholders with real data (works even without JS)
			status := getInitialStatus()
			html = strings.Replace(html, `id="cardOperator">-`, fmt.Sprintf(`id="cardOperator">%s`, status["operator"]), 1)
			html = strings.Replace(html, `id="cardNetwork">-`, fmt.Sprintf(`id="cardNetwork">%s`, status["network"]), 1)
			html = strings.Replace(html, `id="cardIMEI">-`, fmt.Sprintf(`id="cardIMEI">%s`, status["imei"]), 1)
			if s, ok := status["signal"].(int); ok && s > 0 {
				html = strings.Replace(html, `width:0%">`, fmt.Sprintf(`width:%d%%">`, s*100/31), 1)
				html = strings.Replace(html, `class="signal-text">-`, fmt.Sprintf(`class="signal-text">%d/31`, s), 1)
			}
			if running, ok := status["running"].(bool); ok && running {
				html = strings.Replace(html, `id="cardStatus">未连接`, `id="cardStatus">`+
					`<span class="status-led led-green"></span> 运行中`, 1)
				html = strings.Replace(html, `class="dot offline"`, `class="dot online"`, 1)
				html = strings.Replace(html, `id="statusText">未连接`, `id="statusText">已连接`, 1)
			}
			w.Write([]byte(html))
			return
		}
		data, err := webFS.ReadFile("web" + path)
		if err == nil {
			ct := "application/octet-stream"
			if strings.HasSuffix(path, ".css") {
				ct = "text/css; charset=utf-8"
			} else if strings.HasSuffix(path, ".js") {
				ct = "application/javascript; charset=utf-8"
			}
			w.Header().Set("Content-Type", ct)
			w.Write(data)
			return
		}
		fileServer.ServeHTTP(w, r)
	}))
}

// ── Main ─────────────────────────────────────────────────────────────────────

func main() {
	log.SetFlags(log.LstdFlags | log.Lshortfile)
	log.Println("短信转发器启动中...")

	InitConfig()
	addLog("info", "系统启动", "")

	mux := http.NewServeMux()
	setupRoutes(mux)

	listener, err := net.Listen("tcp", "localhost:"+serverPort)
	if err != nil {
		// Port already in use - another instance is running.
		// Show notification and open browser instead of failing silently.
		if isAddrInUse(err) {
			notifyDesktop("SMS Forwarder 已在运行中", "浏览器已打开 http://localhost:"+serverPort)
			openBrowser("http://localhost:" + serverPort)
			os.Exit(0)
		}
		log.Fatalf("监听失败: %v", err)
	}

	server := &http.Server{Handler: mux}
	stopCh = make(chan struct{})

	// Start HTTP server
	go func() {
		if err := server.Serve(listener); err != nil && err != http.ErrServerClosed {
			log.Printf("服务异常: %v", err)
		}
	}()

	addLog("info", "HTTP服务已启动", "http://localhost:"+serverPort)

	// Show startup notification so user knows it's running
	notifyDesktop("SMS Forwarder 已启动", "访问 http://localhost:"+serverPort+" 或点击托盘图标")

	// Auto-connect modem with retry (for slow USB init after boot/replug)
	go func() {
		time.Sleep(1 * time.Second)
		fmt.Println("AUTO-CONNECT goroutine started")
		for attempt := 1; attempt <= 10; attempt++ {
			cfg := GetConfig()
			port := cfg.Serial.Port
			if port == "auto" || port == "" {
				port = FindModemPort()
			}
			if port != "" {
				baud := cfg.Serial.Baudrate
				if baud == 0 {
					baud = 115200
				}
				fmt.Printf("AUTO-CONNECT attempt %d: opening %s @ %d\n", attempt, port, baud)
				if err := modem.Open(port, baud); err != nil {
					fmt.Printf("AUTO-CONNECT Open FAILED: %v\n", err)
				} else {
					if err := modem.Init(); err != nil {
						log.Printf("初始化警告: %v", err)
					}
					startPolling()
					addLog("info", "已自动连接", port)
					log.Printf("✅ 已连接 %s", port)
					return
				}
			}
			fmt.Printf("AUTO-CONNECT attempt %d: no port, retrying in %ds...\n", attempt, attempt*2)
			time.Sleep(time.Duration(attempt*2) * time.Second)
		}
		fmt.Println("AUTO-CONNECT: all retries failed")
	}()

	// Open browser
	go func() {
		time.Sleep(500 * time.Millisecond)
		openBrowser("http://localhost:" + serverPort)
	}()

	// Run system tray (blocks on main thread until Quit)
	log.Printf("运行中 | http://localhost:%s | 右键托盘图标退出", serverPort)
	runSystray(func() {
		log.Println("正在关闭...")
		stopPolling()
		modem.Close()
		close(stopCh)
		server.Close()
		log.Println("已停止")
	})
}

func isAddrInUse(err error) bool {
	if err == nil {
		return false
	}
	// Check for common "address already in use" error messages
	msg := err.Error()
	return strings.Contains(msg, "address already in use") ||
		strings.Contains(msg, "bind: address already in use") ||
		strings.Contains(msg, "in use")
}

// notifyDesktop shows a desktop notification (cross-platform).
// parseSMSTime converts modem SMS timestamp to ISO 8601.
// Input format: "YY/MM/DD,HH:MM:SS+ZZ" where ZZ is quarter-hour offset.
// getInitialStatus returns current modem status for pre-populating the dashboard.
func getInitialStatus() map[string]interface{} {
	signal, operator, imei, network := 0, "", "", ""
	if modem.IsOpenNonBlocking() {
		if s, err := modem.GetSignal(); err == nil {
			signal = s
		}
		if i, err := modem.GetIMEI(); err == nil {
			imei = i
		}
		if o, err := modem.GetOperator(); err == nil {
			operator = o
		}
		if n, err := modem.GetNetwork(); err == nil {
			network = n
		}
	}
	runningMu.RLock()
	isRunning := running
	runningMu.RUnlock()
	return map[string]interface{}{
		"running":  isRunning,
		"polling":  isRunning,
		"signal":   signal,
		"imei":     imei,
		"operator": operator,
		"network":  network,
		"port":     serverPort,
	}
}

func mustMarshal(v interface{}) string {
	b, _ := json.Marshal(v)
	return string(b)
}

func parseSMSTime(raw string) string {
	if raw == "" {
		return time.Now().Format("2006-01-02T15:04:05+08:00")
	}
	// Parse: YY/MM/DD,HH:MM:SS+TZ
	var yy, mm, dd, hh, mi, ss, tz int
	_, err := fmt.Sscanf(raw, "%d/%d/%d,%d:%d:%d+%d", &yy, &mm, &dd, &hh, &mi, &ss, &tz)
	if err != nil {
		// Try without timezone
		_, err = fmt.Sscanf(raw, "%d/%d/%d,%d:%d:%d", &yy, &mm, &dd, &hh, &mi, &ss)
		if err != nil {
			return raw // Return as-is if unparseable
		}
		tz = 32 // Default to +8:00 (China)
	}
	// +tz is quarter hours, e.g., +32 = +8:00
	tzHours := tz / 4
	tzMins := (tz % 4) * 15
	tzOffset := fmt.Sprintf("%+03d:%02d", tzHours, tzMins)
	return fmt.Sprintf("20%02d-%02d-%02dT%02d:%02d:%02d%s", yy, mm, dd, hh, mi, ss, tzOffset)
}

func notifyDesktop(title, body string) {
	switch runtime.GOOS {
	case "linux":
		exec.Command("notify-send", title, body, "--icon=dialog-information", "-t", "3000").Start()
	case "darwin":
		exec.Command("osascript", "-e",
			fmt.Sprintf(`display notification "%s" with title "%s"`, body, title)).Start()
	case "windows":
		// Windows 10+ toast via PowerShell
		exec.Command("powershell", "-Command",
			fmt.Sprintf(`New-BurntToastNotification -Text "%s", "%s"`, title, body)).Start()
	}
}

// GET /api/calls
func handleCalls(w http.ResponseWriter, r *http.Request) {
	callMu.RLock()
	defer callMu.RUnlock()
	if callHistory == nil {
		writeJSON(w, []CallRecord{})
		return
	}
	writeJSON(w, callHistory)
}

func addCallRecord(number, callType string) {
	callMu.Lock()
	defer callMu.Unlock()
	callHistory = append(callHistory, CallRecord{
		Time:     time.Now().Format("2006-01-02 15:04:05"),
		Number:   number,
		Type:     callType,
		Duration: "-",
	})
	if len(callHistory) > maxHistory {
		callHistory = callHistory[len(callHistory)-maxHistory:]
	}
}

func openBrowser(url string) {
	time.Sleep(300 * time.Millisecond)
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "linux":
		cmd = exec.Command("xdg-open", url)
	case "darwin":
		cmd = exec.Command("open", url)
	case "windows":
		cmd = exec.Command("cmd", "/c", "start", url)
	default:
		return
	}
	cmd.Start()
}
