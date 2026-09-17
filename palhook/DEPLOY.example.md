# PalHook 部署示例（模板）

> 复制本文件为 DEPLOY.md 并填入你自己的环境信息。
> DEPLOY.md 已在 .gitignore 中，**永远不要提交到公开仓库**。

## 测试/生产环境信息（示例，全部为占位符）

| 项目 | 值 |
|------|-----|
| 服务器IP | <你的服务器IP> |
| SSH用户 | <ssh用户名> |
| SSH密码/密钥 | <登录方式> |
| 游戏端口 | 8211/udp |
| PalHook API端口 | 13335/tcp |
| AdminPassword | <游戏服管理员密码> |
| 面板端口 | 8080 |

## 部署步骤（概要，详见仓库根目录 DEPLOY-PRODUCTION.md）

```bash
# 1. 编译 (注意: 用老 glibc 环境编译成通用版, 见 DEPLOY-PRODUCTION.md)
gcc -shared -fPIC -O2 -std=gnu17 -o libpalhook.so palhook.c -lpthread -ldl

# 2. 放进游戏目录并注入
#    MCSM/Docker 启动命令最前面加: LD_PRELOAD=/绝对路径/libpalhook.so

# 3. 验证
curl http://127.0.0.1:13335/health
# {"status":"ok","version":"0.9.x","initialized":true,...}
```

## 安全提醒

- 13335 端口有管理权限（刷道具/踢人/传送），**绝不能公网暴露**
- PalHook API 认证 = admin:<AdminPassword>（自动从游戏配置发现）
- 面板登录密码务必修改默认值
