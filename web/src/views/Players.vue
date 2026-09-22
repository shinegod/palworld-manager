<template>
  <div class="players-page">
    <h2 class="page-title">玩家管理</h2>

    <el-tabs v-model="activeTab" class="dark-tabs">
      <el-tab-pane label="在线" name="online">
        <div class="tab-toolbar">
          <el-input
            v-model="onlineSearch"
            placeholder="搜索玩家..."
            clearable
            :prefix-icon="Search"
            class="search-input"
          />
          <el-tag type="info" size="large">{{ filteredOnline.length }} 在线</el-tag>
        </div>
        <el-table
          :data="filteredOnline ?? []"
          stripe
          class="dark-table"
          empty-text="当前无玩家在线"
        >
          <el-table-column prop="name" label="名称" min-width="140" />
          <el-table-column prop="level" label="等级" width="80" align="center" />
          <el-table-column label="延迟" width="100" align="center">
            <template #default="{ row }">
              <span :class="pingClass(row.ping)">{{ Math.round(row.ping) }}ms</span>
            </template>
          </el-table-column>
          <el-table-column label="IP / 归属地" min-width="230">
            <template #default="{ row }">
              <div class="ip-cell">
                <span class="ip-line">
                  <span v-if="flagEmoji(ipInfos[row.iP]?.countryCode)" class="flag">{{ flagEmoji(ipInfos[row.iP]?.countryCode) }}</span>
                  <span class="mono">{{ row.iP || '-' }}</span>
                </span>
                <span v-if="ipInfos[row.iP]?.country" class="ip-geo">
                  {{ ipInfos[row.iP].country }} {{ ipInfos[row.iP].regionName || '' }} {{ ipInfos[row.iP].city || '' }} · {{ ipInfos[row.iP].isp || '' }}
                </span>
              </div>
            </template>
          </el-table-column>
          <el-table-column label="平台" width="100" align="center">
            <template #default="{ row }">
              <el-tag size="small" :type="row.platform === 'Steam' ? 'success' : 'info'">{{ row.platform || guessPlatform(row.userId) }}</el-tag>
            </template>
          </el-table-column>
          <el-table-column label="操作" width="130" align="center">
            <template #default="{ row }">
              <el-dropdown trigger="click" @command="(cmd: string) => handleAction(cmd, row)">
                <el-button size="small" type="primary">
                  操作<el-icon><ArrowDown /></el-icon>
                </el-button>
                <template #dropdown>
                  <el-dropdown-menu>
                    <el-dropdown-item command="item">🎒 刷道具</el-dropdown-item>
                    <el-dropdown-item command="pal">🐾 刷帕鲁</el-dropdown-item>
                    <el-dropdown-item command="stats">⚡ 角色属性</el-dropdown-item>
                    <el-dropdown-item command="teleport">📍 传送</el-dropdown-item>
                    <el-dropdown-item command="kick" divided>踢出</el-dropdown-item>
                    <el-dropdown-item command="ban">封禁</el-dropdown-item>
                  </el-dropdown-menu>
                </template>
              </el-dropdown>
            </template>
          </el-table-column>
        </el-table>
      </el-tab-pane>

      <el-tab-pane label="历史" name="history">
        <div class="tab-toolbar">
          <el-input
            v-model="historySearch"
            placeholder="搜索历史..."
            clearable
            :prefix-icon="Search"
            class="search-input"
          />
        </div>
        <el-table
          :data="filteredHistory ?? []"
          stripe
          class="dark-table"
          empty-text="暂无玩家历史记录"
        >
          <el-table-column prop="name" label="名称" min-width="140" />
          <el-table-column prop="level" label="等级" width="80" align="center" />
          <el-table-column prop="platform" label="平台" width="100" align="center">
            <template #default="{ row }">
              <el-tag size="small" type="info">{{ row.platform ?? '未知' }}</el-tag>
            </template>
          </el-table-column>
          <el-table-column label="首次登录" min-width="160">
            <template #default="{ row }">{{ formatTime(row.first_seen) }}</template>
          </el-table-column>
          <el-table-column label="最后登录" min-width="160">
            <template #default="{ row }">{{ formatTime(row.last_seen) }}</template>
          </el-table-column>
          <el-table-column label="游玩时长" width="120" align="center">
            <template #default="{ row }">{{ formatPlaytime(row.total_playtime_seconds) }}</template>
          </el-table-column>
          <el-table-column label="状态" width="100" align="center">
            <template #default="{ row }">
              <el-tag :type="row.online ? 'success' : 'info'" size="small">
                {{ row.online ? '在线' : '离线' }}
              </el-tag>
            </template>
          </el-table-column>
        </el-table>
        <div class="pagination-wrap">
          <el-pagination
            v-model:current-page="historyPage"
            :page-size="historyPageSize"
            :total="historyTotal"
            layout="prev, pager, next, total"
            @current-change="fetchHistory"
          />
        </div>
      </el-tab-pane>

      <el-tab-pane label="封禁" name="bans">
        <el-table
          :data="banList ?? []"
          stripe
          class="dark-table"
          empty-text="暂无封禁玩家"
        >
          <el-table-column prop="player_name" label="玩家名称" min-width="140" />
          <el-table-column prop="user_id" label="用户ID" min-width="200" />
          <el-table-column prop="reason" label="原因" min-width="200" />
          <el-table-column label="封禁时间" min-width="160">
            <template #default="{ row }">{{ formatTime(row.banned_at) }}</template>
          </el-table-column>
          <el-table-column label="操作" width="120" align="center">
            <template #default="{ row }">
              <el-button size="small" type="success" @click="openUnbanDialog(row)">解封</el-button>
            </template>
          </el-table-column>
        </el-table>
      </el-tab-pane>
    </el-tabs>

    <!-- Kick Dialog -->
    <el-dialog v-model="kickDialogVisible" title="踢出玩家" width="420" class="dark-dialog">
      <p>确定将 <strong>{{ targetPlayer?.name }}</strong> 踢出服务器？</p>
      <el-input v-model="actionReason" placeholder="原因（可选）" />
      <template #footer>
        <el-button @click="kickDialogVisible = false">取消</el-button>
        <el-button type="warning" :loading="actionLoading" @click="doKick">踢出</el-button>
      </template>
    </el-dialog>

    <!-- Ban Dialog -->
    <el-dialog v-model="banDialogVisible" title="封禁玩家" width="420" class="dark-dialog">
      <p>确定封禁 <strong>{{ targetPlayer?.name }}</strong>？</p>
      <el-input v-model="actionReason" placeholder="原因（可选）" />
      <template #footer>
        <el-button @click="banDialogVisible = false">取消</el-button>
        <el-button type="danger" :loading="actionLoading" @click="doBan">封禁</el-button>
      </template>
    </el-dialog>

    <!-- Unban Dialog -->
    <el-dialog v-model="unbanDialogVisible" title="解封玩家" width="420" class="dark-dialog">
      <p>确定解封 <strong>{{ targetBan?.player_name }}</strong>？</p>
      <template #footer>
        <el-button @click="unbanDialogVisible = false">取消</el-button>
        <el-button type="success" :loading="actionLoading" @click="doUnban">解封</el-button>
      </template>
    </el-dialog>

    <!-- Give Item Dialog -->
    <el-dialog v-model="giveItemVisible" title="给玩家刷道具" width="520" class="dark-dialog">
      <p>目标玩家: <strong>{{ targetPlayer?.name }}</strong></p>
      <el-select
        v-model="giveItemForm.item_id"
        filterable
        remote
        :remote-method="searchItems"
        placeholder="搜索物品 (中文/英文/ID)"
        style="width: 100%; margin-bottom: 12px"
      >
        <el-option
          v-for="it in itemOptions"
          :key="it.key"
          :label="it.nameZh + ' / ' + it.nameEn + ' (' + it.key + ')'"
          :value="it.key"
        />
      </el-select>
      <el-input-number v-model="giveItemForm.count" :min="1" :max="9999" />
      <template #footer>
        <el-button @click="giveItemVisible = false">取消</el-button>
        <el-button type="primary" :loading="giveLoading" @click="doGiveItem">刷入背包</el-button>
      </template>
    </el-dialog>

    <!-- Stats Dialog (经验/等级/金币/科技点) -->
    <el-dialog v-model="statsVisible" title="角色属性" width="520" class="dark-dialog">
      <p>目标玩家: <strong>{{ targetPlayer?.name }}</strong> (Lv{{ targetPlayer?.level }})</p>
      <el-form label-width="90px">
        <el-form-item label="加经验">
          <el-input-number v-model="statsForm.exp" :min="1" :max="99999999" :step="10000" />
          <el-button type="primary" :loading="statsLoading" @click="doGiveExp" style="margin-left: 8px">发放</el-button>
        </el-form-item>
        <el-form-item label="设置等级">
          <el-input-number v-model="statsForm.level" :min="1" :max="50" />
          <el-button type="primary" :loading="statsLoading" @click="doSetLevel" style="margin-left: 8px">设置</el-button>
        </el-form-item>
        <el-form-item label="加金币">
          <el-input-number v-model="statsForm.money" :min="1" :max="9999999" />
          <el-button type="primary" :loading="statsLoading" @click="doGiveMoney" style="margin-left: 8px">发放</el-button>
        </el-form-item>
        <el-form-item label="科技点">
          <el-input-number v-model="statsForm.tech" :min="0" :max="9999" />
          <span class="form-hint" style="margin: 0 8px">普通</span>
          <el-input-number v-model="statsForm.boss_tech" :min="0" :max="9999" />
          <span class="form-hint" style="margin-left: 8px">古代</span>
          <el-button type="primary" :loading="statsLoading" @click="doSetTech" style="margin-left: 8px">设置</el-button>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="statsVisible = false">关闭</el-button>
      </template>
    </el-dialog>

    <!-- Teleport Dialog -->
    <el-dialog v-model="tpVisible" title="传送玩家" width="520" class="dark-dialog">
      <p>目标玩家: <strong>{{ targetPlayer?.name }}</strong></p>
      <el-tabs v-model="tpTab">
        <el-tab-pane label="传送到坐标" name="coords">
          <el-form label-width="70px">
            <el-form-item label="X"><el-input-number v-model="tpForm.x" :step="1000" style="width: 100%" /></el-form-item>
            <el-form-item label="Y"><el-input-number v-model="tpForm.y" :step="1000" style="width: 100%" /></el-form-item>
            <el-form-item label="Z"><el-input-number v-model="tpForm.z" :step="100" style="width: 100%" /></el-form-item>
            <el-button type="primary" :loading="tpLoading" @click="doTeleportCoords">传送</el-button>
          </el-form>
        </el-tab-pane>
        <el-tab-pane label="传送到玩家" name="player">
          <el-form label-width="90px">
            <el-form-item label="目标玩家">
              <el-select v-model="tpForm.to_player" placeholder="选择在线玩家" style="width: 100%" @focus="refreshOnline">
                <el-option
                  v-for="pl in onlinePlayers"
                  :key="pl.playerId"
                  :label="pl.name + ' (Lv' + pl.level + ')'"
                  :value="pl.name"
                />
              </el-select>
            </el-form-item>
            <el-button type="success" :loading="tpLoading" @click="doTeleportToPlayer">传送过去</el-button>
          </el-form>
        </el-tab-pane>
      </el-tabs>
      <template #footer>
        <el-button @click="tpVisible = false">关闭</el-button>
      </template>
    </el-dialog>

    <!-- Give Pal Dialog -->
    <el-dialog v-model="givePalVisible" title="给玩家刷帕鲁" width="520" class="dark-dialog">
      <p>目标玩家: <strong>{{ targetPlayer?.name }}</strong></p>
      <el-select
        v-model="givePalForm.pal_id"
        filterable
        remote
        :remote-method="searchPals"
        placeholder="搜索帕鲁 (中文/英文/ID)"
        style="width: 100%; margin-bottom: 12px"
      >
        <el-option
          v-for="p in palOptions"
          :key="p.key"
          :label="p.nameZh + ' / ' + p.nameEn + ' (' + p.key + ')'"
          :value="p.key"
        />
      </el-select>
      <el-input-number v-model="givePalForm.level" :min="1" :max="100" />
      <el-checkbox v-model="givePalForm.capture" style="margin-left: 12px">自动捕获</el-checkbox>
      <template #footer>
        <el-button @click="givePalVisible = false">取消</el-button>
        <el-button type="success" :loading="giveLoading" @click="doGivePal">刷出</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive, computed, onMounted } from 'vue'
