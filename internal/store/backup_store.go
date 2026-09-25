package store

import (
	"database/sql"

	"github.com/shinegod/palworld-manager/internal/shared/model"
)

type BackupStore struct {
	db *sql.DB
}

func NewBackupStore(db *sql.DB) *BackupStore {
	return &BackupStore{db: db}
}

func (s *BackupStore) List(limit int) ([]model.Backup, error) {
	rows, err := s.db.Query("SELECT id, filename, size_bytes, save_dir, COALESCE(notes, ''), created_at " +
		"FROM backups ORDER BY id DESC LIMIT ?", limit)
	if err != nil {
		return nil, err
	}
	defer func() { _ = rows.Close() }()
	var out []model.Backup
	for rows.Next() {
		var b model.Backup
		var size sql.NullInt64
		var saveDir sql.NullString
		if err := rows.Scan(&b.ID, &b.Filename, &size, &saveDir, &b.Notes, &b.CreatedAt); err != nil {
			return nil, err
		}
		b.SizeBytes = size.Int64
		b.SaveDir = saveDir.String
		out = append(out, b)
	}
	return out, rows.Err()
}

func (s *BackupStore) Add(filename, saveDir, notes string, sizeBytes int64) error {
	_, err := s.db.Exec("INSERT INTO backups (filename, size_bytes, save_dir, notes) VALUES (?,?,?,?)",
		filename, sizeBytes, saveDir, notes)
	return err
}

func (s *BackupStore) GetByID(id int64) (*model.Backup, error) {
	var b model.Backup
	var size sql.NullInt64
	var saveDir sql.NullString
	err := s.db.QueryRow("SELECT id, filename, size_bytes, save_dir, COALESCE(notes, ''), created_at " +
		"FROM backups WHERE id = ?", id).Scan(&b.ID, &b.Filename, &size, &saveDir, &b.Notes, &b.CreatedAt)
	if err != nil {
		return nil, err
	}
	b.SizeBytes = size.Int64
	b.SaveDir = saveDir.String
	return &b, nil
}

func (s *BackupStore) Delete(id int64) error {
	_, err := s.db.Exec("DELETE FROM backups WHERE id = ?", id)
	return err
}

// AuditLog 操作审计 (备份/恢复/删档等危险操作)
func (s *BackupStore) AuditLog(username, action, target, details string) {
	_, _ = s.db.Exec("INSERT INTO audit_log (username, action, target, details) VALUES (?,?,?,?)",
		username, action, target, details)
}

