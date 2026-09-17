package store

import (
	"database/sql"
	"encoding/json"
)

type ConfigStore struct {
	db *sql.DB
}

func NewConfigStore(db *sql.DB) *ConfigStore {
	return &ConfigStore{db: db}
}

func (s *ConfigStore) Get(key string) (string, bool) {
	var val string
	err := s.db.QueryRow("SELECT value FROM app_config WHERE key = ?", key).Scan(&val)
	if err != nil {
		return "", false
	}
	return val, true
}

func (s *ConfigStore) Set(key, value string) error {
	_, err := s.db.Exec(`INSERT INTO app_config (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, key, value)
	return err
}

func (s *ConfigStore) Delete(key string) error {
	_, err := s.db.Exec("DELETE FROM app_config WHERE key = ?", key)
	return err
}

func (s *ConfigStore) GetAll() (map[string]string, error) {
	rows, err := s.db.Query("SELECT key, value FROM app_config")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	result := make(map[string]string)
	for rows.Next() {
		var k, v string
		if err := rows.Scan(&k, &v); err != nil {
			continue
		}
		result[k] = v
	}
	return result, nil
}

type ConnectionConfig struct {
	PalworldAPIURL  string `json:"palworld_api_url"`
	PalworldUser    string `json:"palworld_user"`
	PalworldPass    string `json:"palworld_pass"`
	RCONHost        string `json:"rcon_host"`
	RCONPort        int    `json:"rcon_port"`
	RCONPass        string `json:"rcon_pass"`
	RCONEnabled     bool   `json:"rcon_enabled"`
	GameDataEnabled bool   `json:"gamedata_enabled"`
	BridgeURL       string `json:"bridge_url"`
	BridgeToken     string `json:"bridge_token"`
	PalHookURL      string `json:"palhook_url"`
	PalHookPassword string `json:"palhook_password"`
	AdminUser       string `json:"admin_user"`
	AdminPass       string `json:"admin_pass"`
	JWTSecret       string `json:"jwt_secret"`
	ListenPort      int    `json:"listen_port"`
}

func (s *ConfigStore) GetConnectionConfig() *ConnectionConfig {
	raw, ok := s.Get("connection")
	if !ok {
		return nil
	}
	var cc ConnectionConfig
	if err := json.Unmarshal([]byte(raw), &cc); err != nil {
		return nil
	}
	return &cc
}

func (s *ConfigStore) SaveConnectionConfig(cc *ConnectionConfig) error {
	data, err := json.Marshal(cc)
	if err != nil {
		return err
	}
	return s.Set("connection", string(data))
}

func (s *ConfigStore) IsConfigured() bool {
	_, ok := s.Get("connection")
	return ok
}
