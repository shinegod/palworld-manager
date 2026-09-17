package model

type InventoryItem struct {
	ItemID   string `json:"item_id"`
	Name     string `json:"name,omitempty"`
	Count    int    `json:"count"`
	SlotIndex int   `json:"slot_index"`
}

type GiveItemRequest struct {
	ItemID string `json:"item_id" binding:"required"`
	Count  int    `json:"count" binding:"required,min=1"`
	Slot   int    `json:"slot"`
}
