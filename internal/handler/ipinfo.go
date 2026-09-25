package handler

import (
	"encoding/json"
	"io"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/gin-gonic/gin"
)

// IPInfoHandler — IP归属地查询 (后端代理 ip-api.com, 内存缓存30分钟, 避免前端跨域/混合内容问题)
type IPInfoHandler struct {
	mu      sync.Mutex
	cache   map[string]cachedIPInfo
	client  *http.Client
}

type cachedIPInfo struct {
	data json.RawMessage
	ts   time.Time
}

func NewIPInfoHandler() *IPInfoHandler {
	return &IPInfoHandler{
		cache:  make(map[string]cachedIPInfo),
		client: &http.Client{Timeout: 6 * time.Second},
	}
}

func (h *IPInfoHandler) Get(c *gin.Context) {
	ip := c.Query("ip")
	if ip == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "ip parameter required"})
		return
	}
	parsed := net.ParseIP(ip)
	if parsed == nil || parsed.IsPrivate() || parsed.IsLoopback() || parsed.IsUnspecified() {
		c.JSON(http.StatusOK, gin.H{"query": ip, "private": true})
		return
	}

	h.mu.Lock()
	if v, ok := h.cache[ip]; ok && time.Since(v.ts) < 30*time.Minute {
		h.mu.Unlock()
		c.Data(http.StatusOK, "application/json", v.data)
		return
	}
	h.mu.Unlock()

	url := "http://ip-api.com/json/" + ip + "?lang=zh-CN&fields=status,country,countryCode,regionName,city,isp,query"
	req, _ := http.NewRequest(http.MethodGet, url, nil)
	req.Header.Set("User-Agent", "palmanager/1.0")
	resp, err := h.client.Do(req)
	if err != nil {
		c.JSON(http.StatusBadGateway, gin.H{"error": "ip-api query failed"})
		return
	}
	defer func() { _ = resp.Body.Close() }()
	body, _ := io.ReadAll(resp.Body)

	h.mu.Lock()
	h.cache[ip] = cachedIPInfo{data: append(json.RawMessage(nil), body...), ts: time.Now()}
	h.mu.Unlock()

	c.Data(http.StatusOK, "application/json", body)
}
