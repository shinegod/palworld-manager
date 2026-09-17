<template>
  <div class="backups-page">
    <h2 class="page-title">备份管理</h2>

    <div class="backup-actions">
      <el-card class="action-card" shadow="never">
        <div class="action-header">
          <el-icon :size="32" color="#22d3ee"><FolderChecked /></el-icon>
          <div>
            <h3>保存世界</h3>
            <p>手动触发服务器世界存档</p>
          </div>
        </div>
        <el-button type="primary" size="large" :loading="saving" @click="saveWorld">
          立即保存
        </el-button>
      </el-card>

      <el-card class="info-card" shadow="never">
        <el-icon :size="24" color="#6b7280"><InfoFilled /></el-icon>
        <div>
          <h4>备份管理</h4>
          <p>
            完整的备份管理功能（定时备份、恢复点、下载）需要 Bridge Agent 模块支持，
            该功能将在后续版本中推出。
          </p>
        </div>
      </el-card>
    </div>

    <div v-if="saveLog.length > 0" class="save-log">
      <h3 class="section-title">最近保存记录</h3>
      <el-table :data="saveLog ?? []" stripe class="dark-table">
        <el-table-column label="时间" width="200">
          <template #default="{ row }">{{ row.time }}</template>
        </el-table-column>
        <el-table-column label="状态" width="120">
          <template #default="{ row }">
            <el-tag :type="row.success ? 'success' : 'danger'" size="small">
              {{ row.success ? '成功' : '失败' }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="message" label="消息" />
      </el-table>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref } from 'vue'
import { FolderChecked, InfoFilled } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'
import http from '@/api'

interface SaveEntry {
  time: string
  success: boolean
  message: string
}

const saving = ref(false)
const saveLog = ref<SaveEntry[]>([])

async function saveWorld() {
  saving.value = true
  try {
    await http.post('/server/save')
    const entry: SaveEntry = {
      time: new Date().toLocaleString(),
      success: true,
      message: '世界保存成功',
    }
    saveLog.value.unshift(entry)
    ElMessage.success('世界保存成功')
  } catch {
    const entry: SaveEntry = {
      time: new Date().toLocaleString(),
      success: false,
      message: '世界保存失败',
    }
    saveLog.value.unshift(entry)
    ElMessage.error('世界保存失败')
  } finally {
    saving.value = false
  }
}
</script>

<style scoped>
.backups-page {
  color: var(--text-primary);
}
.page-title {
  margin: 0 0 16px 0;
  font-size: 22px;
  color: var(--accent);
}
.backup-actions {
  display: flex;
  flex-direction: column;
  gap: 16px;
  margin-bottom: 24px;
}
.action-card {
  background: var(--surface, #1a1a1a);
  border-color: var(--border-color, #2a2a2a);
}
.action-card :deep(.el-card__body) {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
}
.action-header {
  display: flex;
  align-items: center;
  gap: 16px;
}
.action-header h3 {
  margin: 0;
  color: var(--text-primary);
  font-size: 16px;
}
.action-header p {
  margin: 4px 0 0 0;
  color: var(--text-secondary, #a0a0a0);
  font-size: 13px;
}
.info-card {
  background: #111827;
  border-color: var(--border-color, #2a2a2a);
}
.info-card :deep(.el-card__body) {
  display: flex;
  align-items: flex-start;
  gap: 12px;
}
.info-card h4 {
  margin: 0 0 4px 0;
  color: var(--text-primary);
  font-size: 14px;
}
.info-card p {
  margin: 0;
  color: var(--text-secondary, #a0a0a0);
  font-size: 13px;
  line-height: 1.5;
}
.section-title {
  margin: 0 0 12px 0;
  font-size: 16px;
  color: var(--text-primary);
}
.save-log {
  margin-top: 8px;
}
</style>
