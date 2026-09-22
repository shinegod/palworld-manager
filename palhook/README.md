# PalHook — 幻兽帕鲁服务端注入库

> 通过 LD_PRELOAD 注入 PalServer 进程的 C 库，提供 HTTP API 实现完全无感在线管理，面板只连 PalHook 即可。
> ⚠️ 本项目绝大部分代码由 AI 辅助生成，不喜欢请直接关闭，勿喷作者。
> English introduction: [../README.en.md](../README.en.md) / 英文介绍见主 README 英文版。


通过 LD_PRELOAD 注入 PalServer-Linux-Shipping 进程，提供 HTTP API 实现**完全无感**在线管理：刷道具、刷帕鲁(自动捕获)、给经验、设等级、传送、改科技点、全服公告、踢人、在线玩家列表、运行指标，不踢人不重启。**管理面板只连 PalHook 即可，无需官方 REST API/RCON/bridge。**

**版本: v0.9.15** | **物品库: 2466项** | **源码: ~6900行C**

---

## 一、项目文件

```
palhook/
├── palhook.c            # 主源码
├── libpalhook.so        # 编译产物 (Linux x86_64)
├── itemdata.json        # 物品ID数据库 (2466项, 来自AdminCommands mod)
├── docker-compose.yml   # 测试环境Docker配置
└── README.md            # 本文档
```

---

## 二、VPS 测试环境搭建

### 2.1 环境要求

- Linux VPS (Ubuntu/Debian)，推荐 4GB+ RAM
- Docker + Docker Compose 已安装
- 开放端口: 8211/udp (游戏), 8212/tcp (REST API), 25575/tcp (RCON), 13335/tcp (PalHook)

### 2.2 部署 PalServer + PalHook

```bash
# 1. 创建工作目录
mkdir -p /root/palhook-test && cd /root/palhook-test

# 2. 上传文件 (从本地)
# palhook.c, docker-compose.yml

# 3. 编译 PalHook
gcc -shared -fPIC -O2 -o libpalhook.so palhook.c -lpthread -ldl

# 4. 启动 PalServer (首次会下载镜像，约10分钟)
docker compose up -d

# 5. 等待服务器完全启动 (约2-3分钟)
docker logs -f palworld-dev

# 6. 将 libpalhook.so 复制进容器
docker cp libpalhook.so palworld-dev:/palworld/libpalhook.so
```

### 2.3 配置 LD_PRELOAD 注入

PalServer 的启动脚本需要加 `LD_PRELOAD`。有两种方式：

**方式A：修改容器内启动脚本**

```bash
# 进入容器
docker exec -it palworld-dev bash

# 编辑启动脚本 (具体路径看容器镜像)
# 在 PalServer 启动命令前加:
export LD_PRELOAD=/palworld/libpalhook.so
```

**方式B：覆盖入口点 (推荐)**

在 `docker-compose.yml` 同目录创建 `init-override.sh`:

```bash
#!/bin/bash
export LD_PRELOAD=/palworld/libpalhook.so
# 执行原始启动脚本
exec /palworld/原始启动脚本路径
```

### 2.4 重启生效

```bash
docker restart palworld-dev

# 等待约60-90秒PalHook初始化
# 检查状态:
curl http://localhost:13335/health
```

**正常响应:**
```json
{"status":"ok","version":"0.7.0","initialized":true,"engine_found":true}
```

### 2.5 验证流程

```bash
# 1. 检查PalHook是否注入成功
curl http://localhost:13335/health

# 2. 查看所有API (需要认证)
curl -u admin:你的AdminPassword http://localhost:13335/help

# 3. 有玩家在线时，测试刷道具
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"item_id":"Stone","count":99}' \
  http://localhost:13335/give-item
```

---

## 三、日常运维操作

### 3.1 更新 PalHook

```bash
# 1. 本地修改 palhook.c 后编译
gcc -shared -fPIC -O2 -o libpalhook.so palhook.c -lpthread -ldl

# 2. 上传到VPS
scp libpalhook.so root@VPS地址:/root/palhook-test/

# 3. 复制进容器并重启
docker cp /root/palhook-test/libpalhook.so palworld-dev:/palworld/libpalhook.so
docker restart palworld-dev

# 4. 等待初始化 (约60秒)
sleep 60 && curl http://localhost:13335/health
```

### 3.2 查看PalHook日志

