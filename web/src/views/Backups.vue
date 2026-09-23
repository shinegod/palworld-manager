<template>
  <div class="backups-page">
    <h2 class="page-title">备份管理</h2>

    <el-alert type="info" :closable="false" style="margin-bottom: 16px">
      <template #title>
        远程模式: 备份 = 从游戏服务器(PalHook)拉取存档打包到面板本地; 恢复/删档 = 推送到游戏服务器并自动重启应用。
        服务器重启由 MCSM 守护自动拉起, 大约需要 1-2 分钟。
      </template>
    </el-alert>

    <el-row :gutter="16">
      <el-col :span="12">
        <el-card shadow="never" class="dark-card" style="margin-bottom: 16px">
          <template #header><b>配置</b></template>
          <el-form label-width="110px">
            <el-form-item label="备份目录">
              <el-input v-model="config.backup_dir" placeholder="data/backups" />
            </el-form-item>
            <el-form-item label="远程存档目录">
              <el-input :model-value="config.save_dir" disabled placeholder="(由游戏服务器自动发现)" />
            </el-form-item>
            <el-form-item>
              <el-button type="primary" :loading="cfgLoading" @click="saveConfig">保存配置</el-button>
              <el-button @click="fetchConfig">刷新</el-button>
            </el-form-item>
          </el-form>
        </el-card>
      </el-col>
      <el-col :span="12">
        <el-card shadow="never" class="dark-card" style="margin-bottom: 16px">
          <template #header><b>操作</b></template>
          <div style="display: flex; gap: 12px; flex-wrap: wrap">
            <el-button type="primary" size="large" :loading="saving" @click="createBackup">
              <el-icon style="margin-right: 6px"><FolderChecked /></el-icon>立即远程备份
            </el-button>
            <el-button type="warning" plain @click="restartServer">重启游戏服务器</el-button>
          </div>
          <p style="color: var(--text-secondary); font-size: 12px; margin-top: 12px">
            备份保存到面板机器的备份目录。恢复/删档会自动重启游戏服务器 (MCSM 守护拉起)。
          </p>
        </el-card>
      </el-col>
    </el-row>

    <el-card shadow="never" class="dark-card" style="margin-bottom: 16px">
      <template #header><b style="color: var(--el-color-danger)">删档 (危险操作)</b></template>
      <div style="display: flex; gap: 12px; flex-wrap: wrap">
        <el-button type="danger" plain @click="wipePlayers">删除全部玩家角色 (保留世界建筑)</el-button>
        <el-button type="danger" @click="wipeWorld">删除整个世界存档 (完全重置)</el-button>
      </div>
      <p style="color: var(--text-secondary); font-size: 12px; margin-top: 12px">
        删档前会自动做一次远程备份。执行后服务器自动重启为全新存档。
      </p>
    </el-card>

    <el-card shadow="never" class="dark-card" style="margin-bottom: 16px">
      <template #header><b>备份列表 (面板本地)</b></template>
      <el-table :data="backups ?? []" stripe empty-text="暂无备份" class="dark-table">
        <el-table-column prop="filename" label="文件名" min-width="220" />
        <el-table-column label="大小" width="120" align="center">
          <template #default="{ row }">{{ formatSize(row.size_bytes) }}</template>
        </el-table-column>
        <el-table-column prop="notes" label="备注" min-width="240" />
        <el-table-column prop="created_at" label="时间" width="180">
          <template #default="{ row }">{{ formatTime(row.created_at) }}</template>
        </el-table-column>
        <el-table-column label="操作" width="220" fixed="right">
          <template #default="{ row }">
            <el-button size="small" type="success" plain @click="restoreBackup(row)">恢复</el-button>
            <el-button size="small" type="primary" plain @click="downloadBackup(row)">下载</el-button>
            <el-button size="small" type="danger" plain @click="deleteBackup(row)">删除</el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <el-card shadow="never" class="dark-card">
      <template #header><b>远程存档文件 ({{ config.save_dir || '未发现' }})</b></template>
      <el-table :data="saveFiles ?? []" stripe empty-text="无法读取远程存档 — 检查 PalHook 连接" class="dark-table">
        <el-table-column prop="path" label="文件" min-width="260" />
        <el-table-column label="大小" width="120" align="center">
          <template #default="{ row }">{{ formatSize(row.size) }}</template>
        </el-table-column>
        <el-table-column label="修改时间" width="180" align="center">
          <template #default="{ row }">{{ formatTime2(row.mtime) }}</template>
        </el-table-column>
      </el-table>
    </el-card>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive, onMounted } from 'vue'
import { FolderChecked } from '@element-plus/icons-vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import http from '@/api'

interface BackupRow {
  id: number
  filename: string
  size_bytes: number
  notes: string
  created_at: string
}

