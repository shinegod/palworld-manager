# 幻兽帕鲁生产环境部署方案（hook-only 架构）

> 面板 (palmanager) + PalHook 注入库，不需要官方 REST API / RCON / Bridge。
> 通用部署文档：把文中的 <占位符> 替换成你自己的环境即可。

---

## 0. 架构总览

```
浏览器 ──HTTPS──> palmanager (8080) ──HTTP/BasicAuth──> PalHook (13335, 仅内网)
                                                        │ LD_PRELOAD 注入
                                                        ▼
                                               PalServer-Linux-Shipping
```

| 组件 | 产物 | 部署位置 |
|------|------|----------|
| PalHook v0.9.5 | `libpalhook.so` (VPS已编译, 可直接下载) | 游戏服: `/workspace/libpalhook.so` |
| 面板 | `bin/palmanager-linux-amd64` (已编译好) | 面板机: `/opt/palmanager/` |

---

## 1. 前置检查（重要）

**PalHook 偏移与游戏版本强绑定**，当前基于 `v1.0.5.102999`（UE 5.1.1）。部署前确认生产服版本：

- 在 MCSM 里看服务器当前游戏版本；若与 `v1.0.5.102999` 不同，先告知我，
  需要重新标定偏移（FNamePool 地址、UClass/UFunction vtable、ProcessEvent 索引）。
- 部署后验证: `curl http://127.0.0.1:13335/health` 里
  `engine_found / console_found / initialized` 必须全为 `true`。

---

## 1.5 MCSM 环境已知坑（重要）

- MCSM 上的旧版 palhook 是 v0.2.0：会注入进每个子进程（13335 被错误进程抢占）、
  用绝对地址硬编码（ASLR 不同 -> 段错误把服务器打崩）。
- 必须使用 **v0.9.5+**：已全部改为 基址+偏移（ASLR 安全）、
  只在 PalServer 进程本体生效、内存只读白名单（无段错误风险）。
- 现成编译产物（**通用版**, 兼容 glibc>=2.14, 即 2011 年后的所有 Linux 发行版）:
  <PalHook编译产物URL或本地路径>
  构建方式: manylinux2014 容器 (CentOS7/glibc2.17) 内编译, 依赖最低只到 GLIBC_2.14
  切勿在 Ubuntu 24.04+ 裸机用默认 gcc 编译 (会绑定 __isoc23_* 符号, 要求 GLIBC_2.38)
  自行编译的命令 (在旧 glibc 环境或容器内):
  gcc -shared -fPIC -O2 -std=gnu17 -o libpalhook.so palhook.c -lpthread -ldl

## 2. PalHook 部署（游戏服）

### 2.1 上传源码并编译（在游戏服机器上，用 gcc 保证 glibc 一致）

```bash
# 把 palhook/palhook.c 传到游戏服
scp palhook/palhook.c user@游戏服:/opt/palhook/
ssh user@游戏服 'cd /opt/palhook && gcc -shared -fPIC -O2 -o libpalhook.so palhook.c -lpthread -ldl'
```

### 2.2 放置到游戏目录

```bash
# 找到 MCSM 里该实例的"文件管理"根目录（形如 /home/mcsm/daemon/.../PalServer）
cp /opt/palhook/libpalhook.so <游戏根目录>/libpalhook.so
chmod 755 <游戏根目录>/libpalhook.so
```

### 2.3 改 MCSM 启动命令（加 LD_PRELOAD）

在 MCSM 实例的 **启动命令** 里，把原来的命令改为（以 steamcmd 官方服为例）：

```bash
LD_PRELOAD="<游戏根目录>/libpalhook.so" ./PalServer.sh -port=8211 -players=32 -useperfthreads -NoAsyncLoadingThread -UseMultithreadForDS
```

> 注意: 用**绝对路径**。已有命令行参数（-publiclobby、-publicport 等）原样保留，
> 只在最前面加 `LD_PRELOAD=...`。

### 2.4 启动并验证（等 60~90 秒初始化）

```bash
curl http://127.0.0.1:13335/health
# {"status":"ok","version":"0.9.4","engine_found":true,"console_found":true,"initialized":true,...}

curl -u admin:<AdminPassword> http://127.0.0.1:13335/players
# {"players":[...]}   在线玩家列表带真实坐标

curl -u admin:<AdminPassword> http://127.0.0.1:13335/guilds
# {"guilds":[...]}    公会+成员列表
```

> **认证密码** = 服务器 AdminPassword。PalHook 启动时自动寻找（环境变量 → 启动目录 →
> `/proc/self/cmdline` → 全内存扫描，支持 base64 编码），无需手动配置；
> 查日志确认 `loaded AdminPassword from ...`。

