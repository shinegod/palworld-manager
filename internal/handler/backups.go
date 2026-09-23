package handler

import (
	"archive/zip"
	"database/sql"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/shinegod/palworld-manager/internal/shared/model"
	"github.com/shinegod/palworld-manager/internal/store"
)

// BackupHandler 存档备份/恢复/删档。面板与 PalServer 同机 (MCSM) 部署时
// 可直接读写存档目录; 危险操作 (恢复/删档) 强制要求游戏服务器已停止。
type BackupHandler struct {
	db     *sql.DB
	bs     *store.BackupStore
	cs     *store.ConfigStore
	client *http.Client
}

func NewBackupHandler(db *sql.DB, cs *store.ConfigStore) *BackupHandler {
	return &BackupHandler{
		db:     db,
		bs:     store.NewBackupStore(db),
		cs:     cs,
		client: &http.Client{Timeout: 4 * time.Second},
	}
}

func (h *BackupHandler) saveDir() string {
	if v, ok := h.cs.Get("save_dir"); ok && v != "" {
		return v
	}
	return "/workspace/pal/Pal/Saved/SaveGames"
}

func (h *BackupHandler) backupDir() string {
	if v, ok := h.cs.Get("backup_dir"); ok && v != "" {
		return v
	}
	return "data/backups"
}

// serverOnline 游戏服务器是否在线 (PalHook /health 可达即在线)
func (h *BackupHandler) serverOnline() bool {
	cc := h.cs.GetConnectionConfig()
	if cc == nil || cc.PalHookURL == "" {
		return false
	}
	req, err := http.NewRequest(http.MethodGet, cc.PalHookURL+"/health", nil)
	if err != nil {
		return false
	}
	req.SetBasicAuth("admin", cc.PalHookPassword)
	resp, err := h.client.Do(req)
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	return resp.StatusCode == http.StatusOK
}

// GET/POST /backups/config
func (h *BackupHandler) GetConfig(c *gin.Context) {
	c.JSON(http.StatusOK, gin.H{
		"save_dir":      h.saveDir(),
		"backup_dir":    h.backupDir(),
		"server_online": h.serverOnline(),
	})
}