```bash
# PalHook日志输出到stderr，跟PalServer日志混在一起
docker logs palworld-dev 2>&1 | grep "\[PalHook\]"

# 实时跟踪
docker logs -f palworld-dev 2>&1 | grep "\[PalHook\]"
```

### 3.3 停止/启动服务器

```bash
docker stop palworld-dev   # 停止
docker start palworld-dev  # 启动
docker restart palworld-dev # 重启
```

---

## 四、认证机制

PalHook 自动寻找服务器的 `AdminPassword` 作为 API 密码，**无需手动配置路径**，每个服务器安装位置不同都能自动适配：

**自动发现顺序:**
1. 环境变量 `PALHOOK_PASSWORD` (最高优先级)
2. 常见路径: /palworld, /workspace, ./Pal 下的 PalWorldSettings.ini
3. 进程工作目录 (getcwd / /proc/self/cwd) 下的 Pal/Saved/Config/LinuxServer/
4. `find` 搜索常见根目录 (/palworld /home /workspace /data /srv /opt /app /server 等, 最深9层)
5. **进程内存扫描**: 游戏启动后配置原文缓存在内存里, 搜索 `AdminPassword="` 模式
   (支持UTF-16 FString和base64编码值自动解码), 游戏启动60秒后重试

**认证方式:** HTTP Basic Auth

```bash
# 格式: -u 用户名:密码 (用户名随意，只校验密码部分)
curl -u admin:你的AdminPassword http://localhost:13335/help
```

**免认证端点:** `/health` (用于监控探测)

**注意:** 密码未找到期间 API 处于无保护状态，找到后自动启用认证。

---

## 五、API 完整文档

### 5.1 基础端点

#### `GET /health` (免认证)

```bash
curl http://localhost:13335/health
```

```json
{
  "status": "ok",
  "version": "0.7.0",
  "pid": 202,
  "initialized": true,
  "engine_found": true,
  "rw_regions": 214,
  "rw_total_mb": 1616
}
```

#### `GET /help`

返回所有可用端点列表。

---

### 5.2 玩家管理

#### `GET /find-players`

自动发现所有在线玩家，返回关键对象地址。

```bash
curl -u admin:密码 http://localhost:13335/find-players
```

```json
{
  "players": [
    {
      "name": "BP_PalPlayerState_C",
      "state": "0x7ed0bfca0dc0",
      "inv": "0x7ed0464d8a00",
      "ctrl": "0x7ed07580ec50"
    }
  ],
  "count": 1
}
```

- `state`: PlayerState 对象地址
- `inv`: InventoryData 对象地址 (背包)
- `ctrl`: PlayerController 对象地址

#### `GET /find-class?name=类名&max=N`

按 UE5 UClass 名称搜索对象实例。支持 Blueprint 类 (BP_xxx_C)，重启后地址变化也能找到。

```bash
curl -u admin:密码 "http://localhost:13335/find-class?name=BP_PalPlayerInventoryData_C&max=5"
```

---

### 5.3 刷道具 ✅

#### `POST /give-item`

```bash
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"item_id":"PalSphere","count":50}' \
  http://localhost:13335/give-item
```

**参数:**
| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| item_id | string | 是 | 物品静态ID，见物品列表 |
| count | int | 否 | 数量，默认1，最大9999 |
| name | string | 否 | 指定玩家名字 (通过PlayerState::GetInventoryData定位背包) |
| inv | string | 否 | InventoryData地址，不传自动查找第一个在线玩家 |

**响应:**
```json
{
  "status": "success",
  "item_id": "PalSphere",
  "fname_idx": 53546,
  "count": 50,
  "inv": "0x7ed0464d8a00",
  "return_value": 0,
  "wait_ms": 10
}
```

**技术原理:** FNamePool查找物品FName索引 → UClass Children链找 AddItem_ServerInternal → 构造24字节参数 → GameThread ProcessEvent

---

### 5.4 传送 ✅ (v0.9.2 游戏原生传送, 不掉血)

#### `POST /teleport`

```bash
# 坐标传送
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"x":-45000,"y":-140000,"z":3500}' \
  http://localhost:13335/teleport

# 传送到某玩家身边 (自动取对方实时坐标+300偏移防重叠)
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"to":"玩家名字"}' \
  http://localhost:13335/teleport

# 把A玩家传到B玩家身边
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"name":"A","to":"B"}' \
  http://localhost:13335/teleport
```

