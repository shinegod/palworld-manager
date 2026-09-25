package store

import (
	"database/sql"
	"time"
)

// Alert 监控中心告警 (与前端 Monitor.vue / api Alert 接口对齐)
type Alert struct {
	ID           int64     `json:"id"`
	Level        string    `json:"level"`
	Category     string    `json:"category"`
	Message      string    `json:"message"`
	Details      string    `json:"details,omitempty"`
	Acknowledged bool      `json:"acknowledged"`
	CreatedAt    time.Time `json:"created_at"`
}

type AlertStore struct {
	db *sql.DB
}

func NewAlertStore(db *sql.DB) *AlertStore {
	return &AlertStore{db: db}
}

// AddAlert 写入告警; 相同消息10分钟内未确认的不重复写 (轮询每60秒跑一次, 防刷屏)
func (s *AlertStore) AddAlert(level, category, message, details string) error {
	var n int
	err := s.db.QueryRow(
		"SELECT COUNT(*) FROM alerts WHERE message = ? AND acknowledged = 0 AND created_at > ?",
		message, time.Now().Add(-10*time.Minute)).Scan(&n)
	if err != nil {
		return err
	}
	if n > 0 {
		return nil
	}
	_, err = s.db.Exec(
		"INSERT INTO alerts (level, category, message, details) VALUES (?,?,?,?)",
		level, category, message, details)
	return err
}

func (s *AlertStore) ListAlerts(limit int) ([]Alert, error) {
	rows, err := s.db.Query("SELECT id, level, category, message, COALESCE(details, ''), acknowledged, created_at " +
		"FROM alerts ORDER BY id DESC LIMIT ?", limit)
	if err != nil {
		return nil, err
	}
	defer func() { _ = rows.Close() }()
	var out []Alert
	for rows.Next() {
		var a Alert
		if err := rows.Scan(&a.ID, &a.Level, &a.Category, &a.Message, &a.Details, &a.Acknowledged, &a.CreatedAt); err != nil {
			return nil, err
		}
		out = append(out, a)
	}
	return out, rows.Err()
}

func (s *AlertStore) AckAlert(id int64) error {
	_, err := s.db.Exec("UPDATE alerts SET acknowledged = 1 WHERE id = ?", id)
	return err
}

func (s *AlertStore) ClearAlerts() error {
	_, err := s.db.Exec("DELETE FROM alerts")
	return err
}
