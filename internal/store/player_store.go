package store

import (
	"database/sql"
	"time"

	"github.com/shinegod/palworld-manager/internal/shared/model"
)

type PlayerStore struct {
	db *sql.DB
}

func NewPlayerStore(db *sql.DB) *PlayerStore {
	return &PlayerStore{db: db}
}

func (s *PlayerStore) Upsert(p model.Player) error {
	_, err := s.db.Exec(`INSERT INTO player_history (uid, steam_id, name, account_name, level, ip, platform, online, first_seen, last_seen) VALUES (?,?,?,?,?,?,?,?,?,?)
		ON CONFLICT(uid) DO UPDATE SET name=excluded.name, account_name=excluded.account_name, level=excluded.level, ip=excluded.ip, online=excluded.online, last_seen=excluded.last_seen`,
		p.UID, p.SteamID, p.Name, p.AccountName, p.Level, p.IP, p.Platform, p.Online, p.FirstSeen, p.LastSeen,
	)
	return err
}

// UpsertPlayerSnapshot 轻量快照: 仅更新在线玩家名字/等级/在线状态 (PalHook轮询用)
func (s *PlayerStore) UpsertPlayerSnapshot(uid, name string, level int) error {
	_, err := s.db.Exec(`INSERT INTO player_history (uid, steam_id, name, account_name, level, ip, platform, online, first_seen, last_seen) VALUES (?,?,?,?,?,?,?,?,?,?)
		ON CONFLICT(uid) DO UPDATE SET name=excluded.name, level=excluded.level, online=excluded.online, last_seen=excluded.last_seen`,
		uid, "", name, "", level, "", "", true, time.Now(), time.Now(),
	)
	return err
}

func (s *PlayerStore) SetOffline(uid string) error {
	_, err := s.db.Exec(`UPDATE player_history SET online = 0 WHERE uid = ?`, uid)
	return err
}

func (s *PlayerStore) GetHistory(limit, offset int) ([]model.Player, error) {
	rows, err := s.db.Query(`SELECT uid, steam_id, name, account_name, level, ip, platform, online, first_seen, last_seen, total_playtime_seconds, is_banned, ban_reason FROM player_history ORDER BY last_seen DESC LIMIT ? OFFSET ?`, limit, offset)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var players []model.Player
	for rows.Next() {
		var p model.Player
		var steamID, accountName, ip, platform, banReason sql.NullString
		if err := rows.Scan(&p.UID, &steamID, &p.Name, &accountName, &p.Level, &ip, &platform, &p.Online, &p.FirstSeen, &p.LastSeen, &p.TotalPlaytimeSeconds, &p.IsBanned, &banReason); err != nil {
			return nil, err
		}
		p.SteamID = steamID.String
		p.AccountName = accountName.String
		p.IP = ip.String
		p.Platform = platform.String
		p.BanReason = banReason.String
		players = append(players, p)
	}
	return players, rows.Err()
}

func (s *PlayerStore) Get(uid string) (*model.Player, error) {
	var p model.Player
	var steamID, accountName, ip, platform, banReason sql.NullString
	err := s.db.QueryRow(`SELECT uid, steam_id, name, account_name, level, ip, platform, online, first_seen, last_seen, total_playtime_seconds, is_banned, ban_reason FROM player_history WHERE uid = ?`, uid).
		Scan(&p.UID, &steamID, &p.Name, &accountName, &p.Level, &ip, &platform, &p.Online, &p.FirstSeen, &p.LastSeen, &p.TotalPlaytimeSeconds, &p.IsBanned, &banReason)
	if err != nil {
		return nil, err
	}
	p.SteamID = steamID.String
	p.AccountName = accountName.String
	p.IP = ip.String
	p.Platform = platform.String
	p.BanReason = banReason.String
	return &p, nil
}

func (s *PlayerStore) MarkBanned(uid, reason string) error {
	_, err := s.db.Exec(`UPDATE player_history SET is_banned = 1, ban_reason = ? WHERE uid = ?`, reason, uid)
	return err
}

func (s *PlayerStore) MarkUnbanned(uid string) error {
	_, err := s.db.Exec(`UPDATE player_history SET is_banned = 0, ban_reason = NULL WHERE uid = ?`, uid)
	return err
}

func (s *PlayerStore) AddPlaytime(uid string, seconds int64) error {
	_, err := s.db.Exec(`UPDATE player_history SET total_playtime_seconds = total_playtime_seconds + ? WHERE uid = ?`, seconds, uid)
	return err
}

func (s *PlayerStore) GetOnlineUIDs() (map[string]time.Time, error) {
	rows, err := s.db.Query(`SELECT uid, last_seen FROM player_history WHERE online = 1`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	result := make(map[string]time.Time)
	for rows.Next() {
		var uid string
		var lastSeen time.Time
		if err := rows.Scan(&uid, &lastSeen); err != nil {
			return nil, err
		}
		result[uid] = lastSeen
	}
	return result, rows.Err()
}