**参数:**
| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| x/y/z | float | 坐标模式 | UE5世界坐标 |
| to | string | 玩家模式 | 目标玩家名字 (实时取坐标+偏移) |
| name | string | 否 | 移动哪个玩家 (默认第一个在线玩家) |

**实现: PalUtility::Teleport(Target, Location, Rotation, bNoCheck=false, bAroundCheck=true)**
- 游戏原生传送函数, bAroundCheck会检查周边找有效落脚点
- 旧版K2_TeleportTo会从高空坠落掉血, 已弃用

---

### 5.5 科技点 ✅

#### `POST /set-tech-points`

```bash
# 设置普通科技点100，古代科技点50
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"tech":100,"boss_tech":50}' \
  http://localhost:13335/set-tech-points
```

**参数:**
| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| tech | int | 否 | 普通科技点 (设置为具体值，不是增加) |
| boss_tech | int | 否 | 古代科技点 (Boss掉落获取) |

至少传一个。

**响应:**
```json
{
  "status": "success",
  "tech": {"old": 7, "new": 100},
  "boss_tech": {"old": 0, "new": 50}
}
```

**技术原理:** 找 PlayerState+0x648 → PalTechnologyData，直接写内存 +0x150 (普通) / +0x154 (古代)。下次AutoSave(30秒)自动持久化。

---

### 5.6 金币 ✅

**钱包金币 = "Money"(Gold Coin)道具的堆叠数**, 通过刷Money道具实现 (实测钱包99->100):

```bash
# 加金币 (支持name指定玩家, amount数量)
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"amount":9999,"name":"玩家名"}' \
  http://localhost:13335/give-money

# 或直接刷Money道具
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"item_id":"Money","count":9999}' \
  http://localhost:13335/give-item
```

**坑: 旧版Debug_AddMoney_ToServer在新版游戏(v1.0.5)已被移除, 不要用。物品ID是Money不是Coin。**

---

### 5.7 给经验 ✅

#### `POST /give-exp`

```bash
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"exp":100000}' \
  http://localhost:13335/give-exp
```

**技术原理 (对照 AdminCommands mod):**
1. 找到玩家 Character (BP_Player_Female_C/Male_C, vtable验证)
2. K2_GetActorLocation 获取玩家真实坐标 (备用: RootComponent→RelativeLocation内存直读)
3. Outer链找 UWorld (Character→Level→World)
4. PalUtility CDO 上调 GiveExpToAroundPlayerCharacter(WorldContextObject, Center, Radius=1000, Exp, bCallDelegate=true)

**关键坑: Exp参数是float不是int32!** SDK签名 `static void GiveExpToAroundPlayerCharacter(..., float Exp, ...)`
写成int32会把位模式当float读≈0。已实测: 50万经验 7级→35级。

**响应示例:**
```json
{"status":"success","exp":100000,"world":"0x...","loc":[-96339.2,-8946.3,16.4],"waited_ms":30}
```

---

### 5.7b 设置等级/经验 ✅

#### `POST /set-level` / `POST /set-exp`

```bash
# 直接设置玩家等级
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"level":30}' \
  http://localhost:13335/set-level

# 或直接写经验值
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"exp":500000}' \
  http://localhost:13335/set-exp
```

**实测内存字段 (IndividualParameter对象):**
| 偏移 | 类型 | 含义 |
|------|------|------|
| +0x3F0 | uint8 (低字节) | 玩家等级 (与REST API一致) |
| +0x3F8 | int64 | 经验值 (给1经验精确+1) |

- 定位链: Character → CharacterParameterComponent属性(off=1584) → IndividualParameter属性(off=376)
- 等级只升不降: 游戏AddExp只做 max(旧等级, curve(exp)), 不会回退
- set-level同时写经验曲线近似值 T(L)≈0.08·L^4.39 (刻意低估, 保证后续AddExp不跳级)

---

### 5.8 刷帕鲁 ✅ (含自动捕获)

#### `POST /spawn-pal`

```bash
# 在玩家旁边刷一只50级阿努比斯
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"pal_id":"Anubis","level":50,"capture":0}' \
  http://localhost:13335/spawn-pal

# 刷出并自动捕获进玩家背包 (capture=1)
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"pal_id":"SheepBall","level":10,"capture":1}' \
  http://localhost:13335/spawn-pal
```

