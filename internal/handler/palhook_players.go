package handler

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"time"

	"strconv"

	"github.com/gin-gonic/gin"
	"github.com/shinegod/palworld-manager/internal/shared/model"
	"github.com/shinegod/palworld-manager/internal/store"
)

// PalHookPlayersHandler — hook-only模式下用PalHook替代官方REST的玩家管理
type PalHookPlayersHandler struct {
	configStore *store.ConfigStore
	banStore    *store.BanStore
	playerStore *store.PlayerStore
	client      *http.Client
}

func NewPalHookPlayersHandler(cs *store.ConfigStore, bans *store.BanStore, players *store.PlayerStore) *PalHookPlayersHandler {
	return &PalHookPlayersHandler{configStore: cs, banStore: bans, playerStore: players, client: &http.Client{Timeout: 90 * time.Second}}
}

type hookPlayer struct {
	Name        string  `json:"name"`
	Uid         string  `json:"uid"`
	Level       int     `json:"level"`
	Exp         int64   `json:"exp"`
	X           float64 `json:"x"`
	Y           float64 `json:"y"`
	Z           float64 `json:"z"`
	IP          string  `json:"ip"`
	Platform    string  `json:"platform"`
	Ping        float64 `json:"ping"`
	Character   string  `json:"character"`
	Playerstate string  `json:"playerstate"`
}

func (h *PalHookPlayersHandler) baseURL() (string, bool) {
	cc := h.configStore.GetConnectionConfig()
	if cc == nil || cc.PalHookURL == "" {
		return "", false
	}
	return cc.PalHookURL, true
}

