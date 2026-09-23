package store

import (
	"database/sql"
	"time"
)

// AnticheatFlag 反作弊检测标记 (JSON 字段名与前端 AntiCheat.vue 对齐)
type AnticheatFlag struct {
	ID          int64     `json:"id"`
	PlayerUID   string    `json:"user_id"`
	PlayerName  string    `json:"player_name"`
	FlagType    string    `json:"flag_type"`
	Severity    string    `json:"severity"`
	Details     string    `json:"details"`
	Resolved    bool      `json:"resolved"`
	ActionTaken string    `json:"action_taken"`
	CreatedAt   time.Time `json:"flagged_at"`
}

type AnticheatStore struct {
	db *sql.DB
}

func NewAnticheatStore(db *sql.DB) *AnticheatStore {
	return &AnticheatStore{db: db}
}

func (s *AnticheatStore) CreateScan(scanType string) (int64, error) {
	res, err := s.db.Exec("INSERT INTO anticheat_scans (scan_type, status) VALUES (?, 'running')", scanType)
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

func (s *AnticheatStore) FinishScan(id int64, summary string) error {
	_, err := s.db.Exec("UPDATE anticheat_scans SET finished_at = ?, status = 'done', result_summary = ? WHERE id = ?",
		time.Now(), summary, id)
	return err
}

// HasUnresolved 同一玩家同一类型是否已有未处理标记 (去重, 避免每个轮询周期重复报警)
func (s *AnticheatStore) HasUnresolved(uid, flagType string) (bool, error) {
	var n int
	err := s.db.QueryRow(
		"SELECT COUNT(*) FROM anticheat_flags WHERE player_uid = ? AND flag_type = ? AND resolved = 0",
		uid, flagType).Scan(&n)
	return n > 0, err
}

func (s *AnticheatStore) AddFlag(scanID int64, uid, flagType, severity, details string) error {
	_, err := s.db.Exec(
		"INSERT INTO anticheat_flags (scan_id, player_uid, flag_type, severity, details) VALUES (?,?,?,?,?)",
		scanID, uid, flagType, severity, details)
	return err
}

func (s *AnticheatStore) ListFlags(limit int) ([]AnticheatFlag, error) {
	rows, err := s.db.Query("SELECT f.id, f.player_uid, COALESCE(h.name, ''), f.flag_type, f.severity, " +
		"COALESCE(f.details, ''), f.resolved, COALESCE(f.action_taken, ''), f.created_at " +
		"FROM anticheat_flags f LEFT JOIN player_history h ON h.uid = f.player_uid " +
		"ORDER BY f.id DESC LIMIT ?", limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []AnticheatFlag
	for rows.Next() {
		var f AnticheatFlag
		if err := rows.Scan(&f.ID, &f.PlayerUID, &f.PlayerName, &f.FlagType, &f.Severity,
			&f.Details, &f.Resolved, &f.ActionTaken, &f.CreatedAt); err != nil {
			return nil, err
		}
		out = append(out, f)
	}
	return out, rows.Err()
}

func (s *AnticheatStore) ResolveFlag(id int64, action string) error {
	_, err := s.db.Exec("UPDATE anticheat_flags SET resolved = 1, action_taken = ? WHERE id = ?", action, id)
	return err
}

func (s *AnticheatStore) ScanCount() int {
	var n int
	_ = s.db.QueryRow("SELECT COUNT(*) FROM anticheat_scans").Scan(&n)
	return n
}