**参数:**
| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| pal_id | string | 是 | 帕鲁CharacterID (如 Anubis, CuteFox, SheepBall) |
| level | int | 否 | 等级1-100，默认1 |
| capture | int | 否 | 1=刷出后自动捕获进玩家背包 |
| name | string | 否 | 指定刷在哪个玩家身边 |
| x/y/z | float | 否 | 指定刷出坐标，默认玩家旁+300,+100 |

**技术原理 (对照 AdminCommands spawn.lua):**
1. PalUtility CDO → GetNPCManager(World) → UNPCManager
2. NPCManager 读属性 NPCAIControllerBaseClass
3. 构造 PalNPCSpawnInfo{ControllerClass, CharacterID(FName), Level, Location, Yaw, Squad}
4. SpawnNPCForServer(SpawnInfo, nil) → SpawnHandle
5. capture=1: 轮询 handle:TryGetIndividualActor() → PalUtility:PalCaptureSuccess(Character, Actor)
6. capture=0 (野生): 帕鲁AI激活 (palai.activate等价):
   SetUpDelegate → SetActiveActor(true) → 控制器SetActiveAI(true) → ReceivePossess
   → SetAutoDefaultAIAction → StartDefaultAIAction → OnPostSpawned
   (无控制器等500ms重试, 已验证野生帕鲁正常出现和行动)

**任意帕鲁ID支持:** FNamePool中没有的ID (未加载的数据) 会通过
KismetStringLibrary::Conv_StringToName 动态intern (UTF-16 FString)。
注意: UE的FName大小写不敏感，池里已有其他大小写条目时会复用已有索引。

**响应示例:**
```json
{"status":"success","pal_id":"SheepBall","level":12,"capture":1,"captured":1,"handle":"0x..."}
```

---

### 5.8b 面板数据接口 ✅ (v0.9.0)

#### `GET /players` — 在线玩家列表

```json
{"players":[{"name":"Shine_clown","uid":"44CD7CE3...","level":48,"exp":1950444,
 "x":-208505.0,"y":8651.0,"z":14002.6,"character":"0x...","playerstate":"0x..."}],"count":1}
```

包含名字/UID/等级/经验/坐标/对象地址。20秒TTL缓存，中文玩家名正确转码。

#### `POST /announce` — 全服公告 (支持中文)

```bash
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"message":"服务器将在10分钟后重启"}' \
  http://localhost:13335/announce
```

实现: PalGameStateInGame::BroadcastServerNotice (NetMulticast RPC)。
**注意: FString参数按值传16字节(Data*,Num,Max), UTF-16LE编码, 中文需UTF-8→UTF-16转换。**

#### `POST /kick` — 踢出玩家

```bash
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"name":"Shine_clown"}' \
  http://localhost:13335/kick
```

按name/uid/character定位玩家 → ClientTravelInternal("Void")。

#### `POST /chat` — 服务器聊天 (v0.9.1 已验证)

```bash
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"message":"大家好","sender":"服务器"}' \
  http://localhost:13335/chat
```

- sender可选, 默认"服务器"
- **游戏内显示效果: `[服务器]: 消息内容`** (实测确认)
- 实现: GameState::BroadcastChatMessage (NetMulticast Reliable, 免内容过滤直发)
- FPalChatMessage{Category=Global(1), Sender, SenderPlayerUId=全零, Message, ReceiverPlayerUIds=在线玩家}
- 消息文本不加任何前缀 (客户端的[服务器]由Sender字段自动渲染)

**SenderPlayerUId实测结论 (重要):**
| UID | 效果 |
|-----|------|
| 全零 | ✅ 客户端回退Sender字段, 显示 [服务器]: 消息 |
| 全0xFF | 显示 [-----] (占位符) |
| 玩家自己的GUID | 显示玩家昵称 |

**坑位记录:** EnterChat_Receive走异步内容过滤管道(过滤服务不可达时消息被丢弃);
SendSystemToPlayerChat在专用服务器不投递。都不要用。

#### `POST /ban` — 封禁玩家

同/kick机制 (踢出+面板本地封禁记录), 参数相同。

#### `GET /metrics` — 运行指标

```json
{"status":"ok","player_count":1,"fps":60,"uptime_sec":242,"version":"0.9.0"}
```

fps由nanosleep hook每秒结算(每帧一次调用≈服务器帧率)。

#### `GET /guilds` — 公会列表 (含成员, v0.9.3)

从服务器内存实时读取公会与成员信息 (不需要存档解析/RCON)。

