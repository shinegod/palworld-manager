package store

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"

	"github.com/rs/zerolog/log"
	_ "modernc.org/sqlite"
)

func InitDB(dbPath string) (*sql.DB, error) {
	dir := filepath.Dir(dbPath)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return nil, fmt.Errorf("create db dir: %w", err)
	}

	db, err := sql.Open("sqlite", dbPath+"?_pragma=journal_mode(wal)&_pragma=busy_timeout(5000)")
	if err != nil {
		return nil, fmt.Errorf("open db: %w", err)
	}

	if err := db.Ping(); err != nil {
		return nil, fmt.Errorf("ping db: %w", err)
	}

	if err := runMigrations(db); err != nil {
		return nil, fmt.Errorf("migrations: %w", err)
	}

	log.Info().Str("path", dbPath).Msg("database initialized")
	return db, nil
}

func runMigrations(db *sql.DB) error {
	migrations := []string{
		`CREATE TABLE IF NOT EXISTS trends (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			timestamp DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
			server_fps INTEGER,
			frame_time REAL,
			player_count INTEGER,
			max_players INTEGER,
			uptime INTEGER,
			in_game_days INTEGER,
			cpu_percent REAL,
			mem_percent REAL,
			disk_percent REAL,
			palserver_cpu REAL,
			palserver_mem INTEGER
		)`,
		`CREATE INDEX IF NOT EXISTS idx_trends_ts ON trends(timestamp)`,

		`CREATE TABLE IF NOT EXISTS player_history (
			uid TEXT PRIMARY KEY,
			steam_id TEXT,
			name TEXT NOT NULL,
			account_name TEXT,
			level INTEGER DEFAULT 0,
			ip TEXT,
			platform TEXT,
			online INTEGER DEFAULT 0,
			first_seen DATETIME DEFAULT CURRENT_TIMESTAMP,
			last_seen DATETIME DEFAULT CURRENT_TIMESTAMP,
			total_playtime_seconds INTEGER DEFAULT 0,
			is_banned INTEGER DEFAULT 0,
			ban_reason TEXT
		)`,

		`CREATE TABLE IF NOT EXISTS bans (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			player_uid TEXT NOT NULL,
			player_name TEXT,
			reason TEXT,
			banned_by TEXT,
			banned_at DATETIME DEFAULT CURRENT_TIMESTAMP,
			unbanned_at DATETIME
		)`,

		`CREATE TABLE IF NOT EXISTS events (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			name TEXT NOT NULL,
			description TEXT,
			enabled INTEGER DEFAULT 1,
			schedule_type TEXT NOT NULL,
			schedule_value TEXT NOT NULL,
			actions TEXT NOT NULL DEFAULT '[]',
			recipients TEXT,
			on_end_actions TEXT,
			next_run DATETIME,
			last_run DATETIME,
			is_template INTEGER DEFAULT 0,
			created_at DATETIME DEFAULT CURRENT_TIMESTAMP
		)`,

		`CREATE TABLE IF NOT EXISTS event_actions (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			event_id INTEGER NOT NULL REFERENCES events(id) ON DELETE CASCADE,
			action_order INTEGER NOT NULL,
			action_type TEXT NOT NULL,
			action_params TEXT NOT NULL,
			target_type TEXT,
			target_value TEXT
		)`,

		`CREATE TABLE IF NOT EXISTS kits (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			name TEXT NOT NULL UNIQUE,
			description TEXT,
			is_starter_kit INTEGER DEFAULT 0,
			created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
			updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
		)`,

		`CREATE TABLE IF NOT EXISTS kit_items (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			kit_id INTEGER NOT NULL REFERENCES kits(id) ON DELETE CASCADE,
			item_type TEXT NOT NULL,
			item_id TEXT NOT NULL,
			count INTEGER DEFAULT 1,
			metadata TEXT
		)`,

		`CREATE TABLE IF NOT EXISTS kit_grants (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			kit_id INTEGER NOT NULL,
			player_uid TEXT NOT NULL,
			granted_at DATETIME DEFAULT CURRENT_TIMESTAMP
		)`,

		`CREATE TABLE IF NOT EXISTS anticheat_scans (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			scan_type TEXT NOT NULL,
			started_at DATETIME DEFAULT CURRENT_TIMESTAMP,
			finished_at DATETIME,
			status TEXT DEFAULT 'running',
			result_summary TEXT
		)`,

		`CREATE TABLE IF NOT EXISTS anticheat_flags (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			scan_id INTEGER REFERENCES anticheat_scans(id),
			player_uid TEXT NOT NULL,
			flag_type TEXT NOT NULL,
			severity TEXT NOT NULL,
			details TEXT,
			created_at DATETIME DEFAULT CURRENT_TIMESTAMP
		)`,

		`CREATE TABLE IF NOT EXISTS alerts (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			level TEXT NOT NULL,
			category TEXT NOT NULL,
			message TEXT NOT NULL,
			details TEXT,
			acknowledged INTEGER DEFAULT 0,
			created_at DATETIME DEFAULT CURRENT_TIMESTAMP
		)`,
		`CREATE INDEX IF NOT EXISTS idx_alerts_created ON alerts(created_at)`,

		`CREATE TABLE IF NOT EXISTS backups (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			filename TEXT NOT NULL,
			size_bytes INTEGER,
			save_dir TEXT,
			notes TEXT,
			created_at DATETIME DEFAULT CURRENT_TIMESTAMP
		)`,

		`CREATE TABLE IF NOT EXISTS audit_log (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			username TEXT NOT NULL,
			action TEXT NOT NULL,
			target TEXT,
			details TEXT,
			created_at DATETIME DEFAULT CURRENT_TIMESTAMP
		)`,
		`CREATE INDEX IF NOT EXISTS idx_audit_created ON audit_log(created_at)`,

		`CREATE TABLE IF NOT EXISTS console_history (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			command TEXT NOT NULL,
			output TEXT,
			executed_at DATETIME DEFAULT CURRENT_TIMESTAMP
		)`,

		`CREATE TABLE IF NOT EXISTS app_config (
			key TEXT PRIMARY KEY,
			value TEXT NOT NULL
		)`,
	}

	for _, m := range migrations {
		if _, err := db.Exec(m); err != nil {
			return fmt.Errorf("exec migration: %w\nSQL: %s", err, m)
		}
	}

	// 轻量列迁移: 早期版本的 anticheat_flags 表缺 resolved/action_taken 两列
	type colMig struct{ table, column, ddl string }
	for _, c := range []colMig{
		{"anticheat_flags", "resolved", "ALTER TABLE anticheat_flags ADD COLUMN resolved INTEGER DEFAULT 0"},
		{"anticheat_flags", "action_taken", "ALTER TABLE anticheat_flags ADD COLUMN action_taken TEXT DEFAULT ''"},
	} {
		rows, err := db.Query("PRAGMA table_info(" + c.table + ")")
		if err != nil {
			continue
		}
		has := false
		for rows.Next() {
			var cid, notnull, pk int
			var name, ctype string
			var dflt sql.NullString
			if rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk) == nil && name == c.column {
				has = true
				break
			}
		}
		rows.Close()
		if !has {
			if _, err := db.Exec(c.ddl); err != nil {
				return fmt.Errorf("migrate %s.%s: %w", c.table, c.column, err)
			}
		}
	}
	return nil
}
