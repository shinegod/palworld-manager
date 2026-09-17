<template>
  <div class="monitor-page">
    <div class="page-header">
      <h2 class="page-title">监控中心</h2>
      <div class="header-actions">
        <el-button type="primary" @click="fetchAlerts" :loading="loading">
          刷新
        </el-button>
        <el-button type="danger" plain @click="handleClearAll" :disabled="alerts.length === 0">
          清除全部
        </el-button>
      </div>
    </div>

    <el-table
      :data="alerts ?? []"
      stripe
      style="width: 100%"
      empty-text="暂无告警 - 服务器运行正常"
      v-loading="loading"
    >
      <el-table-column prop="level" label="级别" width="100">
        <template #default="{ row }">
          <el-tag
            :type="levelType(row.level)"
            size="small"
            effect="dark"
          >
            {{ row.level.toUpperCase() }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="category" label="分类" width="120">
        <template #default="{ row }">
          <el-tag size="small" type="info">{{ row.category }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="message" label="消息" min-width="300" />
      <el-table-column prop="created_at" label="时间" width="180">
        <template #default="{ row }">
          <span class="time-text">{{ formatTime(row.created_at) }}</span>
        </template>
      </el-table-column>
      <el-table-column label="操作" width="120" fixed="right">
        <template #default="{ row }">
          <el-button size="small" type="success" plain @click="handleAck(row.id)">
            确认
          </el-button>
        </template>
      </el-table-column>
    </el-table>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { dashboardApi, type Alert } from '@/api'

const alerts = ref<Alert[]>([])
const loading = ref(false)

function levelType(level: string): 'success' | 'warning' | 'danger' | 'info' {
  switch (level) {
    case 'critical':
    case 'error':
      return 'danger'
    case 'warn':
      return 'warning'
    case 'info':
      return 'success'
    default:
      return 'info'
  }
}

function formatTime(ts: string): string {
  if (!ts) return '-'
  const d = new Date(ts)
  return d.toLocaleString()
}

async function fetchAlerts() {
  loading.value = true
  try {
    const res = await dashboardApi.alerts()
    alerts.value = res.data ?? []
  } catch {
    ElMessage.error('加载告警信息失败')
  } finally {
    loading.value = false
  }
}

async function handleAck(id: number) {
  try {
    await dashboardApi.ackAlert(id)
    alerts.value = alerts.value.filter((a) => a.id !== id)
    ElMessage.success('告警已确认')
  } catch {
    ElMessage.error('确认告警失败')
  }
}

async function handleClearAll() {
  try {
    await ElMessageBox.confirm('确定清除所有告警？', '确认', {
      type: 'warning',
    })
    await dashboardApi.clearAlerts()
    alerts.value = []
    ElMessage.success('所有告警已清除')
  } catch {
    // cancelled or error
  }
}

onMounted(() => {
  fetchAlerts()
})
</script>

<style scoped>
.monitor-page {
  color: var(--text-primary);
}
.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 16px;
}
.page-title {
  margin: 0;
  font-size: 22px;
  color: var(--accent);
}
.header-actions {
  display: flex;
  gap: 8px;
}
.time-text {
  font-size: 12px;
  color: var(--text-secondary);
}
</style>
