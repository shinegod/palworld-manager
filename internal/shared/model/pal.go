package model

type Pal struct {
	InstanceID string   `json:"instance_id"`
	PalID      string   `json:"pal_id"`
	NickName   string   `json:"nickname,omitempty"`
	Level      int      `json:"level"`
	HP         int      `json:"hp"`
	MaxHP      int      `json:"max_hp"`
	OwnerUID   string   `json:"owner_uid,omitempty"`
	GuildID    string   `json:"guild_id,omitempty"`
	UnitType   string   `json:"unit_type"`
	LocationX  float64  `json:"location_x"`
	LocationY  float64  `json:"location_y"`
	LocationZ  float64  `json:"location_z"`
	IsActive   bool     `json:"is_active"`
	Passives   []string `json:"passives,omitempty"`
	IVs        PalIVs   `json:"ivs,omitempty"`
}

type OwnedPal struct {
	Pal
	Attack     int `json:"attack"`
	Defense    int `json:"defense"`
	WorkSpeed  int `json:"work_speed"`
}

type PalIVs struct {
	HP      int `json:"hp"`
	Attack  int `json:"attack"`
	Defense int `json:"defense"`
}

type GivePalRequest struct {
	PalID    string   `json:"pal_id" binding:"required"`
	Level    int      `json:"level" binding:"required,min=1,max=55"`
	IVs      PalIVs   `json:"ivs"`
	Passives []string `json:"passives"`
}