```json
{
  "guilds": [
    {
      "id": "694002D1E0F5438D815B8B0FB04D9C5E",
      "name": "111",
      "level": 2,
      "basecamp_count": 1,
      "admin_uid": "44CD7CE3000000000000000000000000",
      "member_count": 1,
      "members": [
        {
          "uid": "44CD7CE3000000000000000000000000",
          "name": "Shine_clown",
          "role": 1,
          "role_name": "GuildMaster",
          "status": 1,
          "last_online_unix": 0
        }
      ]
    }
  ],
  "count": 1,
  "source": "scan+map"
}
```

- `role`: 0=None 1=GuildMaster(会长) 2=SubMaster(副会) 3=Member 4=Guest
- `status`: 1=在线 0=离线
- 实现: 按具体类名(PalGroupGuild/PalGroupIndependentGuild)扫描实例 + 类链/ID/字段校验过滤废弃对象,
  成员通过 PlayerInfoRepInfoArray 快数组反射解析 (含离线成员名字/角色/最后在线时间)

---

### 5.9 底层调试端点

#### `GET /readmem?addr=0x...&count=N`

读取进程内存，返回N个qword (8字节) 的值。

```bash
curl -u admin:密码 "http://localhost:13335/readmem?addr=0x7ed0bfca0dc0&count=4"
```

#### `GET /fname?idx=N`

FName索引转字符串。

```bash
curl -u admin:密码 "http://localhost:13335/fname?idx=5840283"
# {"index":5840283,"name":"Stone"}
```

#### `GET /search-bytes?hex=...&max=N`

在所有rw内存段搜索字节模式。

```bash
curl -u admin:密码 "http://localhost:13335/search-bytes?hex=50616c53706865726500&max=3"
```

#### `POST /call-function`

调用任意UFunction (高级用法)。

```bash
curl -u admin:密码 -X POST -H "Content-Type: application/json" \
  -d '{"obj":"0x...", "func":"函数名", "hex_params":"参数hex"}' \
  http://localhost:13335/call-function
```

---

## 六、物品ID列表

完整2466项见 `itemdata.json`。以下是常用物品：

### 帕鲁球
| ID | 名称 |
|----|------|
| PalSphere | 帕鲁球 |
| PalSphere_Mega | 超级球 |
| PalSphere_Giga | 高级球 |
| PalSphere_Tera | 终极球 |
| PalSphere_Master | 大师球 |
| PalSphere_Legend | 传说球 |
| PalSphere_Ultimate | 至尊球 |
| PalSphere_Exotic | 异域球 |
| PalSphere_Ancient_1 | 日轮球 |
| PalSphere_Ancient_2 | 古代球 |

### 弹药
| ID | 名称 |
|----|------|
| Arrow | 箭矢 |
| Arrow_Fire | 火箭矢 |
| Arrow_Poison | 毒箭矢 |
| HandgunBullet | 手枪弹药 |
| RifleBullet | 步枪弹药 |
| ShotgunBullet | 霰弹 |
| AssaultRifleBullet | 突击步枪弹药 |

### 基础素材
| ID | 名称 |
|----|------|
| Stone | 石头 |
| Wood | 木头 |
| Fiber | 纤维 |
| Leather | 皮革 |
| Cloth | 布 |
| Coal | 煤 |
| Sulfur | 硫磺 |
| Charcoal | 木炭 |
| Horn | 角 |

### 金属
| ID | 名称 |
|----|------|
| CopperIngot | 铜锭 |
| IronIngot | 铁锭 |
| Gold | 金 |
| Silver | 银 |
| Diamond | 钻石 |
| Ruby | 红宝石 |
| Sapphire | 蓝宝石 |

### 高级素材
| ID | 名称 |
|----|------|
| Cement | 水泥 |
| CarbonFiber | 碳纤维 |
| Plastic | 塑料 |
| Blueprint | 蓝图 |

### 食物
| ID | 名称 |
|----|------|
| Berry | 浆果 |
| Cake | 蛋糕 |
| Pizza | 披萨 |
| Salad | 沙拉 |
| Stew | 炖菜 |

### 武器
| ID | 名称 |
|----|------|
| StonePickaxe | 石镐 |
| Torch | 火把 |
| Sword | 剑 |
| Spear | 矛 |
| AssaultRifle | 突击步枪 |

