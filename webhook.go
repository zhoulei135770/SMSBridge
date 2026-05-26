package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"strings"
	"time"
)

// ── DingTalk Webhook ─────────────────────────────────────────────────────────

// DingTalkMessage is the webhook payload for DingTalk.
type DingTalkMessage struct {
	MsgType  string           `json:"msgtype"`
	Text     *DingTalkText    `json:"text,omitempty"`
	Markdown *DingTalkMarkdown `json:"markdown,omitempty"`
	At       *DingTalkAt       `json:"at,omitempty"`
}

type DingTalkText struct {
	Content string `json:"content"`
}

type DingTalkMarkdown struct {
	Title string `json:"title"`
	Text  string `json:"text"`
}

type DingTalkAt struct {
	AtMobiles []string `json:"atMobiles,omitempty"`
	IsAtAll   bool     `json:"isAtAll,omitempty"`
}

// SendDingtalk sends a message via DingTalk webhook (with retry).
func SendDingtalk(cfg DingtalkConfig, title, content string) error {
	if !cfg.Enabled || cfg.Token == "" {
		return nil
	}

	webhookURL := fmt.Sprintf("https://oapi.dingtalk.com/robot/send?access_token=%s", cfg.Token)

	if cfg.Mode == "sign" && cfg.Secret != "" {
		timestamp := time.Now().UnixMilli()
		sign := dingTalkSign(cfg.Secret, timestamp)
		webhookURL += fmt.Sprintf("&timestamp=%d&sign=%s", timestamp, sign)
	}

	fullContent := content
	if cfg.Mode == "keyword" && cfg.Keyword != "" {
		if !strings.Contains(fullContent, cfg.Keyword) {
			fullContent = cfg.Keyword + "\n" + fullContent
		}
	}

	msg := DingTalkMessage{
		MsgType: "text",
		Text: &DingTalkText{
			Content: fullContent,
		},
	}

	data, err := json.Marshal(msg)
	if err != nil {
		return fmt.Errorf("marshal dingtalk msg: %w", err)
	}

	// Retry up to 3 times with backoff
	var lastErr error
	for i := 0; i < 2; i++ {
		client := &http.Client{Timeout: 5 * time.Second}
		resp, err := client.Post(webhookURL, "application/json", bytes.NewReader(data))
		if err != nil {
			lastErr = fmt.Errorf("dingtalk request: %w", err)
			time.Sleep(time.Duration(i+1) * 2 * time.Second)
			continue
		}
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		var result struct {
			ErrCode int    `json:"errcode"`
			ErrMsg  string `json:"errmsg"`
		}
		json.Unmarshal(body, &result)
		if result.ErrCode != 0 {
			return fmt.Errorf("dingtalk error %d: %s", result.ErrCode, result.ErrMsg)
		}
		return nil
	}
	return lastErr
}

func dingTalkSign(secret string, timestamp int64) string {
	mac := hmac.New(sha256.New, []byte(secret))
	fmt.Fprintf(mac, "%d\n%s", timestamp, secret)
	return url.QueryEscape(base64.StdEncoding.EncodeToString(mac.Sum(nil)))
}

// ── WeChat Work Webhook ──────────────────────────────────────────────────────

// WechatMessage is the webhook payload for WeChat Work.
type WechatMessage struct {
	MsgType  string       `json:"msgtype"`
	Text     *WechatText `json:"text,omitempty"`
	Markdown *WechatMarkdown `json:"markdown,omitempty"`
}

type WechatText struct {
	Content             string   `json:"content"`
	MentionedMobileList []string `json:"mentioned_mobile_list,omitempty"`
}

type WechatMarkdown struct {
	Content string `json:"content"`
}

// SendWechat sends a message via WeChat Work webhook (with retry).
func SendWechat(cfg WechatConfig, content string) error {
	if !cfg.Enabled || cfg.Key == "" {
		return nil
	}

	webhookURL := fmt.Sprintf("https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=%s", cfg.Key)

	msg := WechatMessage{
		MsgType: "text",
		Text: &WechatText{
			Content: content,
		},
	}

	data, err := json.Marshal(msg)
	if err != nil {
		return fmt.Errorf("marshal wechat msg: %w", err)
	}

	var lastErr error
	for i := 0; i < 2; i++ {
		client := &http.Client{Timeout: 5 * time.Second}
		resp, err := client.Post(webhookURL, "application/json", bytes.NewReader(data))
		if err != nil {
			lastErr = fmt.Errorf("wechat request: %w", err)
			time.Sleep(time.Duration(i+1) * 2 * time.Second)
			continue
		}
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		var result struct {
			ErrCode int    `json:"errcode"`
			ErrMsg  string `json:"errmsg"`
		}
		json.Unmarshal(body, &result)
		if result.ErrCode != 0 {
			return fmt.Errorf("wechat error %d: %s", result.ErrCode, result.ErrMsg)
		}
		return nil
	}
	return lastErr
}

// ── Filter ───────────────────────────────────────────────────────────────────