import { Search, ArrowDown } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'
import http from '@/api'
import { loadItems, loadPals, type GameItem, type GamePal } from '@/composables/useGameData'
import { usePolling } from '@/composables/usePolling'

interface OnlinePlayer {
  name: string
  accountName: string
  playerId: string
  userId: string
  iP: string
  platform: string
  ping: number
  location_x: number
  location_y: number
  level: number
}

interface HistoryPlayer {
  uid: string
  name: string
  level: number
  online: boolean
  first_seen: string
  last_seen: string
  total_playtime_seconds: number
  platform: string
}

interface BanRecord {
  id: number
  user_id: string
  player_name: string
  reason: string
  banned_at: string
}

const activeTab = ref('online')
const onlineSearch = ref('')
const historySearch = ref('')

const onlinePlayers = ref<OnlinePlayer[]>([])
const historyPlayers = ref<HistoryPlayer[]>([])
const banList = ref<BanRecord[]>([])
const historyPage = ref(1)
const historyPageSize = 50
const historyTotal = ref(0)

const kickDialogVisible = ref(false)
const banDialogVisible = ref(false)
const unbanDialogVisible = ref(false)
const actionReason = ref('')
const actionLoading = ref(false)
const targetPlayer = ref<OnlinePlayer | null>(null)
const targetBan = ref<BanRecord | null>(null)