interface SaveFile {
  path: string
  size: number
  mtime: number
}

const config = reactive({ save_dir: '', backup_dir: '' })
const cfgLoading = ref(false)
const saving = ref(false)
const backups = ref<BackupRow[]>([])
const saveFiles = ref<SaveFile[]>([])

function formatSize(bytes: number): string {
  if (!bytes) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB']
  let i = 0
  let v = bytes
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++ }
  return v.toFixed(1) + ' ' + units[i]
}

function formatTime(ts: string): string {
  return ts ? new Date(ts).toLocaleString() : '-'
}

function formatTime2(sec: number): string {
  return sec ? new Date(sec * 1000).toLocaleString() : '-'
}

async function fetchConfig() {
  try {
    const res = await http.get('/backups/config')
    config.save_dir = res.data.save_dir ?? ''
    config.backup_dir = res.data.backup_dir ?? ''
  } catch { /* ignore */ }
}

async function saveConfig() {
  cfgLoading.value = true
  try {
    await http.post('/backups/config', { backup_dir: config.backup_dir })
    ElMessage.success('配置已保存')
    await fetchAll()
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '保存失败')
  } finally {
    cfgLoading.value = false
  }
}

async function createBackup() {
  saving.value = true
  try {
    const res = await http.post('/backups/create', {}, { timeout: 1800000 })
    ElMessage.success('远程备份完成: ' + res.data.filename + ' (' + res.data.files + ' 个文件)')
    await fetchAll()
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '备份失败')
  } finally {
    saving.value = false
  }
}

async function restoreBackup(row: BackupRow) {
  try {
    await ElMessageBox.confirm('确定用 "' + row.filename + '" 覆盖服务器存档？恢复后游戏服务器会自动重启 (1-2分钟)。', '恢复备份', { type: 'warning', confirmButtonText: '恢复并重启', cancelButtonText: '取消' })
  } catch { return }
  try {
    await http.post('/backups/restore', { id: row.id }, { timeout: 1800000 })
    ElMessage.success('存档已上传并应用, 服务器正在重启')
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '恢复失败')
  }
}

async function downloadBackup(row: BackupRow) {
  try {
    const res = await http.get('/backups/download', { params: { id: row.id }, responseType: 'blob', timeout: 300000 })
    const urlObj = window.URL.createObjectURL(new Blob([res.data]))
    const a = document.createElement('a')
    a.href = urlObj
    a.download = row.filename
    a.click()
    window.URL.revokeObjectURL(urlObj)
  } catch {
    ElMessage.error('下载失败')
  }
}

async function deleteBackup(row: BackupRow) {
  try {
    await ElMessageBox.confirm('删除备份 "' + row.filename + '"？', '删除备份', { type: 'warning' })
  } catch { return }
  try {
    await http.post('/backups/delete', { id: row.id })
    ElMessage.success('已删除')
    await fetchAll()
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '删除失败')
  }
}

async function wipePlayers() {
  await doWipe('players', '删除全部玩家角色后, 所有玩家将重新创建角色。服务器会自动重启。确定继续？')
}

async function wipeWorld() {
  await doWipe('world', '删除整个世界存档 = 完全重置服务器。此操作不可撤销！服务器会自动重启。确定继续？')
}

async function doWipe(scope: string, warn: string) {
  try {
    await ElMessageBox.confirm(warn, '删档确认', { type: 'error', confirmButtonText: '继续删档', cancelButtonText: '取消' })
  } catch { return }
  try {
    const res = await http.post('/backups/wipe', { scope }, { timeout: 1800000 })
    ElMessage.success('删档完成, 服务器正在重启为全新存档')
    await fetchAll()
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '删档失败')
  }
}

async function restartServer() {
  try {
    await ElMessageBox.confirm('重启游戏服务器？玩家会断开连接, MCSM 会自动拉起新实例 (1-2分钟)。', '重启服务器', { type: 'warning' })
  } catch { return }
  try {
    await http.post('/backups/restart', {}, { timeout: 30000 })
    ElMessage.success('重启指令已发送, 服务器即将重启')
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '重启失败')
  }
}

async function fetchAll() {
  await fetchConfig()
  try {
    const res = await http.get('/backups/list')
    backups.value = res.data ?? []
  } catch { /* ignore */ }
  try {
    const res = await http.get('/backups/saves')
    saveFiles.value = res.data.files ?? []
    if (res.data.save_dir) config.save_dir = res.data.save_dir
  } catch { /* ignore */ }
}

onMounted(fetchAll)
</script>

<style scoped>
.backups-page { color: var(--text-primary); }
.page-title { margin: 0 0 16px; font-size: 22px; color: var(--accent); }
.dark-card { background: var(--bg-surface, #1a1a1a); border: 1px solid var(--border-color, #333); }
</style>
