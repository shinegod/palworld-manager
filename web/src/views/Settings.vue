<template>
  <div class="settings-page">
    <h2 class="page-title">设置</h2>

    <el-tabs v-model="activeTab" class="settings-tabs">
      <el-tab-pane label="连接配置" name="connection">
        <el-card shadow="never" class="config-card">
          <template #header>
            <div class="card-header">
              <span>服务器连接</span>
              <el-tag :type="configured ? 'success' : 'warning'" size="small">
                {{ configured ? '已配置' : '未配置' }}
              </el-tag>
            </div>
          </template>

          <el-form :model="connForm" label-width="180px" label-position="left">
            <el-divider content-position="left">Palworld REST API</el-divider>
            <el-form-item label="API 地址" required>
              <el-input v-model="connForm.palworld_api_url" placeholder="http://your-server:8212" />
            </el-form-item>
            <el-form-item label="API 用户名">
              <el-input v-model="connForm.palworld_user" placeholder="admin" />
            </el-form-item>
            <el-form-item label="API 密码">
              <el-input v-model="connForm.palworld_pass" type="password" show-password placeholder="服务器管理员密码" />
            </el-form-item>
            <el-form-item label="GameData API">
              <el-switch v-model="connForm.gamedata_enabled" />
              <span class="form-hint">服务器启动参数包含 -enable-gamedata-api 时启用</span>
            </el-form-item>

            <el-divider content-position="left">PalHook (唯一数据源)</el-divider>
            <el-form-item label="PalHook 地址">
              <el-input v-model="connForm.palhook_url" placeholder="http://your-server:13335" />
              <span class="form-hint">注入库 HTTP API 地址，保存后即时生效无需重启</span>
            </el-form-item>
            <el-form-item label="PalHook 密码">
              <el-input v-model="connForm.palhook_password" type="password" show-password placeholder="服务器 AdminPassword" />
            </el-form-item>

            <el-divider content-position="left">面板管理</el-divider>
            <el-form-item label="管理员用户名">
              <el-input v-model="connForm.admin_user" placeholder="admin" />
            </el-form-item>
            <el-form-item label="管理员密码" required>
              <el-input v-model="connForm.admin_pass" type="password" show-password placeholder="面板登录密码" />
            </el-form-item>
            <el-form-item label="监听端口">
              <el-input-number v-model="connForm.listen_port" :min="1" :max="65535" />
            </el-form-item>

            <el-form-item>
              <el-button type="primary" @click="saveConnection" :loading="saving">保存并重启生效</el-button>
              <el-button @click="testConnection" :loading="testing">测试连接</el-button>
            </el-form-item>
          </el-form>

          <el-alert v-if="saveMessage" :type="saveMessageType" :closable="true" style="margin-top: 12px" @close="saveMessage = ''">
            {{ saveMessage }}
          </el-alert>
        </el-card>
      </el-tab-pane>

      <el-tab-pane label="游戏设置" name="game">
        <div style="margin-bottom: 12px">
          <el-input v-model="search" placeholder="搜索设置项..." prefix-icon="Search" clearable class="search-input" />
        </div>

        <el-skeleton :loading="gameLoading" :rows="10" animated>
          <el-collapse v-model="activeGroups" class="settings-collapse">
            <el-collapse-item v-for="group in filteredGroups" :key="group.name" :title="group.name" :name="group.name">
              <template #title>
                <span class="group-title">{{ group.name }}</span>
                <el-tag size="small" type="info" class="group-count">{{ group.items.length }}</el-tag>
              </template>
              <el-table :data="group.items ?? []" stripe style="width: 100%" size="small">
                <el-table-column prop="key" label="设置项" min-width="280">
                  <template #default="{ row }"><span class="setting-key">{{ row.key }}</span></template>
                </el-table-column>
                <el-table-column prop="value" label="值" min-width="200">
                  <template #default="{ row }">
                    <el-tag v-if="typeof row.value === 'boolean'" :type="row.value ? 'success' : 'danger'" size="small">{{ row.value }}</el-tag>
                    <span v-else class="setting-value">{{ formatValue(row.value) }}</span>
                  </template>
                </el-table-column>
              </el-table>
            </el-collapse-item>
          </el-collapse>
        </el-skeleton>
        <el-empty v-if="!gameLoading && filteredGroups.length === 0" description="未找到设置项" />
      </el-tab-pane>
    </el-tabs>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive, computed, onMounted } from 'vue'
import { ElMessage } from 'element-plus'
import http, { serverApi } from '@/api'

const activeTab = ref('connection')
const configured = ref(false)
const saving = ref(false)
const testing = ref(false)
const saveMessage = ref('')
const saveMessageType = ref<'success' | 'error' | 'info'>('info')

const connForm = reactive({
  palworld_api_url: '',
  palworld_user: 'admin',
  palworld_pass: '',
  rcon_host: '127.0.0.1',
  rcon_port: 25575,
  rcon_pass: '',
  rcon_enabled: true,
  gamedata_enabled: false,
  bridge_url: '',
  bridge_token: '',
  palhook_url: '',
  palhook_password: '',
  admin_user: 'admin',
  admin_pass: '',
  jwt_secret: '',
  listen_port: 8080,
})

async function loadConnectionConfig() {
  try {
    const res = await http.get('/config/connection')
    const data = res.data as any
    configured.value = data.configured
    if (data.config) {
      Object.assign(connForm, data.config)
    }
  } catch { /* first time, no config */ }
}