const giveItemVisible = ref(false)
const givePalVisible = ref(false)
const giveLoading = ref(false)
const giveItemForm = reactive({ item_id: '', count: 99 })
const givePalForm = reactive({ pal_id: '', level: 50, capture: true })
const allItems = ref<GameItem[]>([])
const allPals = ref<GamePal[]>([])
const itemOptions = ref<GameItem[]>([])
const palOptions = ref<GamePal[]>([])

const statsVisible = ref(false)
const statsLoading = ref(false)
const statsForm = reactive({ exp: 100000, level: 50, money: 9999, tech: 100, boss_tech: 50 })

const tpVisible = ref(false)
const tpLoading = ref(false)
const tpTab = ref('coords')
const tpForm = reactive({ x: 0, y: 0, z: 0, to_player: '' })

function handleAction(cmd: string, row: OnlinePlayer) {
  switch (cmd) {
    case 'item': openGiveItemDialog(row); break
    case 'pal': openGivePalDialog(row); break
    case 'stats': openStatsDialog(row); break
    case 'teleport': openTpDialog(row); break
    case 'kick': openKickDialog(row); break
    case 'ban': openBanDialog(row); break
  }
}

function openStatsDialog(player: OnlinePlayer) {
  targetPlayer.value = player
  statsForm.level = player.level
  statsVisible.value = true
}

