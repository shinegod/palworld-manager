package handler

import (
	"archive/zip"
	"bytes"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/shinegod/palworld-manager/internal/shared/model"
	"github.com/shinegod/palworld-manager/internal/store"
)

// BackupHandler 远程存档管理 (面板与游戏服通常不在同一台机器):
// 备份 = 从 PalHook /saves 拉取所有文件打包成 zip 存在面板本地;
// 恢复 = 解包本地 zip 逐文件上传到 hook 暂存区, 然后 /saves/apply 应用并重启游戏;
// 删档 = hook /saves/wipe (删前先在面板侧做一次远程备份), 服务器自动重启为全新存档。
type BackupHandler struct {
	db     *sql.DB
	bs     *store.BackupStore
	cs     *store.ConfigStore
	client *http.Client // 短超时 (状态检查)
	big    *http.Client // 长超时 (大文件传输)
}

func NewBackupHandler(db *sql.DB, cs *store.ConfigStore) *BackupHandler {
	return &BackupHandler{
		db:     db,
		bs:     store.NewBackupStore(db),
		cs:     cs,
		client: &http.Client{Timeout: 5 * time.Second},
		big:    &http.Client{Timeout: 30 * time.Minute},
	}
}

func (h *BackupHandler) backupDir() string {
	if v, ok := h.cs.Get("backup_dir"); ok && v != "" {
		return v
	}
	return "data/backups"
}

// hookDo 请求 PalHook, 返回响应体(最多8MB)与状态码
func (h *BackupHandler) hookDo(method, path string, body io.Reader, clen int64) ([]byte, int, error) {
	cc := h.cs.GetConnectionConfig()
	if cc == nil || cc.PalHookURL == "" {
		return nil, 0, fmt.Errorf("PalHook未配置")
	}
	req, err := http.NewRequest(method, cc.PalHookURL+path, body)
	if err != nil {
		return nil, 0, err
	}
	req.SetBasicAuth("admin", cc.PalHookPassword)
	if clen >= 0 {
		req.ContentLength = clen
	}
	client := h.client
	if body != nil {
		client = h.big
	}
	resp, err := client.Do(req)
	if err != nil {
		return nil, 0, err
	}
	defer func() { _ = resp.Body.Close() }()
	data, _ := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	return data, resp.StatusCode, nil
}

// serverOnline 游戏服务器是否在线 (PalHook /health 可达即在线)
func (h *BackupHandler) serverOnline() bool {
	_, code, err := h.hookDo(http.MethodGet, "/health", nil, -1)
	return err == nil && code == http.StatusOK
}

// hookSaves 拉取 hook 的存档文件列表
func (h *BackupHandler) hookSaves() (string, []map[string]any, error) {
	data, code, err := h.hookDo(http.MethodGet, "/saves", nil, -1)
	if err != nil || code != http.StatusOK {
		return "", nil, fmt.Errorf("PalHook /saves 失败: %v (code=%d)", err, code)
	}
	var pr struct {
		SaveDir string           `json:"save_dir"`
		Files   []map[string]any `json:"files"`
	}
	if err := json.Unmarshal(data, &pr); err != nil {
		return "", nil, err
	}
	return pr.SaveDir, pr.Files, nil
}

// GET/POST /backups/config
func (h *BackupHandler) GetConfig(c *gin.Context) {
	saveDir, _, _ := h.hookSaves()
	c.JSON(http.StatusOK, gin.H{
		"save_dir":      saveDir,
		"backup_dir":    h.backupDir(),
		"server_online": h.serverOnline(),
	})
}

