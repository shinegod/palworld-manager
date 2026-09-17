package model

import "time"

type Backup struct {
	ID        int64     `json:"id"`
	Filename  string    `json:"filename"`
	SizeBytes int64     `json:"size_bytes"`
	SaveDir   string    `json:"save_dir,omitempty"`
	Notes     string    `json:"notes,omitempty"`
	CreatedAt time.Time `json:"created_at"`
}
