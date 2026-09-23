package main

import (
	"context"
	"flag"
	"fmt"
	"io/fs"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/gin-contrib/cors"
	"github.com/gin-gonic/gin"
	"github.com/rs/zerolog"
	"github.com/rs/zerolog/log"

	palmanager "github.com/shinegod/palworld-manager"
	"github.com/shinegod/palworld-manager/internal/handler"
	"github.com/shinegod/palworld-manager/internal/shared/config"
	"github.com/shinegod/palworld-manager/internal/store"
)

var version = "dev"

func main() {
	configPath := flag.String("config", "", "path to config file (optional, can configure via web UI)")
	listenPort := flag.Int("port", 8080, "listen port")
	dbPath := flag.String("db", "./data/palmanager.db", "database path")
	flag.Parse()

	log.Logger = zerolog.New(zerolog.ConsoleWriter{Out: os.Stderr, TimeFormat: time.RFC3339}).
		With().Timestamp().Caller().Logger()

	// Init database first — config may come from it
	db, err := store.InitDB(*dbPath)
	if err != nil {
		log.Fatal().Err(err).Msg("init database")
	}
	defer db.Close()

	configStore := store.NewConfigStore(db)

	// Build effective config: file > db > defaults
	cfg := buildConfig(*configPath, configStore, *listenPort)

	// Stores (hook-only架构: 仅保留必要存储)
	playerStore := store.NewPlayerStore(db)
	banStore := store.NewBanStore(db)

	// Handlers
	authHandler := handler.NewAuthHandler(&cfg.Auth)
	appConfigHandler := handler.NewAppConfigHandler(configStore)
	eventHandler := handler.NewEventHandler(db, configStore)
	palhookHandler := handler.NewPalHookHandler(configStore)
	hookPlayers := handler.NewPalHookPlayersHandler(configStore, banStore, playerStore)
	hookDash := handler.NewHookDashboardHandler(configStore)
	ipInfo := handler.NewIPInfoHandler()
	anticheatHandler := handler.NewAnticheatHandler(db, configStore)
	alertHandler := handler.NewAlertHandler(db)
	backupHandler := handler.NewBackupHandler(db, configStore)

	// Router
	gin.SetMode(gin.ReleaseMode)
	r := gin.New()
	r.Use(gin.Recovery())
	// 注意: AllowCredentials 与 AllowOrigins:"*" 按 CORS 规范互斥, 浏览器会直接拒绝。
	// 本项目用 Bearer token 而非 Cookie, 不需要 credentials。
	r.Use(cors.New(cors.Config{
		AllowOrigins:     []string{"*"},
		AllowMethods:     []string{"GET", "POST", "PUT", "DELETE", "OPTIONS"},
		AllowHeaders:     []string{"Origin", "Content-Type", "Authorization"},
		AllowCredentials: false,
		MaxAge:           12 * time.Hour,
	}))

	// Public routes (no auth)
	r.POST("/api/auth/login", authHandler.Login)
	r.GET("/api/setup/status", appConfigHandler.GetSetupStatus)

	// Protected routes — 全部由 PalHook 提供数据 (hook-only)
	api := r.Group("/api")
	api.Use(handler.AuthMiddleware(cfg.Auth.JWTSecret))
	{
		// App config (connection settings)
		api.GET("/config/connection", appConfigHandler.GetConnectionConfig)
		api.POST("/config/connection", appConfigHandler.SaveConnectionConfig)

		// Dashboard (PalHook /metrics + /players)
		api.GET("/dashboard/realtime", hookDash.GetRealtime)
		api.GET("/dashboard/trends", hookDash.GetTrends)
		api.GET("/dashboard/info", hookDash.GetInfo)
		api.GET("/dashboard/alerts", alertHandler.GetAlerts)
		api.POST("/dashboard/alerts/ack", alertHandler.AckAlert)
		api.POST("/dashboard/alerts/clear", alertHandler.ClearAlerts)

		// Players (PalHook + 本地封禁记录)
		api.GET("/players/online", hookPlayers.GetOnlinePlayers)
		api.GET("/players/history", hookPlayers.GetPlayerHistory)
		api.POST("/players/kick", hookPlayers.KickPlayer)
		api.POST("/players/ban", hookPlayers.BanPlayer)
		api.POST("/players/unban", hookPlayers.UnbanPlayer)
		api.GET("/players/bans", hookPlayers.GetBans)

		// Server (PalHook)
		api.POST("/server/announce", palhookHandler.Announce)
		api.POST("/server/save", func(c *gin.Context) {
			c.JSON(http.StatusOK, gin.H{"status": "ok", "note": "游戏自动存档, 无需手动保存"})
		})
		api.GET("/server/settings", hookDash.GetInfo)

		// Map (PalHook /players 坐标)
		api.GET("/map/players", hookDash.GetPlayerPositions)

		// IP 归属地 (后端代理查询, 带缓存)
		api.GET("/ipinfo", ipInfo.Get)

		// PalHook 直连代理 (在线管理操作)
		api.GET("/palhook/health", palhookHandler.Health)
		api.POST("/palhook/give-item", palhookHandler.GiveItem)
		api.POST("/palhook/give-exp", palhookHandler.GiveExp)
		api.POST("/palhook/give-money", palhookHandler.GiveMoney)
		api.POST("/palhook/spawn-pal", palhookHandler.SpawnPal)
		api.POST("/palhook/teleport", palhookHandler.Teleport)
		api.POST("/palhook/set-tech-points", palhookHandler.SetTechPoints)
		api.POST("/palhook/set-level", palhookHandler.SetLevel)
		api.GET("/palhook/players", palhookHandler.Players)
		api.POST("/palhook/announce", palhookHandler.Announce)
		api.POST("/palhook/kick", palhookHandler.Kick)
		api.GET("/palhook/metrics", palhookHandler.Metrics)
		api.POST("/palhook/chat", palhookHandler.Chat)
		api.GET("/palhook/guilds", palhookHandler.Guilds)

		// Events (always available)
		api.GET("/events", eventHandler.List)
		api.POST("/events", eventHandler.Create)
		api.PUT("/events/:id", eventHandler.Update)
		api.DELETE("/events/:id", eventHandler.Delete)
		api.POST("/events/:id/trigger", eventHandler.Trigger)

		// AntiCheat 占位 (hook侧暂无数据)
		api.GET("/anticheat/flags", anticheatHandler.GetFlags)
		api.POST("/anticheat/scan", anticheatHandler.Scan)
		api.POST("/anticheat/flags/:id/resolve", anticheatHandler.ResolveFlag)

		// 备份/恢复/删档 (面板与PalServer同机部署时直接读写存档目录)
		api.GET("/backups/config", backupHandler.GetConfig)
		api.POST("/backups/config", backupHandler.SaveConfig)
		api.GET("/backups/saves", backupHandler.ListSaves)
		api.POST("/backups/create", backupHandler.Create)
		api.GET("/backups/list", backupHandler.List)
		api.POST("/backups/restore", backupHandler.Restore)
		api.POST("/backups/delete", backupHandler.Delete)
		api.POST("/backups/wipe", backupHandler.Wipe)
		api.GET("/backups/download", backupHandler.Download)
		api.POST("/backups/restart", backupHandler.Restart)
	}

	// Embedded frontend with SPA fallback
	frontendFS, err := fs.Sub(palmanager.FrontendFS, "web/dist")
	if err != nil {
		log.Fatal().Err(err).Msg("frontend fs")
	}
	fileServer := http.FileServer(http.FS(frontendFS))
	r.NoRoute(func(c *gin.Context) {
		f, err := frontendFS.Open(c.Request.URL.Path[1:])
		if err == nil {
			f.Close()
			fileServer.ServeHTTP(c.Writer, c.Request)
			return
		}
		c.Request.URL.Path = "/"
		fileServer.ServeHTTP(c.Writer, c.Request)
	})

	// 后台历史快照轮询 (只打 PalHook /players, 60s一次)
	historyStop := make(chan struct{})
	go handler.StartHookHistoryRecorder(configStore, playerStore, anticheatHandler, historyStop)

	// 事件系统定时调度器 (单次/循环/Cron 触发公告/聊天/重启)
	go eventHandler.Scheduler().Start(historyStop)

	// Start server
	addr := fmt.Sprintf("0.0.0.0:%d", cfg.Server.Port)
	srv := &http.Server{Addr: addr, Handler: r}

	log.Info().Str("version", version).Str("addr", addr).Msg("palmanager starting (hook-only)")
	if configStore.GetConnectionConfig() == nil || configStore.GetConnectionConfig().PalHookURL == "" {
		log.Warn().Msg("PalHook未配置 — 打开浏览器 设置→连接配置 填写 PalHook 地址")
	}

	go func() {
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatal().Err(err).Msg("server error")
		}
	}()

	// Graceful shutdown
	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit
	log.Info().Msg("shutting down...")
	close(historyStop)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := srv.Shutdown(ctx); err != nil {
		log.Error().Err(err).Msg("server shutdown")
	}
	log.Info().Msg("bye")
}

