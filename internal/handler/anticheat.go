package handler

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/shinegod/palworld-manager/internal/store"
)

// 反作弊阈值 (与 PalHook 侧的实际上限对齐)
const (
	MaxPlayerLevel = 80     // 1.0版本玩家等级上限
	MaxPlayerExp   = int64(100_000_000) // 80级经验曲线约18M, 上限留足余量
)

// PlayerSnapshot 反作弊/告警用的玩家快照 (来自 PalHook /players)
type PlayerSnapshot struct {
	UID      string
	Name     string
	IP       string
	Platform string
	Level    int
	Exp      int64
	Ping     float64
}

type AnticheatHandler struct {
	store  *store.AnticheatStore
	alerts *store.AlertStore
	cs     *store.ConfigStore
	client *http.Client
}

func NewAnticheatHandler(db *sql.DB, cs *store.ConfigStore) *AnticheatHandler {
	return &AnticheatHandler{
		store:  store.NewAnticheatStore(db),
		alerts: store.NewAlertStore(db),
		cs:     cs,
		client: &http.Client{Timeout: 15 * time.Second},
	}
}

// RunChecks 对玩家快照跑检测规则, 生成 flags 和 alerts。返回新产生的标记数。
func (h *AnticheatHandler) RunChecks(scanType string, players []PlayerSnapshot) (int, error) {
	scanID, err := h.store.CreateScan(scanType)
	if err != nil {
		return 0, err
	}
	n := 0

	// 同IP多开: 同一IP同时有2个以上不同UID在线
	multi := map[string]bool{}
	ipUIDs := map[string][]string{}
	for _, p := range players {
		if p.IP != "" && p.IP != "0.0.0.0" {
			ipUIDs[p.IP] = append(ipUIDs[p.IP], p.UID)
		}
	}
	for _, uids := range ipUIDs {
		if len(uids) > 1 {
			for _, uid := range uids {
				multi[uid] = true
			}
		}
	}

	for _, p := range players {
		// 等级超上限 (客户端改档典型特征)
		if p.Level > MaxPlayerLevel {
			if dup, _ := h.store.HasUnresolved(p.UID, "level_overflow"); !dup {
				detail := fmt.Sprintf("等级 %d 超过上限 %d (存档篡改嫌疑)", p.Level, MaxPlayerLevel)
				_ = h.store.AddFlag(scanID, p.UID, "level_overflow", "high", detail)
				_ = h.alerts.AddAlert("error", "anticheat", fmt.Sprintf("玩家 %s 等级异常: %d", p.Name, p.Level), detail)
				n++
			}
		}
		// 经验超上限
		if p.Exp > MaxPlayerExp {
			if dup, _ := h.store.HasUnresolved(p.UID, "exp_overflow"); !dup {
				detail := fmt.Sprintf("经验 %d 超过合理上限 (改档嫌疑)", p.Exp)
				_ = h.store.AddFlag(scanID, p.UID, "exp_overflow", "medium", detail)
				_ = h.alerts.AddAlert("warning", "anticheat", fmt.Sprintf("玩家 %s 经验异常: %d", p.Name, p.Exp), detail)
				n++
			}
		}
		// 同IP多账号
		if multi[p.UID] {
			if dup, _ := h.store.HasUnresolved(p.UID, "same_ip_multi"); !dup {
				detail := "同一IP同时存在多个账号在线 (多开/共用IP嫌疑)"
				_ = h.store.AddFlag(scanID, p.UID, "same_ip_multi", "medium", detail)
				_ = h.alerts.AddAlert("warning", "anticheat", fmt.Sprintf("玩家 %s 同IP多账号在线 (%s)", p.Name, p.IP), detail)
				n++
			}
		}
	}

	summary := "无异常"
	if n > 0 {
		summary = fmt.Sprintf("发现 %d 项异常", n)
	}
	_ = h.store.FinishScan(scanID, summary)
	return n, nil
}

// fetchHookPlayers 拉取 PalHook /players 并转成快照列表
func (h *AnticheatHandler) fetchHookPlayers() ([]PlayerSnapshot, error) {
	cc := h.cs.GetConnectionConfig()
	if cc == nil || cc.PalHookURL == "" {
		return nil, fmt.Errorf("PalHook未配置")
	}
	req, err := http.NewRequest(http.MethodGet, cc.PalHookURL+"/players", nil)
	if err != nil {
		return nil, err
	}
	req.SetBasicAuth("admin", cc.PalHookPassword)
	resp, err := h.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("PalHook /players 状态码 %d", resp.StatusCode)
	}
	var pr struct {
		Players []map[string]any `json:"players"`
	}
	if err := json.Unmarshal(body, &pr); err != nil {
		return nil, err
	}
	out := make([]PlayerSnapshot, 0, len(pr.Players))
	for _, p := range pr.Players {
		snap := PlayerSnapshot{}
		snap.UID, _ = p["uid"].(string)
		snap.Name, _ = p["name"].(string)
		snap.IP, _ = p["ip"].(string)
		snap.Platform, _ = p["platform"].(string)
		if v, ok := p["level"].(float64); ok {
			snap.Level = int(v)
		}
		if v, ok := p["exp"].(float64); ok {
			snap.Exp = int64(v)
		}
		if v, ok := p["ping"].(float64); ok {
			snap.Ping = v
		}
		out = append(out, snap)
	}
	return out, nil
}

// GET /anticheat/flags 检测日志
func (h *AnticheatHandler) GetFlags(c *gin.Context) {
	limit := 100
	if v, err := strconv.Atoi(c.DefaultQuery("limit", "100")); err == nil && v > 0 && v <= 500 {
		limit = v
	}
	flags, err := h.store.ListFlags(limit)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	if flags == nil {
		flags = []store.AnticheatFlag{}
	}
	c.JSON(http.StatusOK, gin.H{"flags": flags, "total_scans": h.store.ScanCount()})
}

// POST /anticheat/scan 手动扫描
func (h *AnticheatHandler) Scan(c *gin.Context) {
	players, err := h.fetchHookPlayers()
	if err != nil {
		c.JSON(http.StatusBadGateway, gin.H{"error": "拉取PalHook玩家失败: " + err.Error()})
		return
	}
	n, err := h.RunChecks("manual", players)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, gin.H{"status": "ok", "players_checked": len(players), "new_flags": n})
}

// POST /anticheat/flags/:id/resolve 标记处理 (body 可选 {"action":"banned"})
func (h *AnticheatHandler) ResolveFlag(c *gin.Context) {
	id, err := strconv.ParseInt(c.Param("id"), 10, 64)
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid id"})
		return
	}
	action := "resolved"
	var req struct {
		Action string `json:"action"`
	}
	_ = c.ShouldBindJSON(&req)
	if req.Action != "" {
		action = req.Action
	}
	if err := h.store.ResolveFlag(id, action); err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, gin.H{"status": "ok"})
}
