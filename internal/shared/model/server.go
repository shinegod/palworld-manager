package model

import "time"

type ServerStatus struct {
	State       string  `json:"state"`
	Version     string  `json:"version,omitempty"`
	Name        string  `json:"name,omitempty"`
	Description string  `json:"description,omitempty"`
	WorldGUID   string  `json:"world_guid,omitempty"`
	PID         int     `json:"pid,omitempty"`
	Uptime      int64   `json:"uptime,omitempty"`
	FPS         int     `json:"fps,omitempty"`
	FrameTime   float64 `json:"frame_time,omitempty"`
	PlayerCount int     `json:"player_count,omitempty"`
	MaxPlayers  int     `json:"max_players,omitempty"`
	InGameDays  int     `json:"in_game_days,omitempty"`
}

type BroadcastRequest struct {
	Message string `json:"message" binding:"required"`
}

type ShutdownRequest struct {
	WaitTime int    `json:"waittime" binding:"required,min=0"`
	Message  string `json:"message"`
}

type SystemMetrics struct {
	CPUPercent   float64 `json:"cpu_percent"`
	MemTotal     uint64  `json:"mem_total"`
	MemUsed      uint64  `json:"mem_used"`
	MemPercent   float64 `json:"mem_percent"`
	DiskTotal    uint64  `json:"disk_total"`
	DiskUsed     uint64  `json:"disk_used"`
	DiskPercent  float64 `json:"disk_percent"`
	PalServerPID int     `json:"palserver_pid"`
	PalServerMem uint64  `json:"palserver_mem"`
	PalServerCPU float64 `json:"palserver_cpu"`
}

type ProcessStatus struct {
	Running bool   `json:"running"`
	PID     int    `json:"pid,omitempty"`
	Uptime  int64  `json:"uptime,omitempty"`
	State   string `json:"state"`
}

type MetricsSnapshot struct {
	Timestamp    time.Time `json:"timestamp"`
	ServerFPS    int       `json:"server_fps"`
	FrameTime    float64   `json:"frame_time"`
	PlayerCount  int       `json:"player_count"`
	MaxPlayers   int       `json:"max_players"`
	Uptime       int64     `json:"uptime"`
	InGameDays   int       `json:"in_game_days"`
	CPUPercent   float64   `json:"cpu_percent"`
	MemPercent   float64   `json:"mem_percent"`
	DiskPercent  float64   `json:"disk_percent"`
	PalServerCPU float64   `json:"palserver_cpu"`
	PalServerMem uint64    `json:"palserver_mem"`
}
