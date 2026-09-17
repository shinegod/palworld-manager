package handler

import (
	"database/sql"
	"encoding/json"
	"net/http"
	"strconv"
	"time"

	"github.com/gin-gonic/gin"
)

type EventHandler struct {
	db *sql.DB
}

type EventRow struct {
	ID            int64     `json:"id"`
	Name          string    `json:"name"`
	Description   string    `json:"description"`
	Enabled       bool      `json:"enabled"`
	ScheduleType  string    `json:"schedule_type"`
	ScheduleValue string    `json:"schedule_value"`
	Actions       string    `json:"actions"`
	Recipients    string    `json:"recipients"`
	OnEndActions  string    `json:"on_end_actions"`
	LastRun       *string   `json:"last_run"`
	CreatedAt     time.Time `json:"created_at"`
}

type EventCreateReq struct {
	Name          string `json:"name" binding:"required"`
	Description   string `json:"description"`
	ScheduleType  string `json:"schedule_type" binding:"required"`
	ScheduleValue string `json:"schedule_value" binding:"required"`
	Actions       string `json:"actions" binding:"required"`
	Recipients    string `json:"recipients"`
	OnEndActions  string `json:"on_end_actions"`
}

func NewEventHandler(db *sql.DB) *EventHandler {
	return &EventHandler{db: db}
}

func (h *EventHandler) List(c *gin.Context) {
	rows, err := h.db.Query(`SELECT id, name, description, enabled, schedule_type, schedule_value, actions, recipients, on_end_actions, last_run, created_at FROM events ORDER BY id DESC`)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	defer rows.Close()

	events := make([]EventRow, 0)
	for rows.Next() {
		var e EventRow
		var desc, recipients, onEnd sql.NullString
		var lastRun sql.NullString
		err := rows.Scan(&e.ID, &e.Name, &desc, &e.Enabled, &e.ScheduleType, &e.ScheduleValue, &e.Actions, &recipients, &onEnd, &lastRun, &e.CreatedAt)
		if err != nil {
			continue
		}
		e.Description = desc.String
		e.Recipients = recipients.String
		e.OnEndActions = onEnd.String
		if lastRun.Valid {
			e.LastRun = &lastRun.String
		}
		events = append(events, e)
	}
	c.JSON(http.StatusOK, events)
}

func (h *EventHandler) Create(c *gin.Context) {
	var req EventCreateReq
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
		return
	}

	var actionsCheck json.RawMessage
	if err := json.Unmarshal([]byte(req.Actions), &actionsCheck); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "actions must be valid JSON"})
		return
	}

	result, err := h.db.Exec(`INSERT INTO events (name, description, enabled, schedule_type, schedule_value, actions, recipients, on_end_actions, created_at) VALUES (?, ?, 1, ?, ?, ?, ?, ?, ?)`,
		req.Name, req.Description, req.ScheduleType, req.ScheduleValue, req.Actions, req.Recipients, req.OnEndActions, time.Now())
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}

	id, _ := result.LastInsertId()
	c.JSON(http.StatusOK, gin.H{"id": id, "message": "event created"})
}

func (h *EventHandler) Update(c *gin.Context) {
	id, err := strconv.ParseInt(c.Param("id"), 10, 64)
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid id"})
		return
	}

	var req EventCreateReq
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
		return
	}

	_, err = h.db.Exec(`UPDATE events SET name=?, description=?, schedule_type=?, schedule_value=?, actions=?, recipients=?, on_end_actions=? WHERE id=?`,
		req.Name, req.Description, req.ScheduleType, req.ScheduleValue, req.Actions, req.Recipients, req.OnEndActions, id)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, gin.H{"message": "event updated"})
}

func (h *EventHandler) Delete(c *gin.Context) {
	id, err := strconv.ParseInt(c.Param("id"), 10, 64)
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid id"})
		return
	}

	_, err = h.db.Exec(`DELETE FROM events WHERE id=?`, id)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	c.JSON(http.StatusOK, gin.H{"message": "event deleted"})
}

func (h *EventHandler) Trigger(c *gin.Context) {
	id, err := strconv.ParseInt(c.Param("id"), 10, 64)
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid id"})
		return
	}

	var actions string
	err = h.db.QueryRow(`SELECT actions FROM events WHERE id=?`, id).Scan(&actions)
	if err != nil {
		c.JSON(http.StatusNotFound, gin.H{"error": "event not found"})
		return
	}

	h.db.Exec(`UPDATE events SET last_run=? WHERE id=?`, time.Now().Format(time.RFC3339), id)

	c.JSON(http.StatusOK, gin.H{"message": "event triggered", "actions": json.RawMessage(actions)})
}