async function saveConnection() {
  if (!connForm.palworld_api_url && !connForm.palhook_url) {
    ElMessage.warning('PalHook 地址和 API 地址至少填一个 (推荐只填 PalHook)')
    return
  }
  if (!connForm.admin_pass) {
    ElMessage.warning('管理员密码不能为空')
    return
  }
  saving.value = true
  try {
    const res = await http.post('/config/connection', connForm)
    saveMessage.value = (res.data as any).message || '已保存'
    saveMessageType.value = 'success'
    configured.value = true
    ElMessage.success('配置已保存，重启 PalManager 后生效')
  } catch (e: any) {
    saveMessage.value = e.response?.data?.error || '保存失败'
    saveMessageType.value = 'error'
  } finally {
    saving.value = false
  }
}

async function testConnection() {
  if (!connForm.palworld_api_url && !connForm.palhook_url) {
    ElMessage.warning('请先输入 PalHook 或 API 地址')
    return
  }
  testing.value = true
  try {
    // 优先测PalHook
    if (connForm.palhook_url) {
      const h = await http.get('/palhook/health', { timeout: 10000 })
      const hd = h.data as any
      if (hd.status === 'ok') {
        ElMessage.success(`PalHook 连接成功 (v${hd.version})`)
        return
      }
    }
    const res = await http.get('/dashboard/info')
    const info = res.data as any
    if (info.servername) {
      ElMessage.success(`连接成功！服务器: ${info.servername} (${info.version})`)
    } else {
      ElMessage.success('连接正常')
    }
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '连接失败')
  } finally {
    testing.value = false
  }
}

const search = ref('')
const gameLoading = ref(false)
const gameSettings = ref<Record<string, unknown>>({})
const activeGroups = ref<string[]>(['Rates', 'Gameplay', 'Server'])

interface SettingItem { key: string; value: unknown }
interface SettingGroup { name: string; items: SettingItem[] }

const groupRules: Record<string, string[]> = {
  Rates: ['Rate', 'Multiplier', 'Speed'],
  Gameplay: ['bEnable', 'bIs', 'bAllow', 'bActive', 'bShow', 'Penalty', 'Difficulty'],
  PvP: ['PvP', 'Pvp', 'pvp', 'PlayerKilling'],
  Network: ['Port', 'IP', 'RCON', 'REST', 'Crossplay', 'Public', 'Chat', 'VoiceChat'],
  Server: ['Server', 'CoopPlayer', 'Guild', 'AutoSave', 'autoSave', 'Ban', 'Log'],
  Building: ['Build', 'BaseCamp', 'Block'],
  Pals: ['Pal', 'Egg', 'Monster', 'Fishing'],
  Items: ['Item', 'Drop', 'Collection', 'Equipment', 'Supply'],
}

function classifyKey(key: string): string {
  for (const [group, patterns] of Object.entries(groupRules)) {
    if (patterns.some((p) => key.includes(p))) return group
  }
  return 'Other'
}

const allGroups = computed<SettingGroup[]>(() => {
  const map = new Map<string, SettingItem[]>()
  for (const [key, value] of Object.entries(gameSettings.value)) {
    const group = classifyKey(key)
    if (!map.has(group)) map.set(group, [])
    map.get(group)!.push({ key, value })
  }
  const order = [...Object.keys(groupRules), 'Other']
  return order.filter((g) => map.has(g)).map((name) => ({
    name, items: map.get(name)!.sort((a, b) => a.key.localeCompare(b.key)),
  }))
})

const filteredGroups = computed<SettingGroup[]>(() => {
  if (!search.value) return allGroups.value
  const q = search.value.toLowerCase()
  return allGroups.value
    .map((g) => ({ ...g, items: g.items.filter((i) => i.key.toLowerCase().includes(q) || String(i.value).toLowerCase().includes(q)) }))
    .filter((g) => g.items.length > 0)
})

function formatValue(val: unknown): string {
  if (Array.isArray(val)) return val.join(', ')
  if (val === null || val === undefined) return '-'
  return String(val)
}

async function loadGameSettings() {
  gameLoading.value = true
  try {
    const res = await serverApi.settings()
    gameSettings.value = res.data as Record<string, unknown>
  } catch { /* not configured yet */ }
  finally { gameLoading.value = false }
}

onMounted(() => {
  loadConnectionConfig()
  loadGameSettings()
})
</script>

<style scoped>
.settings-page { color: var(--text-primary); }
.page-title { margin: 0 0 16px; font-size: 22px; color: var(--accent); }
.config-card { background: var(--bg-surface, #1a1a1a); }
.card-header { display: flex; align-items: center; justify-content: space-between; }
.form-hint { margin-left: 12px; font-size: 12px; color: var(--text-secondary); }
.search-input { width: 300px; }
.settings-collapse {
  --el-collapse-border-color: var(--border-color);
  --el-collapse-header-bg-color: var(--bg-surface);
  --el-collapse-content-bg-color: var(--bg-base);
}
.group-title { font-weight: 600; color: var(--text-primary); }
.group-count { margin-left: 8px; }
.setting-key { font-family: 'Fira Code', 'Consolas', monospace; font-size: 13px; color: var(--accent); }
.setting-value { font-family: 'Fira Code', 'Consolas', monospace; font-size: 13px; }
</style>
