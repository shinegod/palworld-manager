# 致谢

本项目（唤兽 huanshou）的实现大量受益于以下开源项目与社区工作，在此表示衷心感谢。

[English version](ACKNOWLEDGEMENTS.en.md)

> ⚠️ **AI 开发声明**：本项目绝大部分代码由 AI 辅助生成。如果不喜欢，请直接关闭页面，勿喷作者。欢迎建设性反馈。

## 直接参考 / 借鉴

| 项目 | 地址 | 借鉴内容 |
|------|------|----------|
| **PalworldModdingKit** | https://github.com/localcc/PalworldModdingKit | Palworld 的 UE SDK 重建（UHT dump）。公会系统（UPalGroupManager / UPalGroupGuild / FPalGuildPlayerInfo 等）的类结构与字段布局直接来自这里 |
| **AdminCommands** | https://github.com/dkoz/AdminCommands | UE4SS Lua 管理指令 mod。刷道具/刷帕鲁/给经验等管理功能的 UE 函数实现（AddItem_ServerInternal、GiveExpToAroundPlayerCharacter、SpawnNPCForServer 等）与物品 ID 表（palhook/itemdata.json，2466 项）参考自此 |
| **palworld-server-tool** | https://github.com/zaigie/palworld-server-tool | Go + Vue3 的帕鲁服务器管理面板。本面板的前后端架构与页面设计（仪表盘/玩家管理/世界地图/公会管理等）借鉴自此项目 |

## 工具链与框架

### PalHook 开发工具
- **RE-UE4SS** — https://github.com/UE4SS-RE/RE-UE4SS — UE 脚本/反射调试框架，开发早期用它验证 UFunction 调用与对象布局
- **Dumper-7** — https://github.com/Encryqed/Dumper-7 — UE SDK 生成器；PalHook 的反射结构解析（UClass/UFunction/FProperty/FNamePool 布局）思路参考其 SDKGen

### 后端依赖（Go，见 go.mod）
Gin、zerolog、golang-jwt、gin-contrib/cors、modernc.org/sqlite 等

### 前端依赖（见 web/package.json）
Vue 3、Element Plus、Vite、Pinia、Vue Router、ECharts、Leaflet + vue-leaflet、Axios 等

### 测试环境
- **palworld-server-docker** — https://github.com/thijsvanloef/palworld-server-docker — 测试服的 Docker 镜像与部署方案
- **palworld-save-reader** — https://github.com/LukeHollandDev/palworld-save-reader — 存档解析（早期版本使用）

## 特别说明

- PalHook 的进程注入（LD_PRELOAD）、内存扫描与 UE 反射调用均为本项目自行实现（基于对**自有测试服务器**进程的逆向分析），但上述项目的公开资料（偏移、类布局、函数签名）是重要的知识来源。
- 游戏及一切相关素材版权归 **Pocketpair** 所有。本项目仅用于自有服务器管理与学习研究，请遵守游戏服务条款，请勿用于破坏他人游戏体验。
- 若上述致谢有遗漏或不当之处，欢迎提 Issue 指正。