### 货币/特殊
| ID | 名称 |
|----|------|
| Coin | 金币 |
| Money | 钱 |
| TechnologyPoint | 科技点 (背包道具，非直接加点) |

---

## 七、技术原理

### 7.1 注入方式

通过 `LD_PRELOAD` 机制在 PalServer 进程启动时加载 `libpalhook.so`。使用 `__attribute__((constructor))` 在 .so 加载时自动执行初始化代码。

### 7.2 UE5 对象发现

PalServer 是 **非PIE (Position Independent Executable)** 的 EXEC 二进制，符号地址就是绝对运行时地址。

1. **FNamePool**: 搜索内存中 "None"+"ByteProperty" 字符串模式，反推 FNamePool 全局变量地址
2. **GEngine**: 通过 `_ZTV14UPalGameEngine` vtable 地址 (0x1a20db8+16) 扫描所有 rw-p 段
3. **Blueprint 类**: 不依赖 vtable (每次重启变化)，通过 UClass+0x18 的 FName 匹配

### 7.3 GameThread 调度

UE5 的 ProcessEvent 必须在 GameThread 调用。PalHook 通过 hook `nanosleep` 实现:

```
HTTP请求线程 → enqueue_cmd(obj, func, params)
                     ↓
GameThread (nanosleep hook) → process_cmd_queue() → ProcessEvent(obj, func, params)
                     ↓
HTTP请求线程 ← 等待 cmd.done 标志
```

UE5 的 `FPlatformProcess::Sleep` 每帧调用 nanosleep，所以命令会在下一帧执行。

### 7.4 UFunction 反射

```
UClass (+0x48 Children) → UField 链表 → 找到 vtable==0x1a51218 的是 UFunction
UFunction (+0x18 FName) → 函数名
UFunction (+0x50 ChildProperties) → FProperty 链表 → 参数信息
UFunction (+0x58) → ParmsSize
UClass (+0x40 SuperStruct) → 父类，递归查找继承的函数
```

### 7.5 FProperty 布局 (UE5.1)

```
FProperty:
  +0x00: FFieldClass*
  +0x08: 类型信息
  +0x10: Owner (UFunction*)
  +0x18: Flags
  +0x20: Next (FProperty*)
  +0x28: NamePrivate (FName)
  +0x38: ElementSize (int32)
  +0x48: 高32位 = Offset_Internal (参数在buffer中的偏移)
```

### 7.6 关键内存偏移

| 对象 | 偏移 | 含义 |
|------|------|------|
| UObject+0x00 | vtable | 虚函数表 |
| UObject+0x10 | ClassPrivate | UClass指针 |
| UObject+0x18 | FName | 对象名 |
| UObject+0x20 | OuterPrivate | 外层对象 |
| UClass+0x18 | FName | 类名 |
| UClass+0x40 | SuperStruct | 父类 |
| UClass+0x48 | Children | UField链表头 |
| UClass+0x50 | ChildProperties | FProperty链表头 |
| PlayerState+0x648 | TechnologyData | PalTechnologyData指针 |
| TechnologyData+0x150 | TechnologyPoint | 普通科技点 (int32) |
| TechnologyData+0x154 | bossTechnologyPoint | 古代科技点 (int32) |
| ProcessEvent | vtable[77] | 偏移0x268 |
| UFunction vtable | 0x1a51218 | 用于识别UFunction对象 |
| FNamePool | BSS ~0xc0b24b0 | Blocks数组在+0x10 |

### 7.7 AddItem_ServerInternal 参数结构

```
ParmsSize = 24 字节

偏移  大小  类型    名称             说明
0x00  8     FName   StaticItemId     物品ID (uint32 index + uint32 number=0)
0x08  4     int32   Count            数量
0x0C  1     bool    IsAssignPassive  false
0x10  4     float   LogDelay         0.0
0x14  1     bool    bNotifyLog       true
0x15  1     bool    ReturnValue      输出参数
```

---

## 八、开发计划

### 已完成 ✅

