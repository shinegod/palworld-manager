package model

import "time"

type MetricsHistory struct {
	ID           int64     `json:"id"`
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
	PalServerMem int64     `json:"palserver_mem"`
}

type Alert struct {
	ID        int64     `json:"id"`
	Level     string    `json:"level"`
	Category  string    `json:"category"`
	Message   string    `json:"message"`
	Details   string    `json:"details,omitempty"`
	CreatedAt time.Time `json:"created_at"`
}

type ConsoleEntry struct {
	ID         int64     `json:"id"`
	Command    string    `json:"command"`
	Output     string    `json:"output"`
	ExecutedAt time.Time `json:"executed_at"`
}