func (h *PalHookPlayersHandler) hookDo(method, path string, body []byte) (*http.Response, error) {
	base, ok := h.baseURL()
	if !ok {
		return nil, http.ErrNotSupported
	}
	req, err := http.NewRequest(method, base+path, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	cc := h.configStore.GetConnectionConfig()
	req.SetBasicAuth("admin", cc.PalHookPassword)
	req.Header.Set("Content-Type", "application/json")
	return h.client.Do(req)
}

func (h *PalHookPlayersHandler) fetchHookPlayers() ([]hookPlayer, error) {
	resp, err := h.hookDo("GET", "/players", nil)
	if err != nil {
		return nil, err
	}
	defer func() { _ = resp.Body.Close() }()
	var out struct {
		Players []hookPlayer `json:"players"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return nil, err
	}
	return out.Players, nil
}

func (h *PalHookPlayersHandler) kickByName(name string) error {
	body, _ := json.Marshal(map[string]string{"name": name})
	resp, err := h.hookDo("POST", "/kick", body)
	if err != nil {
		return err
	}
	defer func() { _ = resp.Body.Close() }()
	_, _ = io.Copy(io.Discard, resp.Body)
	return nil
}

type onlinePlayerOut struct {
	Name        string  `json:"name"`
	AccountName string  `json:"accountName"`
	PlayerID    string  `json:"playerId"`
	UserID      string  `json:"userId"`
	IP          string  `json:"iP"`
	Platform    string  `json:"platform"`
	Ping        float64 `json:"ping"`
	LocationX   float64 `json:"location_x"`
	LocationY   float64 `json:"location_y"`
	LocationZ   float64 `json:"location_z"`
	Level       int     `json:"level"`
}

// GetPlayerHistory 本地历史库 (由后台PalHook轮询快照填充)
func (h *PalHookPlayersHandler) GetPlayerHistory(c *gin.Context) {
	limit := 50
	offset := 0
	if v, err := strconv.Atoi(c.DefaultQuery("limit", "50")); err == nil && v > 0 && v <= 500 {
		limit = v
	}
	if v, err := strconv.Atoi(c.DefaultQuery("offset", "0")); err == nil && v >= 0 {
		offset = v
	}
	players, err := h.playerStore.GetHistory(limit, offset)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	if players == nil {
		players = []model.Player{}
	}
	c.JSON(http.StatusOK, players)
}

func (h *PalHookPlayersHandler) GetOnlinePlayers(c *gin.Context) {
	players, err := h.fetchHookPlayers()
	if err != nil {
		c.JSON(http.StatusBadGateway, gin.H{"error": "PalHook玩家列表失败: " + err.Error()})
		return
	}
	out := make([]onlinePlayerOut, 0, len(players))
	for _, p := range players {
		out = append(out, onlinePlayerOut{
			Name: p.Name, AccountName: p.Name, PlayerID: p.Uid, UserID: p.Uid,
			IP: p.IP, Platform: p.Platform,
			Ping: p.Ping, LocationX: p.X, LocationY: p.Y, LocationZ: p.Z, Level: p.Level,
		})
	}
	c.JSON(http.StatusOK, out)
}

// playerActionReq 踢出/封禁共用入参。PalHook 只能按名字操作, 前端主要传 userid,
// 所以 userid 需要回查在线列表换成名字。
type playerActionReq struct {
	UserID  string `json:"userid"`
	Name    string `json:"name"`
	Message string `json:"message"`
}

// resolveName 补齐名字: 前端只给 userid 时, 从 PalHook 在线列表反查
func (h *PalHookPlayersHandler) resolveName(req *playerActionReq) {
	if req.Name != "" || req.UserID == "" {
		return
	}
	players, err := h.fetchHookPlayers()
	if err != nil {
		return
	}
	for _, p := range players {
		if p.Uid == req.UserID {
			req.Name = p.Name
			return
		}
	}
}

// operator 取当前登录管理员名, 避免裸类型断言 panic
func operator(c *gin.Context) string {
	if v, ok := c.Get("username"); ok {
		if s, ok := v.(string); ok && s != "" {
			return s
		}
	}
	return "unknown"
}

func (h *PalHookPlayersHandler) KickPlayer(c *gin.Context) {
	var req playerActionReq
	_ = c.ShouldBindJSON(&req)
	if req.Name == "" && req.UserID == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "name or userid is required"})
		return
	}
	h.resolveName(&req)
	if req.Name == "" {
		c.JSON(http.StatusNotFound, gin.H{"error": "玩家不在线或未找到, 无法踢出"})
		return
	}
	if err := h.kickByName(req.Name); err != nil {
		c.JSON(http.StatusBadGateway, gin.H{"error": "踢出失败: " + err.Error()})
		return
	}
	c.JSON(http.StatusOK, gin.H{"status": "ok"})
}

func (h *PalHookPlayersHandler) BanPlayer(c *gin.Context) {
	var req playerActionReq
	_ = c.ShouldBindJSON(&req)
	if req.Name == "" && req.UserID == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "name or userid is required"})
		return
	}
	h.resolveName(&req)

	// 在线就踢出; 离线玩家只记封禁 (不算失败)
	kicked := false
	if req.Name != "" {
		kicked = h.kickByName(req.Name) == nil
	}
	if h.banStore != nil {
		_ = h.banStore.Insert(req.UserID, req.Name, req.Message, operator(c))
	}
	// 历史库同步标记, 让玩家列表能看出封禁状态
	if h.playerStore != nil && req.UserID != "" {
		_ = h.playerStore.MarkBanned(req.UserID, req.Message)
	}
	c.JSON(http.StatusOK, gin.H{"status": "ok", "kicked": kicked})
}

func (h *PalHookPlayersHandler) GetBans(c *gin.Context) {
	bans, err := h.banStore.GetBans(50, 0)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, bans)
}

func (h *PalHookPlayersHandler) UnbanPlayer(c *gin.Context) {
	var req struct {
		UserID string `json:"userid"`
	}
	_ = c.ShouldBindJSON(&req)
	if req.UserID == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "userid is required"})
		return
	}
	if h.banStore != nil {
		_ = h.banStore.MarkUnbanned(req.UserID)
	}
	if h.playerStore != nil {
		_ = h.playerStore.MarkUnbanned(req.UserID)
	}
	c.JSON(http.StatusOK, gin.H{"status": "ok"})
}