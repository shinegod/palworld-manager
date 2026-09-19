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
	defer resp.Body.Close()
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
	defer resp.Body.Close()
	io.Copy(io.Discard, resp.Body)
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

func (h *PalHookPlayersHandler) KickPlayer(c *gin.Context) {
	var req struct {
		Name string `json:"name"`
	}
	_ = c.ShouldBindJSON(&req)
	if req.Name == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "name is required"})
		return
	}
	if err := h.kickByName(req.Name); err != nil {
		c.JSON(http.StatusBadGateway, gin.H{"error": "踢出失败: " + err.Error()})
		return
	}
	c.JSON(http.StatusOK, gin.H{"status": "ok"})
}

func (h *PalHookPlayersHandler) BanPlayer(c *gin.Context) {
	var req struct {
		UserID  string `json:"userid"`
		Name    string `json:"name"`
		Message string `json:"message"`
	}
	_ = c.ShouldBindJSON(&req)
	if req.Name == "" && req.UserID == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "name or userid is required"})
		return
	}
	// 踢出 + 本地封禁记录
	if req.Name != "" {
		_ = h.kickByName(req.Name)
	}
	username, _ := c.Get("username")
	if h.banStore != nil {
		_ = h.banStore.Insert(req.UserID, req.Name, req.Message, username.(string))
	}
	c.JSON(http.StatusOK, gin.H{"status": "ok"})
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
	c.JSON(http.StatusOK, gin.H{"status": "ok"})
}