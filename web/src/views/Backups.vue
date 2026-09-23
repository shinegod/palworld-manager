<template>
  <div class="backups-page">
    <h2 class="page-title">备份管理</h2>

    <el-alert :type="serverOnline ? 'success' : 'info'" :closable="false" style="margin-bottom: 16px">
      <template #title>
        {{ serverOnline ? '游戏服务器在线 — 恢复/删档前请先在 MCSM 停止服务器' : '游戏服务器离线 — 可安全恢复/删档' }}
      </template>
    </el-alert>

    <el-row :gutter="16">
      <el-col :span="12">
        <el-card shadow="never" class="dark-card" style="margin-bottom: 16px">
          <template #header><b>存档目录配置</b></template>
          <el-form label-width="110px">
            <el-form-item label="存档目录">
              <el-input v-model="config.save_dir" placeholder="/workspace/pal/Pal/Saved/SaveGames" />
            </el-form-item>
            <el-form-item label="备份目录">
              <el-input v-model="config.backup_dir" placeholder="data/backups" />
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
          <el-button type="primary" size="large" :loading="saving" @click="createBackup">
            <el-icon style="margin-right: 6px"><FolderChecked /></el-icon>立即备份存档
          </el-button>
          <p style="color: var(--text-secondary); font-size: 12px; margin-top: 12px">
            备份为 zip 压缩包存入备份目录。恢复与删档操作会强制要求服务器已停止，并且删档前会自动先做一次备份。
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
        删档前会自动备份一次到备份目录。删除玩家角色后，玩家重新进服会创建全新角色；删除世界存档则整个世界重置。操作前请务必在 MCSM 停止游戏服务器。
      </p>
    </el-card>

    <el-card shadow="never" class="dark-card" style="margin-bottom: 16px">
      <template #header><b>备份列表</b></template>
      <el-table :data="backups ?? []" stripe empty-text="暂无备份" class="dark-table">
        <el-table-column prop="filename" label="文件名" min-width="220" />
        <el-table-column label="大小" width="120" align="center">
          <template #default="{ row }">{{ formatSize(row.size_bytes) }}</template>
        </el-table-column>
        <el-table-column prop="notes" label="备注" min-width="240" />
        <el-table-column prop="created_at" label="时间" width="180">
          <template #default="{ row }">{{ formatTime(row.created_at) }}</template>
        </el-table-column>
        <el-table-column label="操作" width="180" fixed="right">
          <template #default="{ row }">
            <el-button size="small" type="success" plain :disabled="serverOnline" @click="restoreBackup(row)">恢复</el-button>
            <el-button size="small" type="danger" plain @click="deleteBackup(row)">删除</el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <el-card shadow="never" class="dark-card">
      <template #header><b>当前存档文件</b></template>
      <el-table :data="saveFiles ?? []" stripe empty-text="存档目录不存在或为空 — 请在配置里设置正确的存档路径" class="dark-table">
        <el-table-column prop="path" label="文件" min-width="260" />
        <el-table-column label="大小" width="120" align="center">
          <template #default="{ row }">{{ formatSize(row.size) }}</template>
        </el-table-column>
        <el-table-column prop="mod_time" label="修改时间" width="180" align="center" />
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
  mod_time: string
}

const config = reactive({ save_dir: '', backup_dir: '' })
const serverOnline = ref(false)
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

async function fetchConfig() {
  try {
    const res = await http.get('/backups/config')
    config.save_dir = res.data.save_dir ?? ''
    config.backup_dir = res.data.backup_dir ?? ''
    serverOnline.value = !!res.data.server_online
  } catch { /* ignore */ }
}

async function saveConfig() {
  cfgLoading.value = true
  try {
    await http.post('/backups/config', { save_dir: config.save_dir, backup_dir: config.backup_dir })
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
    const res = await http.post('/backups/create', {}, { timeout: 300000 })
    ElMessage.success('备份完成: ' + res.data.filename)
    await fetchAll()
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '备份失败')
  } finally {
    saving.value = false
  }
}

async function restoreBackup(row: BackupRow) {
  try {
    await ElMessageBox.confirm('确定用 "' + row.filename + '" 覆盖当前存档？此操作前请确认游戏服务器已停止。', '恢复备份', { type: 'warning', confirmButtonText: '恢复', cancelButtonText: '取消' })
  } catch { return }
  try {
    await http.post('/backups/restore', { id: row.id }, { timeout: 300000 })
    ElMessage.success('恢复完成, 请启动游戏服务器')
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '恢复失败')
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
  await doWipe('players', '删除全部玩家角色后, 所有玩家将重新创建角色。确定继续？')
}

async function wipeWorld() {
  await doWipe('world', '删除整个世界存档 = 完全重置服务器。此操作不可撤销！确定继续？')
}

async function doWipe(scope: string, warn: string) {
  try {
    await ElMessageBox.confirm(warn, '删档确认', { type: 'error', confirmButtonText: '继续删档', cancelButtonText: '取消' })
  } catch { return }
  try {
    const res = await http.post('/backups/wipe', { scope }, { timeout: 300000 })
    ElMessage.success('删档完成, 启动服务器后即为全新存档 (已自动备份: ' + res.data.backup + ')')
    await fetchAll()
  } catch (e: any) {
    ElMessage.error(e.response?.data?.error || '删档失败')
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
  } catch { /* ignore */ }
}

onMounted(fetchAll)
</script>

<style scoped>
.backups-page { color: var(--text-primary); }
.page-title { margin: 0 0 16px; font-size: 22px; color: var(--accent); }
.dark-card { background: var(--bg-surface, #1a1a1a); border: 1px solid var(--border-color, #333); }
</style>
