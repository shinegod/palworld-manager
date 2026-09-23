<template>
  <div class="anticheat-page">
    <div class="page-header">
      <h2 class="page-title">反作弊</h2>
      <div class="header-actions">
        <el-button type="primary" @click="runScan" :loading="scanning">立即扫描</el-button>
        <el-button type="warning" plain @click="showConfig">配置</el-button>
      </div>
    </div>

    <el-row :gutter="16" style="margin-bottom: 16px">
      <el-col :span="6">
        <el-card shadow="never" class="stat-card">
          <div class="stat-value">{{ stats.totalScans }}</div>
          <div class="stat-label">扫描总次数</div>
        </el-card>
      </el-col>
      <el-col :span="6">
        <el-card shadow="never" class="stat-card">
          <div class="stat-value" style="color: var(--el-color-danger)">{{ stats.totalFlags }}</div>
          <div class="stat-label">检测标记</div>
        </el-card>
      </el-col>
      <el-col :span="6">
        <el-card shadow="never" class="stat-card">
          <div class="stat-value" style="color: var(--el-color-success)">{{ stats.resolved }}</div>
          <div class="stat-label">已处理</div>
        </el-card>
      </el-col>
      <el-col :span="6">
        <el-card shadow="never" class="stat-card">
          <div class="stat-value" style="color: var(--el-color-warning)">{{ stats.pending }}</div>
          <div class="stat-label">待处理</div>
        </el-card>
      </el-col>
    </el-row>

    <el-alert type="info" :closable="false" style="margin-bottom: 16px">
      反作弊每 60 秒自动对比 PalHook 在线玩家快照: 等级超过上限 (80)、经验异常、同 IP 多账号在线。
      也可手动触发立即扫描。
    </el-alert>

    <h3 style="color: var(--text-primary); margin-bottom: 12px">检测日志</h3>
    <el-table :data="flags ?? []" stripe empty-text="未检测到异常 - 服务器运行正常">
      <el-table-column prop="player_name" label="玩家" min-width="150" />
      <el-table-column prop="flag_type" label="类型" width="120">
        <template #default="{ row }">
          <el-tag size="small">{{ row.flag_type }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="severity" label="严重程度" width="100">
        <template #default="{ row }">
          <el-tag :type="severityType(row.severity)" size="small" effect="dark">
            {{ row.severity }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="details" label="详情" min-width="250" />
      <el-table-column prop="flagged_at" label="时间" width="180">
        <template #default="{ row }">{{ formatTime(row.flagged_at) }}</template>
      </el-table-column>
      <el-table-column label="状态" width="100">
        <template #default="{ row }">
          <el-tag :type="row.resolved ? 'success' : 'danger'" size="small">
            {{ row.resolved ? '已处理' : '待处理' }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column label="操作" width="160" fixed="right">
        <template #default="{ row }">
          <template v-if="!row.resolved">
            <el-button size="small" type="success" plain @click="resolveFlag(row)">处理</el-button>
            <el-button size="small" type="danger" plain @click="banFlagged(row)">封禁</el-button>
          </template>
        </template>
      </el-table-column>
    </el-table>

    <el-dialog v-model="configVisible" title="反作弊配置" width="500px">
      <el-form label-width="200px">
        <el-form-item label="等级差异阈值">
          <el-input-number v-model="config.levelGapThreshold" :min="1" :max="50" />
        </el-form-item>
        <el-form-item label="扫描间隔（分钟）">
          <el-input-number v-model="config.scanIntervalMinutes" :min="5" :max="1440" />
        </el-form-item>
        <el-form-item label="自动扫描">
          <el-switch v-model="config.autoScanEnabled" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="configVisible = false">关闭</el-button>
        <el-button type="primary" @click="saveConfig">保存</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive, onMounted } from 'vue'
import { ElMessage } from 'element-plus'
import http from '@/api'

interface Flag {
  id: number
  player_name: string
  user_id: string
  flag_type: string
  severity: string
  details: string
  resolved: boolean
  action_taken: string
  flagged_at: string
}

const flags = ref<Flag[]>([])
const scanning = ref(false)
const configVisible = ref(false)
const config = reactive({ levelGapThreshold: 10, scanIntervalMinutes: 30, autoScanEnabled: false })
const stats = reactive({ totalScans: 0, totalFlags: 0, resolved: 0, pending: 0 })

function severityType(s: string): 'success' | 'warning' | 'danger' | 'info' {
  switch (s) { case 'high': return 'danger'; case 'medium': return 'warning'; case 'low': return 'info'; default: return 'info' }
}

function formatTime(ts: string): string {
  return ts ? new Date(ts).toLocaleString() : '-'
}

async function fetchFlags() {
  try {
    const res = await http.get('/anticheat/flags')
    flags.value = (res.data?.flags as Flag[]) ?? []
    stats.totalScans = res.data?.total_scans ?? 0
    stats.totalFlags = flags.value.length
    stats.resolved = flags.value.filter((f) => f.resolved).length
    stats.pending = flags.value.filter((f) => !f.resolved).length
  } catch { /* ignore */ }
}

async function runScan() {
  scanning.value = true
  try {
    await http.post('/anticheat/scan')
    stats.totalScans++
    ElMessage.success('扫描完成')
    await fetchFlags()
  } catch {
    ElMessage.error('扫描失败')
  } finally {
    scanning.value = false
  }
}

async function resolveFlag(flag: Flag) {
  try {
    await http.post('/anticheat/flags/' + flag.id + '/resolve')
    flag.resolved = true
    stats.resolved++
    stats.pending--
    ElMessage.success('已标记为已处理')
  } catch {
    ElMessage.error('处理失败')
  }
}

async function banFlagged(flag: Flag) {
  try {
    await http.post('/players/ban', { userid: flag.user_id, message: '反作弊: ' + flag.flag_type })
    await http.post('/anticheat/flags/' + flag.id + '/resolve', { action: 'banned' })
    flag.resolved = true
    flag.action_taken = 'banned'
    ElMessage.success('玩家已封禁')
    await fetchFlags()
  } catch {
    ElMessage.error('封禁失败 (玩家可能已离线, 仍可在封禁列表手动添加)')
  }
}

function showConfig() { configVisible.value = true }
function saveConfig() {
  ElMessage.success('配置已保存（仅本地）')
  configVisible.value = false
}

let refreshTimer: ReturnType<typeof setInterval> | null = null

onMounted(() => {
  fetchFlags()
  refreshTimer = setInterval(fetchFlags, 30000)
})
</script>

<style scoped>
.anticheat-page { color: var(--text-primary); }
.page-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 16px; }
.page-title { margin: 0; font-size: 22px; color: var(--accent); }
.header-actions { display: flex; gap: 8px; }
.stat-card { text-align: center; background: var(--bg-surface, #1a1a1a); }
.stat-value { font-size: 28px; font-weight: 700; color: var(--accent); }
.stat-label { font-size: 12px; color: var(--text-secondary); margin-top: 4px; }
</style>