function openTpDialog(player: OnlinePlayer) {
  targetPlayer.value = player
  tpForm.to_player = ''
  tpVisible.value = true
  refreshOnline()
}

const filteredOnline = computed(() => {
  const q = onlineSearch.value.toLowerCase()
  if (!q) return onlinePlayers.value
  return onlinePlayers.value.filter(
    (p) => p.name.toLowerCase().includes(q) || p.iP.includes(q),
  )
})

const filteredHistory = computed(() => {
  const q = historySearch.value.toLowerCase()
  if (!q) return historyPlayers.value
  return historyPlayers.value.filter((p) => p.name.toLowerCase().includes(q))
})

function pingClass(ping: number): string {
  if (ping < 50) return 'ping-good'
  if (ping < 100) return 'ping-warn'
  return 'ping-bad'
}

const ipInfos = ref<Record<string, any>>({})

async function fetchIpInfos() {
  const ips = Array.from(new Set(onlinePlayers.value.map((p) => p.iP).filter((ip) => ip && ip !== '0.0.0.0')))
  for (const ip of ips) {
    try {
      const res = await http.get('/ipinfo', { params: { ip }, timeout: 8000 })
      if (res.data?.country) {
        ipInfos.value[ip] = res.data
      }
    } catch {
      // ignore single failures
    }
  }
}