func (h *BackupHandler) SaveConfig(c *gin.Context) {
	var req struct {
		SaveDir   string `json:"save_dir"`
		BackupDir string `json:"backup_dir"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "bad request"})
		return
	}
	if req.SaveDir != "" {
		_ = h.cs.Set("save_dir", req.SaveDir)
	}
	if req.BackupDir != "" {
		_ = h.cs.Set("backup_dir", req.BackupDir)
	}
	c.JSON(http.StatusOK, gin.H{"status": "ok"})
}

// GET /backups/saves 当前存档目录内容 (最多50项, 按大小倒序)
func (h *BackupHandler) ListSaves(c *gin.Context) {
	type saveFile struct {
		Path    string `json:"path"`
		Size    int64  `json:"size"`
		ModTime string `json:"mod_time"`
	}
	var out []saveFile
	dir := h.saveDir()
	if st, err := os.Stat(dir); err != nil || !st.IsDir() {
		c.JSON(http.StatusOK, gin.H{"save_dir": dir, "exists": false, "files": []saveFile{}})
		return
	}
	_ = filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() || len(out) >= 200 {
			return nil
		}
		rel, _ := filepath.Rel(dir, path)
		out = append(out, saveFile{Path: rel, Size: info.Size(), ModTime: info.ModTime().Format("2006-01-02 15:04:05")})
		return nil
	})
	sort.Slice(out, func(i, j int) bool { return out[i].Size > out[j].Size })
	if len(out) > 50 {
		out = out[:50]
	}
	c.JSON(http.StatusOK, gin.H{"save_dir": dir, "exists": true, "files": out})
}

// POST /backups/create 立即备份 (在线时也可执行, 但会在备注里提示)
func (h *BackupHandler) Create(c *gin.Context) {
	dir := h.saveDir()
	if st, err := os.Stat(dir); err != nil || !st.IsDir() {
		c.JSON(http.StatusBadRequest, gin.H{"error": "存档目录不存在: " + dir + " (请在配置里设置正确的存档路径)"})
		return
	}
	bdir := h.backupDir()
	if err := os.MkdirAll(bdir, 0o755); err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "创建备份目录失败: " + err.Error()})
		return
	}
	name := fmt.Sprintf("save-%s.zip", time.Now().Format("20060102-150405"))
	path := filepath.Join(bdir, name)
	if err := zipDir(dir, path); err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "备份失败: " + err.Error()})
		return
	}
	st, _ := os.Stat(path)
	var size int64
	if st != nil {
		size = st.Size()
	}
	notes := "手动备份"
	if h.serverOnline() {
		notes = "服务器在线时备份 (存档文件可能处于写入中, 建议停服后备份)"
	}
	_ = h.bs.Add(name, dir, notes, size)
	h.bs.AuditLog(operator(c), "backup_create", name, dir)
	c.JSON(http.StatusOK, gin.H{"status": "ok", "filename": name, "size": size})
}

// GET /backups/list
func (h *BackupHandler) List(c *gin.Context) {
	list, err := h.bs.List(100)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	if list == nil {
		list = []model.Backup{}
	}
	c.JSON(http.StatusOK, list)
}

// POST /backups/restore {id} 恢复备份 (必须停服)
func (h *BackupHandler) Restore(c *gin.Context) {
	var req struct {
		ID int64 `json:"id"`
	}
	if err := c.ShouldBindJSON(&req); err != nil || req.ID <= 0 {
		c.JSON(http.StatusBadRequest, gin.H{"error": "id is required"})
		return
	}
	if h.serverOnline() {
		c.JSON(http.StatusConflict, gin.H{"error": "游戏服务器仍在运行, 请先在MCSM停止服务器再恢复"})
		return
	}
	b, err := h.bs.GetByID(req.ID)
	if err != nil {
		c.JSON(http.StatusNotFound, gin.H{"error": "备份不存在"})
		return
	}
	zipPath := filepath.Join(h.backupDir(), filepath.Base(b.Filename))
	if _, err := os.Stat(zipPath); err != nil {
		c.JSON(http.StatusNotFound, gin.H{"error": "备份文件不存在: " + zipPath})
		return
	}
	// 恢复前先把当前存档再备份一份 (安全网)
	safety := fmt.Sprintf("save-pre-restore-%s.zip", time.Now().Format("20060102-150405"))
	_ = zipDir(h.saveDir(), filepath.Join(h.backupDir(), safety))
	_ = h.bs.Add(safety, h.saveDir(), "恢复前自动备份", 0)
	if err := unzipDir(zipPath, h.saveDir()); err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "恢复失败: " + err.Error()})
		return
	}
	h.bs.AuditLog(operator(c), "backup_restore", b.Filename, h.saveDir())
	c.JSON(http.StatusOK, gin.H{"status": "ok", "note": "恢复完成, 请启动游戏服务器"})
}

// POST /backups/delete {id}
func (h *BackupHandler) Delete(c *gin.Context) {
	var req struct {
		ID int64 `json:"id"`
	}
	if err := c.ShouldBindJSON(&req); err != nil || req.ID <= 0 {
		c.JSON(http.StatusBadRequest, gin.H{"error": "id is required"})
		return
	}
	b, err := h.bs.GetByID(req.ID)
	if err != nil {
		c.JSON(http.StatusNotFound, gin.H{"error": "备份不存在"})
		return
	}
	zipPath := filepath.Join(h.backupDir(), filepath.Base(b.Filename))
	_ = os.Remove(zipPath)
	_ = h.bs.Delete(req.ID)
	c.JSON(http.StatusOK, gin.H{"status": "ok"})
}

// POST /backups/wipe {scope:"players"|"world"} 删档 (必须停服, 删前自动备份)
func (h *BackupHandler) Wipe(c *gin.Context) {
	var req struct {
		Scope string `json:"scope"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "bad request"})
		return
	}
	if req.Scope != "players" && req.Scope != "world" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "scope 必须是 players(仅玩家角色) 或 world(整个世界)"})
		return
	}
	if h.serverOnline() {
		c.JSON(http.StatusConflict, gin.H{"error": "游戏服务器仍在运行, 请先在MCSM停止服务器再删档"})
		return
	}
	dir := h.saveDir()
	// 删档前强制备份
	safety := fmt.Sprintf("save-pre-wipe-%s.zip", time.Now().Format("20060102-150405"))
	bdir := h.backupDir()
	_ = os.MkdirAll(bdir, 0o755)
	if err := zipDir(dir, filepath.Join(bdir, safety)); err == nil {
		_ = h.bs.Add(safety, dir, "删档前自动备份", 0)
	}
	// 找世界目录 SaveGames/0/<32位hex GUID>/
	worldRe := regexp.MustCompile("^[0-9A-Fa-f]{32}$")
	var deleted []string
	entries, _ := os.ReadDir(dir)
	for _, e := range entries {
		if !e.IsDir() || !worldRe.MatchString(e.Name()) {
			continue
		}
		worldDir := filepath.Join(dir, e.Name())
		if req.Scope == "world" {
			if err := os.RemoveAll(worldDir); err == nil {
				deleted = append(deleted, worldDir)
			}
		} else {
			playersDir := filepath.Join(worldDir, "Players")
			if st, err := os.Stat(playersDir); err == nil && st.IsDir() {
				if err := os.RemoveAll(playersDir); err == nil {
					deleted = append(deleted, playersDir+"/*")
					_ = os.MkdirAll(playersDir, 0o755)
				}
			}
		}
	}
	if len(deleted) == 0 {
		c.JSON(http.StatusNotFound, gin.H{"error": "未找到可删除的存档 (检查存档目录配置)"})
		return
	}
	h.bs.AuditLog(operator(c), "wipe_"+req.Scope, strings.Join(deleted, ", "), dir)
	c.JSON(http.StatusOK, gin.H{"status": "ok", "deleted": deleted, "backup": safety, "note": "删档完成, 启动服务器后即为全新存档"})
}

