package model

import "time"

type Kit struct {
	ID          int64     `json:"id"`
	Name        string    `json:"name" binding:"required"`
	Description string    `json:"description"`
	IsStarterKit bool     `json:"is_starter_kit"`
	Items       []KitItem `json:"items,omitempty"`
	CreatedAt   time.Time `json:"created_at"`
	UpdatedAt   time.Time `json:"updated_at"`
}

type KitItem struct {
	ID       int64  `json:"id"`
	KitID    int64  `json:"kit_id"`
	ItemType string `json:"item_type" binding:"required,oneof=item pal"`
	ItemID   string `json:"item_id" binding:"required"`
	Count    int    `json:"count"`
	Metadata string `json:"metadata,omitempty"`
}