func (h *BackupHandler) SaveConfig(c *gin.Context) {
	var req struct {
		BackupDir string `json:"backup_dir"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "bad request"})
		return
	}
	if req.BackupDir != "" {
		_ = h.cs.Set("backup_dir", req.BackupDir)
	}
	c.JSON(http.StatusOK, gin.H{"status": "ok"})
}

// GET /backups/saves 远程存档文件列表 (代理 hook)
func (h *BackupHandler) ListSaves(c *gin.Context) {
	saveDir, files, err := h.hookSaves()
	if err != nil {
		c.JSON(http.StatusBadGateway, gin.H{"error": err.Error()})
		return
	}
	if files == nil {
		files = []map[string]any{}
	}
	c.JSON(http.StatusOK, gin.H{"save_dir": saveDir, "exists": saveDir != "", "files": files})
}

// POST /backups/create 远程备份: 从 hook 拉全部文件打成 zip 存在面板本地
func (h *BackupHandler) Create(c *gin.Context) {
	saveDir, files, err := h.hookSaves()
	if err != nil {
		c.JSON(http.StatusBadGateway, gin.H{"error": err.Error()})
		return
	}
	if len(files) == 0 {
		c.JSON(http.StatusBadRequest, gin.H{"error": "远程存档为空 (hook /saves 没有返回文件)"})
		return
	}
	bdir := h.backupDir()
	if err := os.MkdirAll(bdir, 0o755); err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "创建备份目录失败: " + err.Error()})
		return
	}
	name := fmt.Sprintf("save-remote-%s.zip", time.Now().Format("20060102-150405"))
	path := filepath.Join(bdir, name)
	zf, err := os.Create(path)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}
	zw := zip.NewWriter(zf)
	skipped := 0
	cc := h.cs.GetConnectionConfig()
	for _, f := range files {
		rel, _ := f["path"].(string)
		if rel == "" {
			continue
		}
		req, err := http.NewRequest(http.MethodGet, cc.PalHookURL+"/saves/download?path="+rel, nil)
		if err != nil {
			skipped++
			continue
		}
		req.SetBasicAuth("admin", cc.PalHookPassword)
		resp, err := h.big.Do(req)
		if err != nil || resp.StatusCode != http.StatusOK {
			if resp != nil {
				_ = resp.Body.Close()
			}
			skipped++
			continue
		}
		w, err := zw.Create(rel)
		if err != nil {
			_ = resp.Body.Close()
			skipped++
			continue
		}
		_, _ = io.Copy(w, resp.Body)
		_ = resp.Body.Close()
	}
	_ = zw.Close()
	_ = zf.Close()
	st, _ := os.Stat(path)
	var size int64
	if st != nil {
		size = st.Size()
	}
	notes := "远程备份 (hook拉取)"
	if h.serverOnline() {
		notes = "远程备份 (服务器在线, 存档可能处于写入中)"
	}
	_ = h.bs.Add(name, saveDir, notes, size)
	h.bs.AuditLog(operator(c), "backup_create_remote", name, saveDir)
	c.JSON(http.StatusOK, gin.H{"status": "ok", "filename": name, "size": size, "files": len(files), "skipped": skipped})
}

// GET /backups/list 本地备份列表
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

// GET /backups/download?id= 下载本地备份 zip
func (h *BackupHandler) Download(c *gin.Context) {
	id, err := strconv.ParseInt(c.Query("id"), 10, 64)
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid id"})
		return
	}
	b, err := h.bs.GetByID(id)
	if err != nil {
		c.JSON(http.StatusNotFound, gin.H{"error": "备份不存在"})
		return
	}
	zipPath := filepath.Join(h.backupDir(), filepath.Base(b.Filename))
	c.FileAttachment(zipPath, b.Filename)
}

// POST /backups/restore {id} 远程恢复: 上传zip内容到hook暂存区并应用 (应用后服务器自动重启)
func (h *BackupHandler) Restore(c *gin.Context) {
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
	zr, err := zip.OpenReader(zipPath)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "打开备份失败: " + err.Error()})
		return
	}
	defer func() { _ = zr.Close() }()
	cc := h.cs.GetConnectionConfig()
	uploaded := 0
	for _, f := range zr.File {
		if f.FileInfo().IsDir() {
			continue
		}
		rc, err := f.Open()
		if err != nil {
			continue
		}
		data, err := io.ReadAll(io.LimitReader(rc, 512<<20))
		_ = rc.Close()
		if err != nil {
			continue
		}
		req2, err := http.NewRequest(http.MethodPost,
			cc.PalHookURL+"/saves/stage?path="+f.Name, bytes.NewReader(data))
		if err != nil {
			continue
		}
		req2.SetBasicAuth("admin", cc.PalHookPassword)
		req2.ContentLength = int64(len(data))
		resp, err := h.big.Do(req2)
		if err != nil {
			continue
		}
		_ = resp.Body.Close()
		if resp.StatusCode == http.StatusOK {
			uploaded++
		}
	}
	if uploaded == 0 {
		c.JSON(http.StatusBadRequest, gin.H{"error": "没有成功上传任何文件"})
		return
	}
	_, code, _ := h.hookDo(http.MethodPost, "/saves/apply", nil, -1)
	if code != http.StatusOK {
		c.JSON(http.StatusBadGateway, gin.H{"error": "应用存档失败 (hook /saves/apply)"})
		return
	}
	h.bs.AuditLog(operator(c), "backup_restore_remote", b.Filename, fmt.Sprintf("%d files", uploaded))
	c.JSON(http.StatusOK, gin.H{"status": "ok", "restarting": true, "uploaded": uploaded, "note": "存档已上传并应用, 服务器正在重启"})
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
	_ = os.Remove(filepath.Join(h.backupDir(), filepath.Base(b.Filename)))
	_ = h.bs.Delete(req.ID)
	c.JSON(http.StatusOK, gin.H{"status": "ok"})
}

// POST /backups/wipe {scope} 远程删档: 先自动远程备份, 再让 hook 删档并重启
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
	// 删档前强制远程备份 (失败则拒绝删档)
	if err := h.createBackupInternal("删档前自动备份"); err != nil {
		c.JSON(http.StatusConflict, gin.H{"error": "删档前自动备份失败, 已中止: " + err.Error()})
		return
	}
	body, _ := json.Marshal(map[string]string{"scope": req.Scope})
	data, code, err := h.hookDo(http.MethodPost, "/saves/wipe", bytes.NewReader(body), int64(len(body)))
	if err != nil || code != http.StatusOK {
		c.JSON(http.StatusBadGateway, gin.H{"error": fmt.Sprintf("删档失败: %v (code=%d) %s", err, code, string(data))})
		return
	}
	h.bs.AuditLog(operator(c), "wipe_remote_"+req.Scope, "", "")
	c.JSON(http.StatusOK, gin.H{"status": "ok", "restarting": true, "note": "删档完成, 服务器正在重启为全新存档"})
}

// POST /backups/restart 远程重启游戏服务器 (hook _exit, MCSM自动拉起)
func (h *BackupHandler) Restart(c *gin.Context) {
	data, code, err := h.hookDo(http.MethodPost, "/saves/restart", nil, -1)
	if err != nil || code != http.StatusOK {
		c.JSON(http.StatusBadGateway, gin.H{"error": fmt.Sprintf("重启失败: %v (code=%d) %s", err, code, string(data))})
		return
	}
	h.bs.AuditLog(operator(c), "restart_remote", "", "")
	c.JSON(http.StatusOK, gin.H{"status": "ok", "restarting": true})
}

// createBackupInternal 内部远程备份 (删档前自动调用)
func (h *BackupHandler) createBackupInternal(note string) error {
	saveDir, files, err := h.hookSaves()
	if err != nil {
		return err
	}
	if len(files) == 0 {
		return fmt.Errorf("远程存档为空")
	}
	bdir := h.backupDir()
	_ = os.MkdirAll(bdir, 0o755)
	name := fmt.Sprintf("save-remote-%s.zip", time.Now().Format("20060102-150405"))
	path := filepath.Join(bdir, name)
	zf, err := os.Create(path)
	if err != nil {
		return err
	}
	zw := zip.NewWriter(zf)
	cc := h.cs.GetConnectionConfig()
	ok := 0
	for _, f := range files {
		rel, _ := f["path"].(string)
		if rel == "" {
			continue
		}
		req, err := http.NewRequest(http.MethodGet, cc.PalHookURL+"/saves/download?path="+rel, nil)
		if err != nil {
			continue
		}
		req.SetBasicAuth("admin", cc.PalHookPassword)
		resp, err := h.big.Do(req)
		if err != nil || resp.StatusCode != http.StatusOK {
			if resp != nil {
				_ = resp.Body.Close()
			}
			continue
		}
		w, err := zw.Create(rel)
		if err == nil {
			_, _ = io.Copy(w, resp.Body)
			ok++
		}
		_ = resp.Body.Close()
	}
	_ = zw.Close()
	_ = zf.Close()
	if ok == 0 {
		_ = os.Remove(path)
		return fmt.Errorf("所有文件下载失败")
	}
	st, _ := os.Stat(path)
	var size int64
	if st != nil {
		size = st.Size()
	}
	_ = h.bs.Add(name, saveDir, note, size)
	return nil
}
