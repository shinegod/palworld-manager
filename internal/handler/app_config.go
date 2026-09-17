package handler

import (
	"net/http"

	"github.com/gin-gonic/gin"
	"github.com/shinegod/palworld-manager/internal/store"
)

type AppConfigHandler struct {
	configStore *store.ConfigStore
}

func NewAppConfigHandler(cs *store.ConfigStore) *AppConfigHandler {
	return &AppConfigHandler{configStore: cs}
}

func (h *AppConfigHandler) GetConnectionConfig(c *gin.Context) {
	cc := h.configStore.GetConnectionConfig()
	if cc == nil {
		c.JSON(http.StatusOK, gin.H{"configured": false})
		return
	}
	cc.PalworldPass = maskPassword(cc.PalworldPass)
	cc.RCONPass = maskPassword(cc.RCONPass)
	cc.AdminPass = maskPassword(cc.AdminPass)
	cc.JWTSecret = maskPassword(cc.JWTSecret)
	cc.PalHookPassword = maskPassword(cc.PalHookPassword)
	c.JSON(http.StatusOK, gin.H{"configured": true, "config": cc})
}

func (h *AppConfigHandler) SaveConnectionConfig(c *gin.Context) {
	var cc store.ConnectionConfig
	if err := c.ShouldBindJSON(&cc); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
		return
	}

	if cc.PalworldAPIURL == "" && cc.PalHookURL == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "palhook_url 和 palworld_api_url 至少填一个"})
		return
	}
	if cc.AdminUser == "" {
		cc.AdminUser = "admin"
	}
	if cc.AdminPass == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "admin_pass is required"})
		return
	}
	if cc.JWTSecret == "" {
		cc.JWTSecret = "palmanager-default-secret-change-me"
	}
	if cc.ListenPort == 0 {
		cc.ListenPort = 8080
	}

	existing := h.configStore.GetConnectionConfig()
	if existing != nil {
		if cc.PalworldPass == "****" || cc.PalworldPass == "" {
			cc.PalworldPass = existing.PalworldPass
		}
		if cc.RCONPass == "****" || cc.RCONPass == "" {
			cc.RCONPass = existing.RCONPass
		}
		if cc.AdminPass == "****" {
			cc.AdminPass = existing.AdminPass
		}
		if cc.JWTSecret == "****" {
			cc.JWTSecret = existing.JWTSecret
		}
		if cc.PalHookPassword == "****" || cc.PalHookPassword == "" {
			cc.PalHookPassword = existing.PalHookPassword
		}
	}

	if err := h.configStore.SaveConnectionConfig(&cc); err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}

	c.JSON(http.StatusOK, gin.H{"message": "Configuration saved. Restart PalManager for changes to take effect."})
}

func (h *AppConfigHandler) GetSetupStatus(c *gin.Context) {
	configured := h.configStore.IsConfigured()
	c.JSON(http.StatusOK, gin.H{"configured": configured})
}

func maskPassword(s string) string {
	if s == "" {
		return ""
	}
	return "****"
}