func buildConfig(filePath string, cs *store.ConfigStore, defaultPort int) *config.Config {
	// Try config file first
	if filePath != "" {
		cfg, err := config.Load(filePath)
		if err == nil {
			return cfg
		}
		log.Warn().Err(err).Str("path", filePath).Msg("config file load failed, trying database")
	}

	// Try database config
	cc := cs.GetConnectionConfig()
	if cc != nil {
		port := cc.ListenPort
		if port == 0 {
			port = defaultPort
		}
		jwtSecret := cc.JWTSecret
		if jwtSecret == "" {
			jwtSecret = "palmanager-default-jwt-secret"
		}
		return &config.Config{
			Server: config.ServerConfig{Host: "0.0.0.0", Port: port},
			Auth: config.AuthConfig{
				Username:         cc.AdminUser,
				Password:         cc.AdminPass,
				JWTSecret:        jwtSecret,
				TokenExpireHours: 24,
			},
			PalHook: config.PalHookConfig{
				URL:      cc.PalHookURL,
				Password: cc.PalHookPassword,
			},
			Database: config.DatabaseConfig{Path: "./data/palmanager.db"},
			Metrics:  config.MetricsConfig{PollIntervalSeconds: 10, RetentionHours: 168},
		}
	}

	// Default: unconfigured state, just serve frontend for setup
	return &config.Config{
		Server: config.ServerConfig{Host: "0.0.0.0", Port: defaultPort},
		Auth: config.AuthConfig{
			Username:         "admin",
			Password:         "admin",
			JWTSecret:        "palmanager-initial-setup",
			TokenExpireHours: 24,
		},
		Database: config.DatabaseConfig{Path: "./data/palmanager.db"},
		Metrics:  config.MetricsConfig{PollIntervalSeconds: 10, RetentionHours: 168},
	}
}