---

## 3. 面板部署（可与游戏服同机，也可单独一台）

### 3.1 放置二进制

```bash
mkdir -p /opt/palmanager
scp bin/palmanager-linux-amd64 user@面板机:/opt/palmanager/palmanager
chmod +x /opt/palmanager/palmanager
```

### 3.2 systemd 服务（推荐）

```ini
# /etc/systemd/system/palmanager.service
[Unit]
Description=PalManager Panel
After=network.target

[Service]
WorkingDirectory=/opt/palmanager
ExecStart=/opt/palmanager/palmanager -port 8080
Restart=always
RestartSec=5
# 数据库与配置都在 /opt/palmanager/data/ 下，记得备份该目录

[Install]
WantedBy=multi-user.target
```

```bash
systemctl daemon-reload && systemctl enable --now palmanager
```

### 3.3 首次配置（浏览器）

1. 打开 `http://<面板机>:8080` → 初始账号 `admin / admin`（登录后到 设置 里改密码）。
2. **设置 → 连接配置**：
   - PalHook 地址: `http://<游戏服内网IP或127.0.0.1>:13335`
   - PalHook 密码: 服务器 AdminPassword
   - 保存后即时生效，无需重启。
3. 验证: 仪表盘出现 FPS/玩家数、玩家管理有在线列表、世界地图有玩家点位、公会管理有公会+成员。

> 配置存在 `<面板目录>/data/palmanager.db`（SQLite）。迁移面板 = 迁移这个文件。

---

## 4. 网络与安全

| 端口 | 用途 | 建议 |
|------|------|------|
| 8080 | 面板 Web | 绑内网或经 nginx + TLS 反代对外 |
| 13335 | PalHook API（**有管理权！**） | **绝不公网暴露**；游戏服本机/内网访问即可，必要时 iptables 限制来源 |
| 8211/udp | 游戏 | 正常放行 |

nginx 反代示例（可选）：

```nginx
server {
    listen 443 ssl;
    server_name panel.example.com;
    # ssl_certificate ...
    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_set_header Host $host;
        proxy_read_timeout 120s;   # 刷帕鲁等操作单次可达90s
    }
}
```

> PalHook 接口能刷道具/踢人/传送，等同服务器管理权：面板登录密码务必改强密码。

---

## 5. 运维手册

### 5.1 更新 PalHook（改偏移/加功能后）

```bash
# 1. 传新 palhook.c 到游戏服编译
# 2. 替换 .so 并重启游戏 (MCSM 实例重启)
# 3. 等 90s 后验证 /health 的 version 与 initialized
```

### 5.2 更新面板

```bash
# 替换二进制后重启服务（前端已内嵌，不需要单独部署静态文件）
scp bin/palmanager-linux-amd64 user@面板机:/opt/palmanager/palmanager.new
ssh user@面板机 'systemctl stop palmanager && mv /opt/palmanager/palmanager.new /opt/palmanager/palmanager && systemctl start palmanager'
```

### 5.3 备份

- **游戏存档**: `<游戏根目录>/Pal/Saved/` 整个目录（Level.sav / LevelMeta.sav / Players/）。
  用 MCSM 定时备份或 rsync 到异地。
- **面板数据**: `/opt/palmanager/data/palmanager.db`（封禁记录/玩家历史/事件）。

### 5.4 回滚

- PalHook 问题: 把 LD_PRELOAD 从 MCSM 启动命令里去掉重启即可（游戏完全不受影响）。
- 面板问题: 保留旧二进制文件即可回滚。

---

## 6. 验证清单（部署完成后逐项打勾）

- [ ] `/health`: initialized=true, version=0.9.5
- [ ] `/players`: 有玩家时名字/UID/等级/坐标正确（50级以上玩家坐标也正常）
- [ ] `/guilds`: 公会名称/基地等级/营地数/成员(名字+角色+在线状态)正确
- [ ] 面板: 仪表盘 FPS/在线玩家实时刷新
- [ ] 面板: 玩家管理 刷道具/刷帕鲁/角色属性/传送/踢人/封禁 各试一次
- [ ] 面板: 全服公告 + [服务器]聊天 玩家能收到
- [ ] 面板: 世界地图玩家点位跟随移动、公会页成员信息正确
- [ ] 公网 curl http://<公网IP>:13335/health **不通**（确认没暴露）
- [ ] 面板改掉默认密码

---

## 7. 待办

- [ ] 生产服游戏版本确认（与 v1.0.5.102999 对比，不一致先告诉我标定偏移）
- [ ] MCSM 实例创建/迁移存档
- [ ] 面板机选定（建议与游戏服同机，少一层网络）
