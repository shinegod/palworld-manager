package handler

import (
	"encoding/json"
	"io"
	"net/http"
	"sync"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/shinegod/palworld-manager/internal/store"
)

// HookDashboardHandler — hook-only模式下用PalHook数据源提供仪表盘/地图/服务器信息
type HookDashboardHandler struct {
	configStore *store.ConfigStore
	client      *http.Client
}

func NewHookDashboardHandler(cs *store.ConfigStore) *HookDashboardHandler {
	return &HookDashboardHandler{configStore: cs, client: &http.Client{Timeout: 30 * time.Second}}
}

func (h *HookDashboardHandler) configured() bool {
	cc := h.configStore.GetConnectionConfig()
	return cc != nil && cc.PalHookURL != ""
}

func (h *HookDashboardHandler) hookGet(path string) ([]byte, int, error) {
	cc := h.configStore.GetConnectionConfig()
	if cc == nil || cc.PalHookURL == "" {
		return nil, http.StatusServiceUnavailable, nil
	}
	req, err := http.NewRequest(http.MethodGet, cc.PalHookURL+path, nil)
	if err != nil {
		return nil, 0, err
	}
	req.SetBasicAuth("admin", cc.PalHookPassword)
	resp, err := h.client.Do(req)
	if err != nil {
		return nil, http.StatusBadGateway, err
	}
	defer func() { _ = resp.Body.Close() }()
	body, _ := io.ReadAll(resp.Body)
	return body, resp.StatusCode, nil
}

// GetRealtime 合并 PalHook /metrics 与 /players (并行请求, 减少串行等待)
func (h *HookDashboardHandler) GetRealtime(c *gin.Context) {
	if !h.configured() {
		c.JSON(http.StatusServiceUnavailable, gin.H{"error": "PalHook未配置，请在 设置→连接配置 中填写 PalHook 地址"})
		return
	}
	// 各 goroutine 只写自己的变量, 避免共享 struct 造成数据竞争
	var metrics map[string]any
	var players []any
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		m := map[string]any{}
		mb, code, err := h.hookGet("/metrics")
		if err != nil || code != http.StatusOK || json.Unmarshal(mb, &m) != nil {
			m = map[string]any{}
		}
		metrics = m
	}()
	go func() {
		defer wg.Done()
		p := []any{}
		pb, pcode, _ := h.hookGet("/players")
		if pcode == http.StatusOK {
			var pr struct {
				Players []any `json:"players"`
			}
			if json.Unmarshal(pb, &pr) == nil && pr.Players != nil {
				p = pr.Players
			}
		}
		players = p
	}()
	wg.Wait()
	c.JSON(http.StatusOK, gin.H{"metrics": metrics, "players": players})
}

// GetInfo 服务器基本信息 (来自 PalHook /health)
func (h *HookDashboardHandler) GetInfo(c *gin.Context) {
	body, code, err := h.hookGet("/health")
	if err != nil {
		c.JSON(http.StatusBadGateway, gin.H{"error": "PalHook连接失败"})
		return
	}
	if code != http.StatusOK {
		c.JSON(code, gin.H{"error": "PalHook health failed"})
		return
	}
	var health map[string]any
	_ = json.Unmarshal(body, &health)
	servername, _ := health["server_name"].(string)
	if servername == "" {
		servername = "未知服务器"
	}
	c.JSON(http.StatusOK, gin.H{
		"version":     health["version"],
		"servername":  servername,
		"description": "hook-only",
		"pid":         health["pid"],
	})
}

func (h *HookDashboardHandler) GetTrends(c *gin.Context)   { c.JSON(http.StatusOK, []any{}) }