// ShouldForward checks if an SMS should be forwarded based on filter rules.
func ShouldForward(filter FilterConfig, from, body string) bool {
	if filter.Mode == "all" || filter.Mode == "" {
		return true
	}

	if len(filter.Keywords) == 0 {
		return filter.Mode == "all"
	}

	matched := matchesKeyword(body, filter.Keywords) || matchesKeyword(from, filter.Keywords)

	switch filter.Mode {
	case "whitelist":
		return matched
	case "blacklist":
		return !matched
	default:
		return true
	}
}

func matchesKeyword(text string, keywords []string) bool {
	for _, kw := range keywords {
		if kw == "" {
			continue
		}
		if strings.Contains(text, kw) {
			return true
		}
	}
	return false
}

// ── Link Extraction ──────────────────────────────────────────────────────────

var linkRegex = regexp.MustCompile(`https?://[^\s<>"{}|\\^` + "`" + `\[\]]+`)

// ExtractLinks extracts URLs from text.
func ExtractLinks(text string) []string {
	matches := linkRegex.FindAllString(text, -1)
	seen := make(map[string]bool)
	var result []string
	for _, link := range matches {
		if !seen[link] {
			seen[link] = true
			result = append(result, link)
		}
	}
	if result == nil {
		return []string{}
	}
	return result
}

// ── Forward ──────────────────────────────────────────────────────────────────

// ForwardSMS sends an SMS to configured webhooks.
func ForwardSMS(cfg Config, from, body, timeStr string) []error {
	var errs []error

	if !ShouldForward(cfg.Filter, from, body) {
		return nil
	}

	// Build content
	content := buildForwardContent(cfg, from, body, timeStr)

	if cfg.Dingtalk.Enabled && cfg.Dingtalk.Token != "" {
		title := fmt.Sprintf("短信 - %s", from)
		if err := SendDingtalk(cfg.Dingtalk, title, content); err != nil {
			errs = append(errs, fmt.Errorf("dingtalk: %w", err))
		}
	}

	if cfg.Wechat.Enabled && cfg.Wechat.Key != "" {
		if err := SendWechat(cfg.Wechat, content); err != nil {
			errs = append(errs, fmt.Errorf("wechat: %w", err))
		}
	}

	return errs
}

func buildForwardContent(cfg Config, from, body, timeStr string) string {
	// Links only mode
	if cfg.Filter.ForwardLinksOnly {
		links := ExtractLinks(body)
		if len(links) > 0 {
			parts := []string{"> " + from, "> " + timeStr, "> 链接:", ""}
			for _, link := range links {
				parts = append(parts, "> "+link)
			}
			return strings.Join(parts, "\n")
		}
		return ""
	}

	// Verification code extraction mode
	if cfg.Filter.ForwardTemplate == "code_only" {
		code := extractVerificationCode(body)
		if code != "" {
			return fmt.Sprintf("【%s】%s", from, code)
		}
		// Fallback: just show sender
		return "> " + from + "\n> " + body
	}

	// Short mode: compact single line
	if cfg.Filter.ForwardTemplate == "short" {
		code := extractVerificationCode(body)
		if code != "" {
			return fmt.Sprintf("【%s】验证码: %s", from, code)
		}
		return fmt.Sprintf("【%s】%s", from, truncate(body, 60))
	}

	// Default mode: clean format
	parts := []string{
		"**【" + from + "】** " + timeStr,
		"",
		truncate(body, 200),
	}

	// Append links
	if cfg.Filter.ForwardLinks {
		links := ExtractLinks(body)
		if len(links) > 0 {
			parts = append(parts, "")
			for _, link := range links {
				parts = append(parts, link)
			}
		}
	}

	return strings.Join(parts, "\n")
}

// extractVerificationCode tries to find a verification code in the SMS body.
func extractVerificationCode(body string) string {
	// Common patterns in Chinese verification SMS
	patterns := []string{
		`验证码[：:]*\s*(\d{4,8})`,
		`验证码[是为：:]*\s*(\d{4,8})`,
		`code[：:]*\s*(\d{4,8})`,
		`\b(\d{4,8})\s*[是为].*验证码`,
		`(\d{4,8})[^\d]*$`,
	}
	for _, p := range patterns {
		re := regexp.MustCompile(p)
		if m := re.FindStringSubmatch(body); len(m) > 1 {
			return m[1]
		}
	}
	return ""
}

func truncate(s string, n int) string {
	runes := []rune(s)
	if len(runes) <= n {
		return s
	}
	return string(runes[:n]) + "..."
}

// ── Webhook Test ─────────────────────────────────────────────────────────────

// TestWebhook sends a test message.
func TestWebhook(cfg Config) error {
	testContent := "这是一条测试消息，来自短信转发器。\n> 时间: " + time.Now().Format("2006-01-02 15:04:05")

	var lastErr error

	if cfg.Dingtalk.Enabled && cfg.Dingtalk.Token != "" {
		if err := SendDingtalk(cfg.Dingtalk, "测试消息", testContent); err != nil {
			lastErr = err
		}
	}

	if cfg.Wechat.Enabled && cfg.Wechat.Key != "" {
		if err := SendWechat(cfg.Wechat, testContent); err != nil {
			lastErr = err
		}
	}

	return lastErr
}
