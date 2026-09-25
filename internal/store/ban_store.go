package store

import (
	"database/sql"
	"time"
)

type Ban struct {
	ID int64 `json:"id"`
	// user_id: 与前端 BanRecord 字段对齐 (Players.vue 的用户ID列/解封按钮都读这个)
	PlayerUID  string     `json:"user_id"`
	PlayerName string     `json:"player_name"`
	Reason     string     `json:"reason"`
	BannedBy   string     `json:"banned_by"`
	BannedAt   time.Time  `json:"banned_at"`
	UnbannedAt *time.Time `json:"unbanned_at,omitempty"`
}

type BanStore struct {
	db *sql.DB
}

func NewBanStore(db *sql.DB) *BanStore {
	return &BanStore{db: db}
}

func (s *BanStore) Insert(playerUID, playerName, reason, bannedBy string) error {
	_, err := s.db.Exec(`INSERT INTO bans (player_uid, player_name, reason, banned_by) VALUES (?,?,?,?)`,
		playerUID, playerName, reason, bannedBy)
	return err
}

func (s *BanStore) GetBans(limit, offset int) ([]Ban, error) {
	rows, err := s.db.Query(`SELECT id, player_uid, player_name, reason, banned_by, banned_at, unbanned_at FROM bans ORDER BY banned_at DESC LIMIT ? OFFSET ?`, limit, offset)
	if err != nil {
		return nil, err
	}
	defer func() { _ = rows.Close() }()

	var bans []Ban
	for rows.Next() {
		var b Ban
		var name, reason, by sql.NullString
		var unbanned sql.NullTime
		if err := rows.Scan(&b.ID, &b.PlayerUID, &name, &reason, &by, &b.BannedAt, &unbanned); err != nil {
			return nil, err
		}
		b.PlayerName = name.String
		b.Reason = reason.String
		b.BannedBy = by.String
		if unbanned.Valid {
			b.UnbannedAt = &unbanned.Time
		}
		bans = append(bans, b)
	}
	return bans, rows.Err()
}

func (s *BanStore) MarkUnbanned(playerUID string) error {
	_, err := s.db.Exec(`UPDATE bans SET unbanned_at = CURRENT_TIMESTAMP WHERE player_uid = ? AND unbanned_at IS NULL`, playerUID)
	return err
}
