package handler

import (
	"database/sql"
	"net/http"
	"strconv"

	"github.com/gin-gonic/gin"
	"github.com/shinegod/palworld-manager/internal/store"
)

// AlertHandler 监控中心告警 (数据来源: 反作弊检测 + 服务器状态轮询)
type AlertHandler struct {
	store *store.AlertStore
}

func NewAlertHandler(db *sql.DB) *AlertHandler {
	return &AlertHandler{store: store.NewAlertStore(db)}
}

func (h *AlertHandler) GetAlerts(c *gin.Context) {
	limit := 100
	if v, err := strconv.Atoi(c.DefaultQuery("limit", "100")); err == nil && v > 0 && v <= 500 {
		limit = v
	}
	alerts, err := h.store.ListAlerts(limit)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	if alerts == nil {
		alerts = []store.Alert{}
	}
	c.JSON(http.StatusOK, alerts)
}

func (h *AlertHandler) AckAlert(c *gin.Context) {
	var req struct {
		ID int64 `json:"id"`
	}
	if err := c.ShouldBindJSON(&req); err != nil || req.ID <= 0 {
		c.JSON(http.StatusBadRequest, gin.H{"error": "id is required"})
		return
	}
	if err := h.store.AckAlert(req.ID); err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, gin.H{"status": "ok"})
}

func (h *AlertHandler) ClearAlerts(c *gin.Context) {
	if err := h.store.ClearAlerts(); err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, gin.H{"status": "ok"})
}
