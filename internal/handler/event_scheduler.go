package handler

import (
	"bytes"
	"database/sql"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/shinegod/palworld-manager/internal/store"
)

// EventAction 事件动作: {"type":"broadcast"|"chat"|"restart","params":{...}}
type EventAction struct {
	Type   string         `json:"type"`
	Params map[string]any `json:"params"`
}

// EventScheduler 定时事件执行器: 每30秒检查一次到期事件并执行动作
// (旧版只有存储和"手动触发"占位, 从不真正执行, 这里补全)
type EventScheduler struct {
	db     *sql.DB
	cs     *store.ConfigStore
	client *http.Client
}

func NewEventScheduler(db *sql.DB, cs *store.ConfigStore) *EventScheduler {
	return &EventScheduler{db: db, cs: cs, client: &http.Client{Timeout: 30 * time.Second}}
}

// hookPost 往 PalHook 发 POST JSON
func (s *EventScheduler) hookPost(path string, payload any) error {
	cc := s.cs.GetConnectionConfig()
	if cc == nil || cc.PalHookURL == "" {
		return fmt.Errorf("PalHook未配置")
	}
	body, _ := json.Marshal(payload)
	req, err := http.NewRequest(http.MethodPost, cc.PalHookURL+path, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.SetBasicAuth("admin", cc.PalHookPassword)
	req.Header.Set("Content-Type", "application/json")
	resp, err := s.client.Do(req)
	if err != nil {
		return err
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode >= 300 {
		return fmt.Errorf("PalHook %s 状态码 %d", path, resp.StatusCode)
	}
	return nil
}

// ExecuteActions 执行事件动作列表 (广播/聊天/重启)
func (s *EventScheduler) ExecuteActions(actionsJSON string) []error {
	var actions []EventAction
	if err := json.Unmarshal([]byte(actionsJSON), &actions); err != nil {
		return []error{fmt.Errorf("actions JSON解析失败: %w", err)}
	}
	var errs []error
	for _, a := range actions {
		switch strings.ToLower(a.Type) {
		case "broadcast", "announce":
			msg, _ := a.Params["message"].(string)
			if msg == "" {
				errs = append(errs, fmt.Errorf("broadcast缺少message参数"))
				continue
			}
			if err := s.hookPost("/announce", map[string]string{"message": msg}); err != nil {
				errs = append(errs, err)
			} else {
				log.Printf("[event] 广播: %s", msg)
			}
		case "chat":
			msg, _ := a.Params["message"].(string)
			if msg == "" {
				errs = append(errs, fmt.Errorf("chat缺少message参数"))
				continue
			}
			if err := s.hookPost("/chat", map[string]string{"message": msg}); err != nil {
				errs = append(errs, err)
			} else {
				log.Printf("[event] 聊天: %s", msg)
			}
		case "restart":
			if err := s.hookPost("/saves/restart", map[string]any{}); err != nil {
				errs = append(errs, err)
			} else {
				log.Printf("[event] 已发送服务器重启指令")
			}
		default:
			errs = append(errs, fmt.Errorf("未知动作类型: %s", a.Type))
		}
	}
	return errs
}

// cronMatch 极简5段cron匹配 (分 时 日 月 周; 支持*、数字、逗号列表)
func cronMatch(field string, v int) bool {
	field = strings.TrimSpace(field)
	if field == "*" {
		return true
	}
	for _, part := range strings.Split(field, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		if n, err := strconv.Atoi(part); err == nil && n == v {
			return true
		}
	}
	return false
}

func cronDue(expr string, now time.Time) bool {
	parts := strings.Fields(expr)
	if len(parts) != 5 {
		return false
	}
	return cronMatch(parts[0], now.Minute()) &&
		cronMatch(parts[1], now.Hour()) &&
		cronMatch(parts[2], now.Day()) &&
		cronMatch(parts[3], int(now.Month())) &&
		cronMatch(parts[4], int(now.Weekday()))
}

// due 判断事件是否到期
func due(scheduleType, scheduleValue string, lastRun *string, createdAt time.Time, now time.Time) bool {
	switch scheduleType {
	case "once":
		// 只执行一次: 之前没跑过且时间已到
		if lastRun != nil && *lastRun != "" {
			return false
		}
		t := parseScheduleTime(scheduleValue, createdAt)
		return !now.Before(t)
	case "recurring":
		// schedule_value = 间隔分钟数
		mins, err := strconv.Atoi(strings.TrimSpace(scheduleValue))
		if err != nil || mins <= 0 {
			return false
		}
		base := createdAt
		if lastRun != nil && *lastRun != "" {
			if t, err := time.Parse(time.RFC3339, *lastRun); err == nil {
				base = t
			}
		}
		return now.Sub(base) >= time.Duration(mins)*time.Minute
	case "cron":
		// 每分钟粒度执行检查, 靠 last_run 去重避免重复触发
		return cronDue(scheduleValue, now)
	}
	return false
}

func parseScheduleTime(v string, fallback time.Time) time.Time {
	layouts := []string{
		"2006-01-02 15:04:05",
		"2006-01-02 15:04",
		time.RFC3339,
	}
	for _, l := range layouts {
		if t, err := time.ParseInLocation(l, strings.TrimSpace(v), time.Local); err == nil {
			return t
		}
	}
	return fallback
}

// Start 调度循环 (每30秒)
func (s *EventScheduler) Start(stop <-chan struct{}) {
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-stop:
			return
		case <-ticker.C:
			s.tick()
		}
	}
}

func (s *EventScheduler) tick() {
	rows, err := s.db.Query("SELECT id, schedule_type, schedule_value, actions, last_run, created_at FROM events WHERE enabled = 1")
	if err != nil {
		log.Printf("[event] 查询失败: %v", err)
		return
	}
	type ev struct {
		id            int64
		scheduleType  string
		scheduleValue string
		actions       string
		lastRun       sql.NullString
		createdAt     time.Time
	}
	var list []ev
	for rows.Next() {
		var e ev
		if rows.Scan(&e.id, &e.scheduleType, &e.scheduleValue, &e.actions, &e.lastRun, &e.createdAt) == nil {
			list = append(list, e)
		}
	}
	_ = rows.Close()

	now := time.Now()
	for _, e := range list {
		var lastRun *string
		if e.lastRun.Valid {
			lastRun = &e.lastRun.String
		}
		if !due(e.scheduleType, e.scheduleValue, lastRun, e.createdAt, now) {
			continue
		}
		// cron类型: 同一分钟内可能重复触发, 用 last_run 去重
		if e.scheduleType == "cron" && lastRun != nil {
			if t, err := time.Parse(time.RFC3339, *lastRun); err == nil && now.Sub(t) < time.Minute {
				continue
			}
		}
		errs := s.ExecuteActions(e.actions)
		_, _ = s.db.Exec("UPDATE events SET last_run = ? WHERE id = ?", now.Format(time.RFC3339), e.id)
		if e.scheduleType == "once" {
			_, _ = s.db.Exec("UPDATE events SET enabled = 0 WHERE id = ?", e.id)
		}
		if len(errs) > 0 {
			log.Printf("[event] id=%d 执行有错误: %v", e.id, errs)
		} else {
			log.Printf("[event] id=%d 已执行", e.id)
		}
	}
}