// GetPlayerPositions 世界地图点位 (来自 PalHook /players 的坐标)
func (h *HookDashboardHandler) GetPlayerPositions(c *gin.Context) {
	body, code, err := h.hookGet("/players")
	if err != nil || code != http.StatusOK {
		c.JSON(http.StatusBadGateway, gin.H{"error": "PalHook连接失败"})
		return
	}
	var pr struct {
		Players []map[string]any `json:"players"`
	}
	if json.Unmarshal(body, &pr) != nil {
		c.JSON(http.StatusBadGateway, gin.H{"error": "PalHook响应解析失败"})
		return
	}
	type mapPoint struct {
		Name  string  `json:"name"`
		Uid   string  `json:"uid"`
		Level int     `json:"level"`
		X     float64 `json:"x"`
		Y     float64 `json:"y"`
		Z     float64 `json:"z"`
	}
	out := make([]mapPoint, 0, len(pr.Players))
	for _, p := range pr.Players {
		mp := mapPoint{}
		if v, ok := p["name"].(string); ok {
			mp.Name = v
		}
		if v, ok := p["uid"].(string); ok {
			mp.Uid = v
		}
		if v, ok := p["level"].(float64); ok {
			mp.Level = int(v)
		}
		if v, ok := p["x"].(float64); ok {
			mp.X = v
		}
		if v, ok := p["y"].(float64); ok {
			mp.Y = v
		}
		if v, ok := p["z"].(float64); ok {
			mp.Z = v
		}
		out = append(out, mp)
	}
	c.JSON(http.StatusOK, out)
}

// StartHookHistoryRecorder 后台轻量轮询: 把在线玩家快照写入历史库 (60s一次, 只打PalHook),
// 顺带跑反作弊检测与服务器在线/离线告警
func StartHookHistoryRecorder(cs *store.ConfigStore, ps *store.PlayerStore, ac *AnticheatHandler, stop <-chan struct{}) {
	const interval = 60 * time.Second
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	client := &http.Client{Timeout: 30 * time.Second}
	hookOK := true // 上一次轮询的连接状态 (用于 在线->离线 转换告警)

	for {
		select {
		case <-stop:
			return
		case <-ticker.C:
			cc := cs.GetConnectionConfig()
			if cc == nil || cc.PalHookURL == "" {
				continue
			}
			req, err := http.NewRequest(http.MethodGet, cc.PalHookURL+"/players", nil)
			if err != nil {
				if hookOK {
					_ = ac.alerts.AddAlert("error", "server", "PalHook 连接失败: "+err.Error(), "")
					hookOK = false
				}
				continue
			}
			req.SetBasicAuth("admin", cc.PalHookPassword)
			resp, err := client.Do(req)
			if err != nil {
				if hookOK {
					_ = ac.alerts.AddAlert("error", "server", "PalHook 连接失败: "+err.Error(), "")
					hookOK = false
				}
				continue
			}
			body, _ := io.ReadAll(resp.Body)
			_ = resp.Body.Close()
			var pr struct {
				Players []map[string]any `json:"players"`
			}
			if json.Unmarshal(body, &pr) != nil {
				continue
			}
			if !hookOK {
				_ = ac.alerts.AddAlert("info", "server", "PalHook 已恢复连接", "")
				hookOK = true
			}

			// 本轮在线的 uid; 上一轮在线但这轮不在的要标记离线,
			// 否则历史列表里的人永远显示"在线"
			nowOnline := make(map[string]bool, len(pr.Players))
			snapshots := make([]PlayerSnapshot, 0, len(pr.Players))
			for _, p := range pr.Players {
				name, _ := p["name"].(string)
				uid, _ := p["uid"].(string)
				if name == "" || uid == "" {
					continue
				}
				lvl := 0
				if v, ok := p["level"].(float64); ok {
					lvl = int(v)
				}
				ip, _ := p["ip"].(string)
				platform, _ := p["platform"].(string)
				nowOnline[uid] = true
				_ = ps.UpsertPlayerSnapshot(uid, name, ip, platform, lvl)
				// 在线时长累加: 每轮 +interval
				_ = ps.AddPlaytime(uid, int64(interval.Seconds()))

				snap := PlayerSnapshot{UID: uid, Name: name, IP: ip, Platform: platform, Level: lvl}
				if v, ok := p["exp"].(float64); ok {
					snap.Exp = int64(v)
				}
				if v, ok := p["ping"].(float64); ok {
					snap.Ping = v
				}
				snapshots = append(snapshots, snap)
			}

			prevOnline, err := ps.GetOnlineUIDs()
			if err != nil {
				continue
			}
			for uid := range prevOnline {
				if !nowOnline[uid] {
					_ = ps.SetOffline(uid)
				}
			}

			// 反作弊检测 (自动模式, 去重后只有新异常才产生标记)
			_, _ = ac.RunChecks("auto", snapshots)
		}
	}
}
