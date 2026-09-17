package model

import "time"

type Player struct {
	UID                  string    `json:"uid"`
	SteamID              string    `json:"steam_id,omitempty"`
	Name                 string    `json:"name"`
	AccountName          string    `json:"account_name,omitempty"`
	Level                int       `json:"level"`
	IP                   string    `json:"ip,omitempty"`
	Ping                 float64   `json:"ping,omitempty"`
	LocationX            float64   `json:"location_x,omitempty"`
	LocationY            float64   `json:"location_y,omitempty"`
	Online               bool      `json:"online"`
	FirstSeen            time.Time `json:"first_seen"`
	LastSeen             time.Time `json:"last_seen"`
	TotalPlaytimeSeconds int64     `json:"total_playtime_seconds"`
	IsBanned             bool      `json:"is_banned"`
	BanReason            string    `json:"ban_reason,omitempty"`
	Platform             string    `json:"platform,omitempty"`
	BuildingCount        int       `json:"building_count,omitempty"`
}

type PlayerDetail struct {
	Player
	Inventory []InventoryItem `json:"inventory"`
	Pals      []OwnedPal      `json:"pals"`
	TechTree  []string        `json:"tech_tree,omitempty"`
	Stats     PlayerStats     `json:"stats"`
}

type PlayerStats struct {
	HP        int `json:"hp"`
	MaxHP     int `json:"max_hp"`
	Attack    int `json:"attack"`
	Defense   int `json:"defense"`
	Stamina   int `json:"stamina"`
	Weight    int `json:"weight"`
	CraftSpeed int `json:"craft_speed"`
}

type PlayerSession struct {
	ID              int64     `json:"id"`
	PlayerUID       string    `json:"player_uid"`
	JoinTime        time.Time `json:"join_time"`
	LeaveTime       time.Time `json:"leave_time,omitempty"`
	DurationSeconds int64     `json:"duration_seconds,omitempty"`
}