function flagEmoji(cc?: string): string {
  if (!cc || cc.length !== 2) return ''
  return String.fromCodePoint(...Array.from(cc.toUpperCase()).map((c) => 127397 + c.charCodeAt(0)))
}

function guessPlatform(userId: string): string {
  if (userId.startsWith('steam_')) return 'Steam'
  if (userId.startsWith('xbox_')) return 'Xbox'
  return '未知'
}

function formatTime(t: string | undefined): string {
  if (!t) return '-'
  return new Date(t).toLocaleString()
}

function formatPlaytime(seconds: number | undefined): string {
  if (!seconds) return '0分'
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  return h > 0 ? `${h}时 ${m}分` : `${m}分`
}

async function fetchOnline() {
  try {
    const res = await http.get<OnlinePlayer[]>('/players/online')
    onlinePlayers.value = res.data ?? []
    fetchIpInfos()
  } catch {
    // REST不可用时从PalHook兜底
    try {
      const res2 = await http.get<any>('/palhook/players', { timeout: 60000 })
      onlinePlayers.value = (res2.data?.players ?? []).map((p: any) => ({
        name: p.name,
        accountName: p.name,
        playerId: p.uid,
        userId: '',
        iP: p.ip ?? '',
        platform: p.platform ?? '',
        ping: p.ping ?? 0,
        location_x: p.x,
        location_y: p.y,
        location_z: p.z,
        level: p.level,
      }))
      fetchIpInfos()
    } catch {
      ElMessage.error('获取在线玩家失败')
    }
  }
}

async function fetchHistory() {
  try {
    const offset = (historyPage.value - 1) * historyPageSize
    const res = await http.get<HistoryPlayer[]>('/players/history', {
      params: { limit: historyPageSize, offset },
    })
    historyPlayers.value = res.data ?? []
    if (historyPlayers.value.length < historyPageSize && historyPage.value === 1) {
      historyTotal.value = historyPlayers.value.length
    } else {
      historyTotal.value = Math.max(historyTotal.value, offset + historyPlayers.value.length + 1)
    }
  } catch {
    ElMessage.error('获取玩家历史记录失败')
  }
}

async function fetchBans() {
  try {
    const res = await http.get<BanRecord[]>('/players/bans')
    banList.value = res.data ?? []
  } catch {
    ElMessage.error('获取封禁列表失败')
  }
}

function openKickDialog(player: OnlinePlayer) {
  targetPlayer.value = player
  actionReason.value = ''
  kickDialogVisible.value = true
}

function openBanDialog(player: OnlinePlayer) {
  targetPlayer.value = player
  actionReason.value = ''
  banDialogVisible.value = true
}

function openUnbanDialog(ban: BanRecord) {
  targetBan.value = ban
  unbanDialogVisible.value = true
}

async function doKick() {
  if (!targetPlayer.value) return
  actionLoading.value = true
  try {
    await http.post('/players/kick', {
      userid: targetPlayer.value.userId,
      message: actionReason.value || '被管理员踢出',
    })
  } catch {
    // REST不可用时从PalHook兜底 (按名字踢)
    try {
      await http.post('/palhook/kick', { name: targetPlayer.value.name }, { timeout: 60000 })
    } catch {
      ElMessage.error('踢出玩家失败')
      actionLoading.value = false
      return
    }
  }
  ElMessage.success(`已踢出 ${targetPlayer.value.name}`)
  kickDialogVisible.value = false
  actionLoading.value = false
  await fetchOnline()
}

async function doBan() {
  if (!targetPlayer.value) return
  actionLoading.value = true
  try {
    await http.post('/players/ban', {
      userid: targetPlayer.value.userId,
      message: actionReason.value || '被管理员封禁',
    })
    ElMessage.success(`已封禁 ${targetPlayer.value.name}`)
    banDialogVisible.value = false
    await fetchOnline()
    await fetchBans()
  } catch {
    ElMessage.error('封禁玩家失败')
  } finally {
    actionLoading.value = false
  }
}

