// Guilds.vue — hook-only 公会管理
<template>
  <div class="guilds-page">
    <div class="page-header">
      <h2 class="page-title">公会管理</h2>
      <div class="header-actions">
        <el-input
          v-model="search"
          placeholder="搜索公会/成员名"
          clearable
          size="default"
          style="width: 200px"
        >
          <template #prefix><el-icon><Search /></el-icon></template>
        </el-input>
        <el-button type="primary" @click="fetchGuilds" :loading="loading">
          <el-icon><Refresh /></el-icon> 刷新
        </el-button>
      </div>
    </div>

    <el-alert v-if="!loading && guilds.length === 0" type="info" :closable="false" style="margin-bottom: 16px">
      暂无公会数据（公会列表通过 PalHook 从服务器内存实时读取）。
    </el-alert>

    <el-table
      :data="filteredGuilds"
      stripe
      v-loading="loading"
      @row-click="selectGuild"
      highlight-current-row
      empty-text="暂无公会数据"
      :default-sort="{ prop: 'level', order: 'descending' }"
    >
      <el-table-column prop="name" label="公会名称" min-width="180" sortable>
        <template #default="{ row }">
          <span class="guild-name">{{ row.name || '未命名公会' }}</span>
        </template>
      </el-table-column>
      <el-table-column prop="level" label="基地等级" width="100" sortable align="center">
        <template #default="{ row }">
          <el-tag size="small" :type="row.level >= 20 ? 'danger' : row.level >= 10 ? 'warning' : 'primary'">
            Lv.{{ row.level ?? '-' }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column label="成员" width="110" align="center" sortable :sort-by="memberSort">
        <template #default="{ row }">
          <span :class="onlineCount(row) > 0 ? 'online-cnt' : 'offline-cnt'">
            {{ onlineCount(row) }} / {{ row.member_count ?? 0 }}
          </span>
        </template>
      </el-table-column>
      <el-table-column prop="basecamp_count" label="营地" width="80" align="center" sortable />
      <el-table-column label="会长" min-width="140">
        <template #default="{ row }">
          <span class="admin-name">{{ adminName(row) }}</span>
        </template>
      </el-table-column>
      <el-table-column label="操作" width="90" align="center">
        <template #default="{ row }">
          <el-button type="primary" link @click.stop="selectGuild(row)">详情</el-button>
        </template>
      </el-table-column>
    </el-table>

    <el-dialog v-model="detailVisible" :title="selectedGuild?.name || '公会详情'" width="76%" top="5vh">
      <template v-if="selectedGuild">
        <el-descriptions :column="3" border size="small" style="margin-bottom: 16px">
          <el-descriptions-item label="公会名称">{{ selectedGuild.name || '未命名' }}</el-descriptions-item>
          <el-descriptions-item label="基地等级">Lv.{{ selectedGuild.level ?? '-' }}</el-descriptions-item>
          <el-descriptions-item label="营地数">{{ selectedGuild.basecamp_count ?? 0 }}</el-descriptions-item>
          <el-descriptions-item label="成员">{{ selectedGuild.member_count ?? 0 }} 人（在线 {{ onlineCount(selectedGuild) }}）</el-descriptions-item>
          <el-descriptions-item label="会长">
            <span v-if="adminName(selectedGuild) !== '-'">
              {{ adminName(selectedGuild) }}
              <el-button type="primary" link size="small" @click="copyText(adminUidOf(selectedGuild))">复制UID</el-button>
            </span>
            <span v-else>-</span>
          </el-descriptions-item>
          <el-descriptions-item label="公会ID">
            <span class="mono">{{ (selectedGuild.id || '').slice(0, 16) }}…</span>
            <el-button type="primary" link size="small" @click="copyText(selectedGuild.id)">复制</el-button>
          </el-descriptions-item>
        </el-descriptions>

        <el-table :data="selectedGuild.members ?? []" stripe size="small" empty-text="暂无成员数据">
          <el-table-column label="名称" min-width="140">
            <template #default="{ row }">{{ row.name || row.uid || '-' }}</template>
          </el-table-column>
          <el-table-column label="UID" min-width="240">
            <template #default="{ row }">
              <span class="mono">{{ row.uid || '-' }}</span>
              <el-button type="primary" link size="small" @click="copyText(row.uid)" v-if="row.uid">复制</el-button>
            </template>
          </el-table-column>
          <el-table-column label="角色" width="100">
            <template #default="{ row }">
              <el-tag :type="roleTag(row.role)" size="small">{{ roleName(row) }}</el-tag>
            </template>
          </el-table-column>
          <el-table-column label="状态" width="90">
            <template #default="{ row }">
              <el-tag :type="row.status === 1 ? 'success' : 'info'" size="small" effect="dark">
                {{ row.status === 1 ? '在线' : '离线' }}
              </el-tag>
            </template>
          </el-table-column>
          <el-table-column label="最后在线" width="150">
            <template #default="{ row }">{{ fmtTime(row) }}</template>
          </el-table-column>
        </el-table>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { ElMessage } from 'element-plus'
import { Search, Refresh } from '@element-plus/icons-vue'
import http from '@/api'

const guilds = ref<any[]>([])
const loading = ref(false)
const search = ref('')
const detailVisible = ref(false)
const selectedGuild = ref<any>(null)

const ROLE_NAMES: Record<number, string> = {
  0: '无', 1: '会长', 2: '副会', 3: '成员', 4: '访客',
}

async function fetchGuilds() {
  loading.value = true
  try {
    const res = await http.get<any>('/palhook/guilds', { timeout: 60000 })
    const d = res.data as any
    guilds.value = (d?.guilds ?? []).map((g: any) => ({
      ...g,
      member_count: g.member_count ?? (g.members?.length ?? 0),
      members: g.members ?? [],
    }))
  } catch {
    guilds.value = []
  } finally {
    loading.value = false
  }
}

const filteredGuilds = computed(() => {
  const q = search.value.trim().toLowerCase()
  if (!q) return guilds.value
  return guilds.value.filter((g: any) => {
    if ((g.name ?? '').toLowerCase().includes(q)) return true
    return (g.members ?? []).some((m: any) => (m.name ?? '').toLowerCase().includes(q))
  })
})

function memberSort(g: any): number {
  return g.member_count ?? 0
}

function onlineCount(g: any): number {
  return (g?.members ?? []).filter((m: any) => m.status === 1).length
}

function adminUidOf(g: any): string {
  return g?.admin_uid ?? ''
}

function adminName(g: any): string {
  const uid = adminUidOf(g)
  if (!uid || uid === '00000000000000000000000000000000') return '-'
  const m = (g?.members ?? []).find((x: any) => x.uid === uid)
  return m?.name || (uid.slice(0, 8) + '…')
}

function roleName(row: any): string {
  return row.role_name || ROLE_NAMES[row.role] || ('角色' + row.role)
}

function roleTag(role: number): string {
  if (role === 1) return 'warning'
  if (role === 2) return 'success'
  return 'info'
}

function fmtTime(row: any): string {
  if (row.status === 1) return '在线中'
  const ts = row.last_online_unix
  if (!ts || ts <= 0) return '-'
  const d = new Date(ts * 1000)
  const now = Date.now() / 1000
  const diff = Math.max(0, Math.floor(now - ts))
  if (diff < 3600) return Math.max(1, Math.floor(diff / 60)) + ' 分钟前'
  if (diff < 86400) return Math.floor(diff / 3600) + ' 小时前'
  const pad = (n: number) => String(n).padStart(2, '0')
  return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes())
}

async function copyText(text: string) {
  if (!text) return
  try {
    await navigator.clipboard.writeText(text)
    ElMessage.success('已复制')
  } catch {
    ElMessage.error('复制失败')
  }
}

function selectGuild(row: any) {
  selectedGuild.value = row
  detailVisible.value = true
}

let timer: ReturnType<typeof setInterval> | null = null

onMounted(() => {
  fetchGuilds()
  timer = setInterval(fetchGuilds, 120000)
})

onUnmounted(() => {
  if (timer) clearInterval(timer)
})
</script>

<style scoped>
.guilds-page { color: var(--text-primary); }
.page-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 16px; flex-wrap: wrap; gap: 8px; }
.page-title { margin: 0; font-size: 22px; color: var(--accent); }
.header-actions { display: flex; gap: 8px; align-items: center; }
.guild-name { font-weight: 600; }
.admin-name { color: var(--text-secondary); }
.mono { font-family: 'JetBrains Mono', 'SF Mono', monospace; font-size: 12px; }
.online-cnt { color: var(--el-color-success); font-weight: 600; }
.offline-cnt { color: var(--text-secondary); }
</style>
