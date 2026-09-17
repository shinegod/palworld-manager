package config

import (
	"os"

	"gopkg.in/yaml.v3"
)

type Config struct {
	Server    ServerConfig    `yaml:"server"`
	Auth      AuthConfig      `yaml:"auth"`
	Palworld  PalworldConfig  `yaml:"palworld"`
	RCON      RCONConfig      `yaml:"rcon"`
	Bridge    BridgeConfig    `yaml:"bridge"`
	PalHook   PalHookConfig   `yaml:"palhook"`
	Database  DatabaseConfig  `yaml:"database"`
	Scheduler SchedulerConfig `yaml:"scheduler"`
	Metrics   MetricsConfig   `yaml:"metrics"`
}

type PalHookConfig struct {
	URL      string `yaml:"url"`
	Password string `yaml:"password"`
}

type BridgeConfig struct {
	URL   string `yaml:"url"`
	Token string `yaml:"token"`
}

type ServerConfig struct {
	Host string `yaml:"host"`
	Port int    `yaml:"port"`
}

type AuthConfig struct {
	Username         string `yaml:"username"`
	Password         string `yaml:"password"`
	JWTSecret        string `yaml:"jwt_secret"`
	TokenExpireHours int    `yaml:"token_expire_hours"`
}

type PalworldConfig struct {
	APIURL          string `yaml:"api_url"`
	APIUser         string `yaml:"api_user"`
	APIPassword     string `yaml:"api_password"`
	GameDataEnabled bool   `yaml:"gamedata_enabled"`
}

type RCONConfig struct {
	Host     string `yaml:"host"`
	Port     int    `yaml:"port"`
	Password string `yaml:"password"`
	Enabled  bool   `yaml:"enabled"`
}

type DatabaseConfig struct {
	Path string `yaml:"path"`
}

type SchedulerConfig struct {
	AutoRestartEnabled       bool  `yaml:"auto_restart_enabled"`
	AutoRestartIntervalHours int   `yaml:"auto_restart_interval_hours"`
	RestartWarningMinutes    []int `yaml:"restart_warning_minutes"`
	AutoBackupEnabled        bool  `yaml:"auto_backup_enabled"`
	AutoBackupIntervalHours  int   `yaml:"auto_backup_interval_hours"`
	BackupRetentionCount     int   `yaml:"backup_retention_count"`
}

type MetricsConfig struct {
	PollIntervalSeconds int `yaml:"poll_interval_seconds"`
	RetentionHours      int `yaml:"retention_hours"`
}

func Load(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var cfg Config
	if err := yaml.Unmarshal(data, &cfg); err != nil {
		return nil, err
	}
	cfg.setDefaults()
	return &cfg, nil
}

func (c *Config) setDefaults() {
	if c.Server.Host == "" {
		c.Server.Host = "0.0.0.0"
	}
	if c.Server.Port == 0 {
		c.Server.Port = 8080
	}
	if c.Auth.Username == "" {
		c.Auth.Username = "admin"
	}
	if c.Auth.TokenExpireHours == 0 {
		c.Auth.TokenExpireHours = 24
	}
	if c.Database.Path == "" {
		c.Database.Path = "./data/palmanager.db"
	}
	if c.Metrics.PollIntervalSeconds == 0 {
		c.Metrics.PollIntervalSeconds = 10
	}
	if c.Metrics.RetentionHours == 0 {
		c.Metrics.RetentionHours = 168
	}
	if c.RCON.Host == "" {
		c.RCON.Host = "127.0.0.1"
	}
	if c.RCON.Port == 0 {
		c.RCON.Port = 25575
	}
	if c.Scheduler.BackupRetentionCount == 0 {
		c.Scheduler.BackupRetentionCount = 48
	}
}