async function doUnban() {
  if (!targetBan.value) return
  actionLoading.value = true
  try {
    await http.post('/players/unban', { userid: targetBan.value.user_id })
    ElMessage.success(`已解封 ${targetBan.value.player_name}`)
    unbanDialogVisible.value = false
    await fetchBans()
  } catch {
    ElMessage.error('解封玩家失败')
  } finally {
    actionLoading.value = false
  }
}

async function ensureGameData() {
  if (allItems.value.length === 0) {
    try {
      allItems.value = await loadItems()
      itemOptions.value = allItems.value.slice(0, 100)
    } catch { /* ignore */ }
  }
  if (allPals.value.length === 0) {
    try {
      allPals.value = await loadPals()
      palOptions.value = allPals.value.slice(0, 100)
    } catch { /* ignore */ }
  }
}

function searchItems(q: string) {
  const s = q.toLowerCase()
  itemOptions.value = allItems.value
    .filter((it) => it.nameZh.toLowerCase().includes(s) || it.nameEn.toLowerCase().includes(s) || it.key.toLowerCase().includes(s))
    .slice(0, 100)
}

function searchPals(q: string) {
  const s = q.toLowerCase()
  palOptions.value = allPals.value
    .filter((p) => p.nameZh.toLowerCase().includes(s) || p.nameEn.toLowerCase().includes(s) || p.key.toLowerCase().includes(s))
    .slice(0, 100)
}

function openGiveItemDialog(player: OnlinePlayer) {
  targetPlayer.value = player
  giveItemForm.item_id = ''
  giveItemForm.count = 99
  giveItemVisible.value = true
  ensureGameData()
}

function openGivePalDialog(player: OnlinePlayer) {
  targetPlayer.value = player
  givePalForm.pal_id = ''
  givePalForm.level = 50
  givePalForm.capture = true
  givePalVisible.value = true
  ensureGameData()
}

async function doGiveItem() {
  if (!targetPlayer.value || !giveItemForm.item_id) {
    ElMessage.warning('请选择物品')
    return
  }
  giveLoading.value = true
  try {
    const res = await http.post('/palhook/give-item', {
      item_id: giveItemForm.item_id,
      count: giveItemForm.count,
      name: targetPlayer.value.name,
    }, { timeout: 60000 })
    const d = res.data as any
    if (d.status === 'success') {
      ElMessage.success(`已给 ${targetPlayer.value.name} 刷入 ${giveItemForm.count} x ${giveItemForm.item_id}`)
      giveItemVisible.value = false
    } else ElMessage.error(d.error || '刷道具失败')
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '刷道具失败')
  } finally {
    giveLoading.value = false
  }
}

async function doGivePal() {
  if (!targetPlayer.value || !givePalForm.pal_id) {
    ElMessage.warning('请选择帕鲁')
    return
  }
  giveLoading.value = true
  try {
    const res = await http.post('/palhook/spawn-pal', {
      pal_id: givePalForm.pal_id,
      level: givePalForm.level,
      capture: givePalForm.capture ? 1 : 0,
      name: targetPlayer.value.name,
    }, { timeout: 90000 })
    const d = res.data as any
    if (d.status === 'success') {
      ElMessage.success(`已给 ${targetPlayer.value.name} 刷出 ${givePalForm.pal_id} (Lv${givePalForm.level})`)
      givePalVisible.value = false
    } else ElMessage.error(d.error || '刷帕鲁失败')
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '刷帕鲁失败')
  } finally {
    giveLoading.value = false
  }
}

async function doGiveExp() {
  if (!targetPlayer.value) return
  statsLoading.value = true
  try {
    const res = await http.post('/palhook/give-exp', { exp: statsForm.exp, name: targetPlayer.value.name }, { timeout: 60000 })
    const d = res.data as any
    if (d.status === 'success') ElMessage.success(`已给 ${targetPlayer.value.name} 发放 ${statsForm.exp} 经验`)
    else ElMessage.error(d.error || '发放失败')
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '发放失败')
  } finally {
    statsLoading.value = false
  }
}

