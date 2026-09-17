<template>
  <div class="dashboard">
    <el-alert
      v-if="store.connectionStatus === 'disconnected'"
      type="warning"
      :closable="false"
      style="margin-bottom: 16px"
      title="PalHook 未连接"
    >
      请到 <router-link to="/settings">设置 → 连接配置</router-link> 填写 PalHook 地址和密码（hook-only 架构，全部数据来自 PalHook）。
    </el-alert>

    <!-- Metric Gauges -->
    <el-row :gutter="16" class="dashboard__row">
      <el-col :xs="12" :sm="8" :md="4" v-for="g in gauges" :key="g.title">
        <GaugeCard
          :title="g.title"
          :value="g.value"
          :max="g.max"
          :unit="g.unit"
          :color="g.color"
          :icon="g.icon"
        />
      </el-col>
    </el-row>

    <!-- Bottom: Players + Quick Actions -->
    <el-row :gutter="16" class="dashboard__row">
      <el-col :xs="24" :md="16">
        <el-card shadow="never">
          <template #header>
            <span class="section-title">在线玩家</span>
          </template>
          <el-table :data="store.onlinePlayers ?? []" size="small" max-height="320" stripe>
            <el-table-column prop="name" label="名称" min-width="120" />
            <el-table-column prop="level" label="等级" width="70" align="center" />
            <el-table-column prop="exp" label="经验" width="110" align="center" />
            <el-table-column label="坐标" min-width="140" align="center">
              <template #default="{ row }">
                <span class="mono">{{ fmtCoord(row) }}</span>
              </template>
            </el-table-column>
          </el-table>
          <div v-if="(store.onlinePlayers ?? []).length === 0" class="empty-hint">
            当前无玩家在线
          </div>
        </el-card>
      </el-col>
      <el-col :xs="24" :md="8">
        <el-card shadow="never" class="actions-card">
          <template #header>
            <span class="section-title">快捷操作</span>
          </template>
          <div class="actions-grid">
            <el-button type="primary" @click="showAnnounce = true">
              <el-icon><ChatDotRound /></el-icon> 发公告
            </el-button>
            <el-button type="info" @click="showChat = true">
              <el-icon><ChatLineRound /></el-icon> 发聊天
            </el-button>
          </div>
          <div class="actions-tip">
            服务器进程的启停请在宿主机 (Docker/MCSM) 操作，hook 注入库只做无感管理。
          </div>
        </el-card>
      </el-col>
    </el-row>

    <!-- Announce Dialog -->
    <el-dialog v-model="showAnnounce" title="发送公告" width="420px" :append-to-body="true">
      <el-input
        v-model="announceMsg"
        type="textarea"
        :rows="3"
        placeholder="输入要广播的消息..."
      />
      <template #footer>
        <el-button @click="showAnnounce = false">取消</el-button>
        <el-button type="primary" :loading="announcing" @click="handleAnnounce">发送</el-button>
      </template>
    </el-dialog>

    <!-- Chat Dialog -->
    <el-dialog v-model="showChat" title="发送聊天消息" width="420px" :append-to-body="true">
      <el-input
        v-model="chatMsg"
        type="textarea"
        :rows="3"
        placeholder="以服务器身份在玩家聊天窗口说话..."
      />
      <template #footer>
        <el-button @click="showChat = false">取消</el-button>
        <el-button type="primary" :loading="chatting" @click="handleChat">发送</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { ElMessage } from 'element-plus'
import { ChatDotRound, ChatLineRound } from '@element-plus/icons-vue'
import { useServerStore } from '@/stores/server'
import { palhookApi } from '@/api'
import GaugeCard from '@/components/dashboard/GaugeCard.vue'

const store = useServerStore()

const gauges = computed(() => {
  const m = store.realtimeMetrics
  return [
    { title: '服务器FPS', value: m?.serverfps ?? 0, max: 120, unit: '', color: '#22d3ee', icon: 'Odometer' },
    { title: '帧时间', value: m?.serverframetime ?? 0, max: 50, unit: 'ms', color: '#f59e0b', icon: 'Timer' },
    { title: '在线玩家', value: m?.currentplayernum ?? 0, max: m?.maxplayernum ?? 32, unit: '', color: '#22c55e', icon: 'User' },
    { title: '运行时间', value: fmtUptime(m?.uptime ?? 0), max: 0, unit: 'h', color: '#a78bfa', icon: 'Clock' },
    { title: '游戏天数', value: m?.days ?? 0, max: 999, unit: '', color: '#fb923c', icon: 'Sunny' },
    { title: 'PalHook', value: hookVersion().length > 0 ? 1 : 0, max: 1, unit: '', color: '#f472b6', icon: 'House' },
  ]
})

function fmtUptime(seconds: number): number {
  return Math.floor(seconds / 3600)
}

function fmtCoord(row: any): string {
  const x = row.location_x ?? row.x ?? 0
  const y = row.location_y ?? row.y ?? 0
  return Math.round(x) + ', ' + Math.round(y)
}

function hookVersion(): string {
  return store.serverInfo?.version ?? ''
}

const showAnnounce = ref(false)
const announceMsg = ref('')
const announcing = ref(false)

const showChat = ref(false)
const chatMsg = ref('')
const chatting = ref(false)

async function handleAnnounce() {
  if (!announceMsg.value.trim()) return
  announcing.value = true
  try {
    await palhookApi.announce(announceMsg.value)
    ElMessage.success('公告已发送')
    showAnnounce.value = false
    announceMsg.value = ''
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '发送公告失败')
  } finally {
    announcing.value = false
  }
}

async function handleChat() {
  if (!chatMsg.value.trim()) return
  chatting.value = true
  try {
    await palhookApi.chat(chatMsg.value)
    ElMessage.success('聊天消息已发送')
    showChat.value = false
    chatMsg.value = ''
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '发送聊天消息失败')
  } finally {
    chatting.value = false
  }
}

let refreshTimer: ReturnType<typeof setInterval> | undefined

onMounted(() => {
  store.fetchInfo()
  store.fetchRealtime()
  refreshTimer = setInterval(() => {
    store.fetchRealtime()
  }, 10000)
})

onUnmounted(() => {
  if (refreshTimer) clearInterval(refreshTimer)
})
</script>

<style scoped>
.dashboard__row {
  margin-bottom: 16px;
}
.section-title {
  font-size: 15px;
  font-weight: 600;
  color: var(--text-primary);
}
.empty-hint {
  text-align: center;
  padding: 24px 0;
  color: var(--text-secondary);
  font-size: 13px;
}
.actions-grid {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
  margin-bottom: 14px;
}
.actions-tip {
  font-size: 12px;
  color: var(--text-secondary);
  line-height: 1.6;
}
.mono {
  font-family: 'JetBrains Mono', 'SF Mono', monospace;
  font-size: 12px;
}
</style>
