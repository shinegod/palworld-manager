# huanshou (唤兽) — 幻兽帕鲁服务器管理面板

[English Introduction](README.en.md)

类 PalSupervisor/uMod 级别的帕鲁服务器管理工具：可视化面板 + 服务端进程注入，实现**完全无感**的在线管理（不踢人、不重启）。

> ⚠️ **AI 开发声明**：本项目绝大部分代码由 AI 辅助生成，需求、架构、测试与部署由作者本人完成。**如果不喜欢，请直接关闭页面，勿喷作者**。欢迎提出建设性的 Issue 和 PR。
>
> ⚠️ 仅限在**自己拥有/运营**的服务器上使用。

## 组件架构

| 组件 | 路径 | 说明 |
|------|------|------|
| palmanager | `cmd/palmanager` + `internal/` | Go Gin 后端 + 内嵌 Vue3 前端 (端口 8080) |
| palhook | `palhook/` | C 注入库，LD_PRELOAD 注入 PalServer 进程 (端口 13335) |
| web | `web/` | Vue3 + Element Plus + TypeScript 前端源码 |

```
浏览器 ──> palmanager (8080) ──HTTP/BasicAuth──> palhook (13335) ──> 全部功能在 PalServer 进程内完成
```

**hook-only 架构：面板只连 PalHook**，官方 REST API / RCON / Bridge 已全部移除；玩家列表/指标/公告/聊天/踢人/封禁/刷道具/刷帕鲁/经验/等级/传送/科技点/金币/公会管理全部由 PalHook 进程内完成。

## 功能（PalHook v0.9.5）

| 功能 | 说明 |
|------|------|
| 刷道具 | 2466 种物品，中英文对照搜索，支持指定玩家 |
| 刷帕鲁 | 任意帕鲁 + 指定等级 + 野生/自动捕获，AI 完整激活 |
| 角色属性 | 经验/等级/金币/科技点（普通+古代），支持指定玩家 |
| 传送 | 坐标传送 + 玩家互传，防掉落伤害 |
| 玩家管理 | 在线列表（名字/UID/等级/经验/坐标）、踢人、封禁 |
| 全服公告 | 支持中文 |
| 服务器聊天 | `[服务器]: 消息` 免过滤直发 |
| 公会管理 | 公会+成员（名字/角色/在线状态/最后在线），内存实时读取 |
| 运行指标 | 玩家数 / FPS / 运行时间 |

## 面板页面

仪表盘 / 玩家管理 / 世界地图 / 公会管理 / 事件系统 / 反作弊 / 设置 / 备份管理 / 监控中心

## 兼容性

- 游戏版本：**v1.0.5.102999**（UE 5.1.1）。其他版本需重新标定偏移（见 palhook/README.md 偏移表）
- 通用编译产物兼容 glibc ≥ 2.14（2011 年后的所有 Linux 发行版）

## 构建

```bash
# 面板 (前端自动构建并嵌入)
make build          # macOS
make build-linux    # Linux amd64, 产出 bin/palmanager-linux-amd64

# PalHook 注入库 (在 Linux 上编译)
cd palhook
gcc -shared -fPIC -O2 -std=gnu17 -o libpalhook.so palhook.c -lpthread -ldl
```

> ⚠️ **PalHook 编译注意**：不要在 Ubuntu 24.04+ 上用默认参数直接编译（gcc 15 默认 C23，会绑定 `__isoc23_*` 符号要求 GLIBC_2.38）。通用做法：在 manylinux2014 容器（CentOS 7 / glibc 2.17）里编译，产物依赖最低只到 GLIBC_2.14，所有 Linux 服务器都能加载：
>
> ```bash
> docker run --rm -v $(pwd):/build quay.io/pypa/manylinux2014_x86_64 \
>   bash -c "/opt/rh/devtoolset-10/root/usr/bin/gcc -shared -fPIC -O2 -std=gnu17 \
>   -o /build/libpalhook.so /build/palhook.c -lpthread -ldl"
> ```

## 部署

完整方案见 **[DEPLOY-PRODUCTION.md](DEPLOY-PRODUCTION.md)**（PalHook 注入 + 面板 systemd + 安全 + 运维手册 + 验证清单）。

三步速览：

1. **PalHook**：编译 `libpalhook.so` 放进游戏目录，启动命令加 `LD_PRELOAD=/绝对路径/libpalhook.so`（MCSM/Docker 同理），自动发现 AdminPassword。
2. **面板**：运行 `bin/palmanager-linux-amd64 -port 8080`，浏览器打开 `http://<面板机>:8080`，首次登录 `admin/admin`，在 **设置 → 连接配置** 填 PalHook 地址。
3. **安全**：PalHook 13335 端口**绝不公网暴露**；面板改强密码，建议 nginx + HTTPS 反代。

## 目录结构

```
├── cmd/palmanager/        # 面板入口 (路由注册)
├── internal/
│   ├── handler/           # HTTP 处理器 (PalHook 代理 + 玩家/仪表盘/事件)
│   └── store/             # SQLite (配置/玩家历史/封禁)
├── web/                   # Vue3 前端 (src/ 源码, public/ 静态资源)
├── palhook/
│   ├── palhook.c          # 注入库全部源码 (~6000 行 C)
│   ├── itemdata.json      # 物品库 (2466 项)
│   ├── README.md          # 技术文档: API/原理/偏移表/FAQ
│   └── DEPLOY.example.md  # 部署信息模板 (复制为 DEPLOY.md 填自己的, 已 gitignore)
├── configs/               # 配置示例
├── Makefile
├── DEPLOY-PRODUCTION.md   # 生产部署方案
└── ACKNOWLEDGEMENTS.md    # 致谢
```

## 文档索引

- [README.en.md](README.en.md) — English Introduction / 英文介绍
- [DEPLOY-PRODUCTION.md](DEPLOY-PRODUCTION.md) — 生产环境部署方案
- [palhook/README.md](palhook/README.md) — PalHook 技术文档（API/原理/偏移表/FAQ）
- [palhook/DEPLOY.example.md](palhook/DEPLOY.example.md) — 部署信息模板
- [configs/palmanager.example.yaml](configs/palmanager.example.yaml) — 面板配置示例
- [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md) — 致谢（借鉴的开源项目）

## 致谢

本项目借鉴了 [PalworldModdingKit](https://github.com/localcc/PalworldModdingKit)（类结构/字段布局）、[AdminCommands](https://github.com/dkoz/AdminCommands)（管理功能实现与物品表）、[palworld-server-tool](https://github.com/zaigie/palworld-server-tool)（面板架构与页面设计）等开源项目，完整名单见 **[ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md)**。在此向所有相关开发者致谢！

## 说明

- 地图瓦片（`web/public/map/tiles/`，约 121MB）已 gitignore；没有它地图页无底图，但玩家点位/Boss塔/快速旅行点仍显示。
- `web/dist/` 为构建产物，由 `make build` 自动生成（内嵌进 Go 二进制），无需提交。
- 游戏及一切素材版权归 Pocketpair 所有。本项目仅用于自有服务器管理与学习，请遵守游戏服务条款。