async function doSetLevel() {
  if (!targetPlayer.value) return
  statsLoading.value = true
  try {
    const res = await http.post('/palhook/set-level', { level: statsForm.level, name: targetPlayer.value.name }, { timeout: 60000 })
    const d = res.data as any
    if (d.status === 'success') ElMessage.success(`${targetPlayer.value.name} 等级已设为 ${d.new_level}`)
    else ElMessage.error(d.error || '设置失败')
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '设置失败')
  } finally {
    statsLoading.value = false
  }
}

async function doGiveMoney() {
  if (!targetPlayer.value) return
  statsLoading.value = true
  try {
    const res = await http.post('/palhook/give-money', { amount: statsForm.money, name: targetPlayer.value.name }, { timeout: 60000 })
    const d = res.data as any
    if (d.status === 'success') ElMessage.success(`已给 ${targetPlayer.value.name} 发放 ${statsForm.money} 金币`)
    else ElMessage.error(d.error || '发放失败')
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '发放失败')
  } finally {
    statsLoading.value = false
  }
}

async function doSetTech() {
  if (!targetPlayer.value) return
  statsLoading.value = true
  try {
    const res = await http.post('/palhook/set-tech-points', {
      tech: statsForm.tech, boss_tech: statsForm.boss_tech, name: targetPlayer.value.name,
    }, { timeout: 60000 })
    const d = res.data as any
    if (d.status === 'success') {
      ElMessage.success(`科技点已设置: 普通=${d.tech?.new ?? statsForm.tech}, 古代=${d.boss_tech?.new ?? statsForm.boss_tech}`)
    } else ElMessage.error(d.error || '设置失败')
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '设置失败')
  } finally {
    statsLoading.value = false
  }
}

async function doTeleportCoords() {
  if (!targetPlayer.value) return
  tpLoading.value = true
  try {
    const res = await http.post('/palhook/teleport', { x: tpForm.x, y: tpForm.y, z: tpForm.z, name: targetPlayer.value.name }, { timeout: 60000 })
    const d = res.data as any
    if (d.status === 'success') ElMessage.success(`${targetPlayer.value.name} 已传送到 (${tpForm.x}, ${tpForm.y}, ${tpForm.z})`)
    else ElMessage.error(d.error || '传送失败')
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '传送失败')
  } finally {
    tpLoading.value = false
  }
}

async function doTeleportToPlayer() {
  if (!targetPlayer.value || !tpForm.to_player) {
    ElMessage.warning('请选择目标玩家')
    return
  }
  tpLoading.value = true
  try {
    const res = await http.post('/palhook/teleport', { name: targetPlayer.value.name, to: tpForm.to_player }, { timeout: 60000 })
    const d = res.data as any
    if (d.status === 'success') ElMessage.success(`${targetPlayer.value.name} 已传送到 ${tpForm.to_player} 身边`)
    else ElMessage.error(d.error || '传送失败')
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '传送失败')
  } finally {
    tpLoading.value = false
  }
}

function refreshOnline() {
  fetchOnline()
}

onMounted(() => {
  fetchHistory()
  fetchBans()
})

usePolling(fetchOnline, 10000)
</script>

<style scoped>
.ip-cell { display: flex; flex-direction: column; line-height: 1.5; }
.ip-line { display: flex; align-items: center; gap: 6px; }
.flag { font-size: 16px; }
.ip-geo { font-size: 12px; color: var(--text-secondary); }
.players-page {
  color: var(--text-primary);
}
.page-title {
  margin: 0 0 16px 0;
  font-size: 22px;
  color: var(--accent);
}
.tab-toolbar {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 16px;
}
.search-input {
  max-width: 300px;
}
.pagination-wrap {
  display: flex;
  justify-content: center;
  margin-top: 16px;
}
.ping-good { color: #22c55e; font-weight: 600; }
.ping-warn { color: #eab308; font-weight: 600; }
.ping-bad  { color: #ef4444; font-weight: 600; }
</style>
