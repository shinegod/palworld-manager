package model

import "time"

type Event struct {
	ID            int64         `json:"id"`
	Name          string        `json:"name" binding:"required"`
	Description   string        `json:"description"`
	Enabled       bool          `json:"enabled"`
	ScheduleType  string        `json:"schedule_type" binding:"required,oneof=once recurring cron"`
	ScheduleValue string        `json:"schedule_value" binding:"required"`
	NextRun       *time.Time    `json:"next_run,omitempty"`
	LastRun       *time.Time    `json:"last_run,omitempty"`
	IsTemplate    bool          `json:"is_template"`
	Actions       []EventAction `json:"actions,omitempty"`
	CleanupActions []EventCleanup `json:"cleanup_actions,omitempty"`
	CreatedAt     time.Time     `json:"created_at"`
}

type EventAction struct {
	ID          int64  `json:"id"`
	EventID     int64  `json:"event_id"`
	ActionOrder int    `json:"action_order"`
	ActionType  string `json:"action_type" binding:"required"`
	ActionParams string `json:"action_params" binding:"required"`
	TargetType  string `json:"target_type"`
	TargetValue string `json:"target_value,omitempty"`
}

type EventCleanup struct {
	ID           int64  `json:"id"`
	EventID      int64  `json:"event_id"`
	ActionType   string `json:"action_type"`
	ActionParams string `json:"action_params,omitempty"`
}
