package handler

import (
	"bytes"
	"io"
	"net/http"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/shinegod/palworld-manager/internal/store"
)

// PalHookHandler 代理 PalHook 注入库的 HTTP API (刷道具/经验/帕鲁/传送等)
type PalHookHandler struct {
	configStore *store.ConfigStore
	client      *http.Client
}

func NewPalHookHandler(cs *store.ConfigStore) *PalHookHandler {
	return &PalHookHandler{
		configStore: cs,
		client:      &http.Client{Timeout: 90 * time.Second},
	}
}

func (h *PalHookHandler) palhookConfig() (string, string, bool) {
	cc := h.configStore.GetConnectionConfig()
	if cc == nil || cc.PalHookURL == "" {
		return "", "", false
	}
	return cc.PalHookURL, cc.PalHookPassword, true
}

func (h *PalHookHandler) proxy(c *gin.Context, method, path string) {
	baseURL, password, ok := h.palhookConfig()
	if !ok {
		c.JSON(http.StatusServiceUnavailable, gin.H{"error": "PalHook未配置，请在 设置→连接配置 中填写 PalHook 地址"})
		return
	}

	body, err := io.ReadAll(c.Request.Body)
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
		return
	}

	req, err := http.NewRequest(method, baseURL+path, bytes.NewReader(body))
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	req.SetBasicAuth("admin", password)
	req.Header.Set("Content-Type", "application/json")

	resp, err := h.client.Do(req)
	if err != nil {
		c.JSON(http.StatusBadGateway, gin.H{"error": "PalHook连接失败: " + err.Error()})
		return
	}
	defer resp.Body.Close()
	respBody, _ := io.ReadAll(resp.Body)
	c.Data(resp.StatusCode, "application/json", respBody)
}

func (h *PalHookHandler) Health(c *gin.Context)        { h.proxy(c, http.MethodGet, "/health") }
func (h *PalHookHandler) GiveItem(c *gin.Context)      { h.proxy(c, http.MethodPost, "/give-item") }
func (h *PalHookHandler) GiveExp(c *gin.Context)       { h.proxy(c, http.MethodPost, "/give-exp") }
func (h *PalHookHandler) GiveMoney(c *gin.Context)     { h.proxy(c, http.MethodPost, "/give-money") }
func (h *PalHookHandler) SpawnPal(c *gin.Context)      { h.proxy(c, http.MethodPost, "/spawn-pal") }
func (h *PalHookHandler) Teleport(c *gin.Context)      { h.proxy(c, http.MethodPost, "/teleport") }
func (h *PalHookHandler) SetTechPoints(c *gin.Context) { h.proxy(c, http.MethodPost, "/set-tech-points") }
func (h *PalHookHandler) SetLevel(c *gin.Context)     { h.proxy(c, http.MethodPost, "/set-level") }
func (h *PalHookHandler) Players(c *gin.Context)      { h.proxy(c, http.MethodGet, "/players") }
func (h *PalHookHandler) Announce(c *gin.Context)     { h.proxy(c, http.MethodPost, "/announce") }
func (h *PalHookHandler) Kick(c *gin.Context)         { h.proxy(c, http.MethodPost, "/kick") }
func (h *PalHookHandler) Metrics(c *gin.Context)      { h.proxy(c, http.MethodGet, "/metrics") }
func (h *PalHookHandler) Chat(c *gin.Context)        { h.proxy(c, http.MethodPost, "/chat") }
func (h *PalHookHandler) Guilds(c *gin.Context)     { h.proxy(c, http.MethodGet, "/guilds") }
