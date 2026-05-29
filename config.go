package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
)

// ── Config Types ─────────────────────────────────────────────────────────────

type SerialConfig struct {
	Port     string `json:"port"`
	Baudrate int    `json:"baudrate"`
}

type DingtalkConfig struct {
	Enabled bool   `json:"enabled"`
	Mode    string `json:"mode"`    // "keyword" or "sign"
	Token   string `json:"token"`   // access_token
	Keyword string `json:"keyword"` // 自定义关键词
	Secret  string `json:"secret"`  // 加签密钥
}

type WechatConfig struct {
	Enabled bool   `json:"enabled"`
	Key     string `json:"key"` // webhook key
}

type FilterConfig struct {
	Mode             string   `json:"mode"`               // "all", "whitelist", "blacklist"
	Keywords         []string `json:"keywords"`           // 关键词列表
	ForwardLinks     bool     `json:"forward_links"`      // 提取并转发链接
	ForwardLinksOnly bool     `json:"forward_links_only"` // 仅转发链接
	ForwardTemplate  string   `json:"forward_template"`    // "default","short","code_only"
}

type Config struct {
	Serial      SerialConfig      `json:"serial"`
	Dingtalk    DingtalkConfig    `json:"dingtalk"`
	Wechat      WechatConfig      `json:"wechat"`
	Filter      FilterConfig      `json:"filter"`
	AutoCleanup AutoCleanupConfig `json:"auto_cleanup"`
	PollingSec  int               `json:"polling_sec"`
	AutoStart   bool              `json:"auto_start"`
	Theme       string            `json:"theme"`
}

// ── Auto Cleanup Config ─────────────────────────────────────────────────────

type AutoCleanupConfig struct {
	Enabled     bool `json:"enabled"`
	MaxAgeHours int  `json:"max_age_hours"` // SMS older than this will be cleaned
	MaxCount    int  `json:"max_count"`     // Max SMS to keep, old ones deleted first
	Threshold   int  `json:"threshold"`     // Trigger cleanup when count exceeds this
}

// ── Default Config ───────────────────────────────────────────────────────────

func DefaultConfig() Config {
	return Config{
		Serial: SerialConfig{
			Port:     "auto",
			Baudrate: 115200,
		},
		Dingtalk: DingtalkConfig{
			Enabled: true,
			Mode:    "keyword",
			Token:   "",
			Keyword: "短信",
			Secret:  "",
		},
		Wechat: WechatConfig{
			Enabled: false,
			Key:     "",
		},
		Filter: FilterConfig{
			Mode:             "whitelist",
			Keywords:         []string{"验证码"},
			ForwardLinks:     true,
			ForwardLinksOnly: false,
			ForwardTemplate:  "default",
		},
		AutoCleanup: AutoCleanupConfig{
			Enabled:     true,
			MaxAgeHours: 24,
			MaxCount:    150,
			Threshold:   160,
		},
		PollingSec: 1,
		AutoStart:  false,
		Theme:      "dark",
	}
}

// ── Config Manager ───────────────────────────────────────────────────────────

var (
	configMu sync.RWMutex
	appCfg   Config
	cfgPath  string
)

// GetConfigDir returns the config directory.
func GetConfigDir() string {
	home, err := os.UserHomeDir()
	if err != nil {
		home = "/tmp"
	}
	return filepath.Join(home, ".config", "sms-forwarder-go")
}

// InitConfig loads or creates the config file.
func InitConfig() error {
	cfgPath = filepath.Join(GetConfigDir(), "config.json")
	configMu.Lock()
	defer configMu.Unlock()

	data, err := os.ReadFile(cfgPath)
	if err != nil {
		if os.IsNotExist(err) {
			appCfg = DefaultConfig()
			return SaveConfig()
		}
		return fmt.Errorf("read config: %w", err)
	}

	appCfg = DefaultConfig() // start with defaults
	if err := json.Unmarshal(data, &appCfg); err != nil {
		return fmt.Errorf("parse config: %w", err)
	}
	return nil
}

// GetConfig returns a copy of the current config.
func GetConfig() Config {
	configMu.RLock()
	defer configMu.RUnlock()
	return appCfg
}

// SaveConfig writes the current config to disk.
func SaveConfig() error {
	dir := filepath.Dir(cfgPath)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return fmt.Errorf("create config dir: %w", err)
	}

	data, err := json.MarshalIndent(appCfg, "", "  ")
	if err != nil {
		return fmt.Errorf("marshal config: %w", err)
	}
	return os.WriteFile(cfgPath, data, 0644)
}

// UpdateConfig applies a patch to the config.
func UpdateConfig(updateFunc func(*Config)) error {
	configMu.Lock()
	updateFunc(&appCfg)
	configMu.Unlock()
	return SaveConfig()
}

// ResetConfig resets config to defaults.
func ResetConfig() error {
	configMu.Lock()
	appCfg = DefaultConfig()
	configMu.Unlock()
	return SaveConfig()
}

// ImpordConfig imports config from JSON data.
func ImportConfig(data []byte) error {
	var newCfg Config
	if err := json.Unmarshal(data, &newCfg); err != nil {
		return fmt.Errorf("invalid config JSON: %w", err)
	}
	configMu.Lock()
	appCfg = newCfg
	configMu.Unlock()
	return SaveConfig()
}

// ExportConfig returns the config as JSON bytes.
func ExportConfig() ([]byte, error) {
	configMu.RLock()
	defer configMu.RUnlock()
	return json.MarshalIndent(appCfg, "", "  ")
}

// UpdateKeywords saves the keyword list.
func UpdateKeywords(keywords []string) error {
	return UpdateConfig(func(c *Config) {
		c.Filter.Keywords = keywords
	})
}

// UpdateDingtalk saves dingtalk settings.
func UpdateDingtalk(token, mode, keyword, secret string, enabled bool) error {
	return UpdateConfig(func(c *Config) {
		c.Dingtalk.Token = token
		c.Dingtalk.Mode = mode
		c.Dingtalk.Keyword = keyword
		c.Dingtalk.Secret = secret
		c.Dingtalk.Enabled = enabled
	})
}

// UpdateWechat saves wechat settings.
func UpdateWechat(key string, enabled bool) error {
	return UpdateConfig(func(c *Config) {
		c.Wechat.Key = key
		c.Wechat.Enabled = enabled
	})
}
