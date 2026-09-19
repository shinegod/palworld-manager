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
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	return body, resp.StatusCode, nil
}

// GetRealtime 合并 PalHook /metrics 与 /players (并行请求, 减少串行等待)
func (h *HookDashboardHandler) GetRealtime(c *gin.Context) {
	if !h.configured() {
		c.JSON(http.StatusServiceUnavailable, gin.H{"error": "PalHook未配置，请在 设置→连接配置 中填写 PalHook 地址"})
		return
	}
	type rtResult struct {
		metrics map[string]any
		players []any
	}
	var res rtResult
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		metrics := map[string]any{}
		mb, code, err := h.hookGet("/metrics")
		if err != nil || code != http.StatusOK || json.Unmarshal(mb, &metrics) != nil {
			metrics = map[string]any{}
		}
		res.metrics = metrics
	}()
	go func() {
		defer wg.Done()
		players := []any{}
		pb, pcode, _ := h.hookGet("/players")
		if pcode == http.StatusOK {
			var pr struct {
				Players []any `json:"players"`
			}
			if json.Unmarshal(pb, &pr) == nil && pr.Players != nil {
				players = pr.Players
			}
		}
		res.players = players
	}()
	wg.Wait()
	c.JSON(http.StatusOK, gin.H{"metrics": res.metrics, "players": res.players})
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
func (h *HookDashboardHandler) GetAlerts(c *gin.Context)   { c.JSON(http.StatusOK, []any{}) }
func (h *HookDashboardHandler) AckAlert(c *gin.Context)    { c.JSON(http.StatusOK, gin.H{"status": "ok"}) }
func (h *HookDashboardHandler) ClearAlerts(c *gin.Context) { c.JSON(http.StatusOK, gin.H{"status": "ok"}) }

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

// StartHookHistoryRecorder 后台轻量轮询: 把在线玩家快照写入历史库 (60s一次, 只打PalHook)
func StartHookHistoryRecorder(cs *store.ConfigStore, ps *store.PlayerStore, stop <-chan struct{}) {
	ticker := time.NewTicker(60 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-stop:
			return
		case <-ticker.C:
			cc := cs.GetConnectionConfig()
			if cc == nil || cc.PalHookURL == "" {
				continue
			}
			req, _ := http.NewRequest(http.MethodGet, cc.PalHookURL+"/players", nil)
			req.SetBasicAuth("admin", cc.PalHookPassword)
			client := &http.Client{Timeout: 30 * time.Second}
			resp, err := client.Do(req)
			if err != nil {
				continue
			}
			body, _ := io.ReadAll(resp.Body)
			resp.Body.Close()
			var pr struct {
				Players []map[string]any `json:"players"`
			}
			if json.Unmarshal(body, &pr) != nil {
				continue
			}
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
				_ = ps.UpsertPlayerSnapshot(uid, name, lvl)
			}
		}
	}
}
