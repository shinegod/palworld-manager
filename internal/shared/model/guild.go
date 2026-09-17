package model

type Guild struct {
	ID          string        `json:"id"`
	Name        string        `json:"name"`
	Level       int           `json:"level"`
	MemberCount int           `json:"member_count"`
	Members     []GuildMember `json:"members,omitempty"`
	Bases       []GuildBase   `json:"bases,omitempty"`
}

type GuildMember struct {
	PlayerUID string `json:"player_uid"`
	Name      string `json:"name"`
	Role      string `json:"role"`
	Level     int    `json:"level"`
	Online    bool   `json:"online"`
}

type GuildBase struct {
	ID        string  `json:"id"`
	Name      string  `json:"name,omitempty"`
	LocationX float64 `json:"location_x"`
	LocationY float64 `json:"location_y"`
	LocationZ float64 `json:"location_z"`
	Level     int     `json:"level"`
	PalCount  int     `json:"pal_count"`
}