- [x] 公会管理 — /guilds 扫描具体类+反射解析快数组, 含成员/角色/在线状态 (v0.9.3)
- [x] /guilds 8秒TTL结果缓存 (冷扫7s -> 命中0.5s) (v0.9.4)
- [x] ASLR安全: 全部硬编码地址改为基址+偏移, 只在PalServer进程生效 (v0.9.5)
- [x] 通用编译: manylinux2014/glibc>=2.14 产物, 兼容所有Linux发行版 (v0.9.5)
- [x] LD_PRELOAD 注入框架 + 自动初始化
- [x] UE5 对象发现 (GEngine, FNamePool, UClass/UFunction 反射)
- [x] ProcessEvent GameThread 调度 (nanosleep hook)
- [x] 刷道具 — AddItem_ServerInternal, 自动查找背包
- [x] 传送 — K2_TeleportTo on Character
- [x] 科技点 — 直接写内存 (普通+古代)
- [x] 金币 — 刷 Coin 道具
- [x] 玩家自动发现 — vtable+UClass双重验证 (修复假对象误匹配)
- [x] Basic Auth 认证 — 自动读 AdminPassword
- [x] 物品ID数据库 — 2466项
- [x] **给经验** — GiveExpToAroundPlayerCharacter (真实坐标+World)
- [x] **刷帕鲁** — NPCManager::SpawnNPCForServer (SpawnInfo反射构造)
- [x] **自动捕获** — TryGetIndividualActor + PalCaptureSuccess
- [x] **任意帕鲁ID** — Conv_StringToName 动态intern未加载的FName
- [x] **性能优化** — FName索引缓存 + 全局对象缓存 (20秒扫描→30ms)
- [x] **前端集成** — Vue3「唤兽控制台」页面 + Go代理 /api/palhook/*

### 待开发 🔧

- [x] **等级修改**: Level@IndividualParameter+0x3F0低字节, Exp@+0x3F8 int64 (/set-level)
- [ ] **多玩家支持**: /give-item 等接口支持指定玩家 (传inv/ctrl参数已支持, 需前端玩家选择器)
- [x] **帕鲁AI激活**: SetUpDelegate+SetActiveActor+SetActiveAI+ReceivePossess等 (实测野生帕鲁可见)
- [ ] **物品分类**: itemdata.json 按类型分组 (武器/素材/食物/球等)
- [ ] **MCSM 生产部署**: 部署到正式服务器

### v0.8.0 关键技术突破记录

1. **假对象问题**: 内存扫描只比对类名会匹配到「存了角色指针的垃圾结构」。
   修复: 验证对象vtable ∈ rodata段 + 验证ClassPrivate的vtable是UClass (原生0x1a50e08 / BP生成0x204f060)
2. **属性查找**: UClass的ChildProperties只含本类声明的属性, 必须沿SuperStruct链向上找
3. **FStructProperty**: 本构建FProperty基址有+0x10偏移, FStructProperty的UScriptStruct*在+0x78 (运行时探测)
4. **FName大小写不敏感**: 池里"sheepball"存在时intern "SheepBall"返回已有索引
5. **FString编码**: 本构建TCHAR=UTF-16 (2字节), 不是4字节

### 参考资源

- AdminCommands mod (UE4SS Lua): https://github.com/dkoz/AdminCommands
- UE4SS Linux for Palworld: https://github.com/BlackBookOfficial/ue4ss-linux-palworld
- palworld-save-tools: https://pypi.org/project/palworld-save-tools
- Palworld Save Pal: https://github.com/oMaN-Rod/palworld-save-pal

---

## 九、常见问题

### Q: PalHook 初始化失败 (initialized=false)

A: PalServer 需要完全启动后 PalHook 才能扫描到 GEngine。通常等待60-90秒。检查日志:
```bash
docker logs palworld-dev 2>&1 | grep "\[PalHook\]"
```

### Q: /give-item 返回 "item not found in FNamePool"

A: 物品ID大小写敏感。用 `itemdata.json` 查正确的ID。例如 `CopperIngot` 不是 `copperingot`。

### Q: 重启后科技点会还原吗？

A: 不会。PalServer 默认30秒自动存档 (AutoSaveSpan=30)，写内存后30秒内就持久化了。刷道具用的 AddItem_ServerInternal 是UE原生函数，自带存档标记。

### Q: 能支持多玩家吗？

A: 当前 /give-item 不传 inv 时给第一个在线玩家。传 inv 参数可以指定玩家。通过 /find-players 获取各玩家的 inv 地址。

### Q: PalHook 会被反作弊检测吗？

A: PalHook 运行在服务端进程内，客户端无感知。PalServer 没有服务端反作弊机制。

### Q: 怎么知道物品给成功了？

A: API 返回 `"status":"success"` 且 `"return_value":0` 表示成功。部分物品 return_value 非0 但仍然成功（如 Coin 返回15是正常的）。