// zipDir 把目录递归打包成 zip
func zipDir(srcDir, zipPath string) error {
	zf, err := os.Create(zipPath)
	if err != nil {
		return err
	}
	defer zf.Close()
	zw := zip.NewWriter(zf)
	defer zw.Close()
	return filepath.Walk(srcDir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}
		rel, err := filepath.Rel(srcDir, path)
		if err != nil {
			return err
		}
		wf, err := zw.Create(rel)
		if err != nil {
			return err
		}
		f, err := os.Open(path)
		if err != nil {
			return err
		}
		defer f.Close()
		_, err = io.Copy(wf, f)
		return err
	})
}

// unzipDir 解压 zip 到目标目录 (覆盖)
func unzipDir(zipPath, dstDir string) error {
	zr, err := zip.OpenReader(zipPath)
	if err != nil {
		return err
	}
	defer zr.Close()
	for _, f := range zr.File {
		// 防路径穿越
		dst := filepath.Join(dstDir, filepath.Clean("/"+f.Name))
		if !strings.HasPrefix(dst, filepath.Clean(dstDir)+string(os.PathSeparator)) && dst != filepath.Clean(dstDir) {
			continue
		}
		if f.FileInfo().IsDir() {
			_ = os.MkdirAll(dst, 0o755)
			continue
		}
		_ = os.MkdirAll(filepath.Dir(dst), 0o755)
		rc, err := f.Open()
		if err != nil {
			continue
		}
		wf, err := os.Create(dst)
		if err != nil {
			rc.Close()
			continue
		}
		_, _ = io.Copy(wf, rc)
		wf.Close()
		rc.Close()
	}
	return nil
}
