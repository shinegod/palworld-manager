<template>
  <div class="events-page">
    <div class="page-header">
      <h2 class="page-title">事件系统</h2>
      <el-button type="primary" @click="showCreate">创建事件</el-button>
    </div>

    <el-table :data="events ?? []" stripe v-loading="loading" empty-text="暂无事件配置">
      <el-table-column prop="name" label="名称" min-width="150">
        <template #default="{ row }">
          <span class="event-name">{{ row.name }}</span>
        </template>
      </el-table-column>
      <el-table-column prop="description" label="描述" min-width="200" />
      <el-table-column prop="schedule_type" label="调度类型" width="120">
        <template #default="{ row }">
          <el-tag size="small">{{ row.schedule_type }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="schedule_value" label="调度值" width="150" />
      <el-table-column prop="enabled" label="状态" width="100">
        <template #default="{ row }">
          <el-tag :type="row.enabled ? 'success' : 'danger'" size="small">
            {{ row.enabled ? '启用' : '禁用' }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="last_run" label="上次执行" width="180">
        <template #default="{ row }">{{ row.last_run ? formatTime(row.last_run) : '从未' }}</template>
      </el-table-column>
      <el-table-column label="操作" width="200" fixed="right">
        <template #default="{ row }">
          <el-button size="small" type="success" plain @click="handleTrigger(row)">触发</el-button>
          <el-button size="small" type="primary" plain @click="editEvent(row)">编辑</el-button>
          <el-button size="small" type="danger" plain @click="handleDelete(row)">删除</el-button>
        </template>
      </el-table-column>
    </el-table>

    <el-dialog v-model="dialogVisible" :title="editingEvent ? '编辑事件' : '创建事件'" width="600px">
      <el-form :model="form" label-width="120px">
        <el-form-item label="名称" required>
          <el-input v-model="form.name" placeholder="事件名称" />
        </el-form-item>
        <el-form-item label="描述">
          <el-input v-model="form.description" type="textarea" :rows="2" placeholder="可选描述" />
        </el-form-item>
        <el-form-item label="调度类型" required>
          <el-select v-model="form.schedule_type" style="width: 100%">
            <el-option label="单次" value="once" />
            <el-option label="循环" value="recurring" />
            <el-option label="Cron" value="cron" />
          </el-select>
        </el-form-item>
        <el-form-item label="调度值" required>
          <el-input v-model="form.schedule_value" :placeholder="schedulePlaceholder" />
        </el-form-item>
        <el-form-item label="动作" required>
          <el-input v-model="form.actions" type="textarea" :rows="4" placeholder='[{"type":"broadcast","params":{"message":"Hello!"}}]' />
        </el-form-item>
        <el-form-item label="接收者">
          <el-input v-model="form.recipients" placeholder='{"mode":"all"} 或 {"mode":"random","count":3}' />
        </el-form-item>
        <el-form-item label="清理动作">
          <el-input v-model="form.on_end_actions" type="textarea" :rows="2" placeholder="可选结束动作 JSON" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="dialogVisible = false">取消</el-button>
        <el-button type="primary" @click="handleSave" :loading="saving">保存</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { eventApi, type GameEvent } from '@/api'

const events = ref<GameEvent[]>([])
const loading = ref(false)
const dialogVisible = ref(false)
const saving = ref(false)
const editingEvent = ref<GameEvent | null>(null)

const form = ref({
  name: '',
  description: '',
  schedule_type: 'once',
  schedule_value: '',
  actions: '',
  recipients: '',
  on_end_actions: '',
})

const schedulePlaceholder = computed(() => {
  switch (form.value.schedule_type) {
    case 'once': return '2026-12-31T23:59:00Z'
    case 'recurring': return '2h（每2小时）'
    case 'cron': return '0 */6 * * *（每6小时）'
    default: return ''
  }
})

function formatTime(ts: string): string {
  return new Date(ts).toLocaleString()
}

async function fetchEvents() {
  loading.value = true
  try {
    const res = await eventApi.list()
    events.value = res.data ?? []
  } catch {
    ElMessage.error('加载事件列表失败')
  } finally {
    loading.value = false
  }
}

function showCreate() {
  editingEvent.value = null
  form.value = { name: '', description: '', schedule_type: 'once', schedule_value: '', actions: '', recipients: '', on_end_actions: '' }
  dialogVisible.value = true
}

function editEvent(event: GameEvent) {
  editingEvent.value = event
  form.value = {
    name: event.name,
    description: event.description,
    schedule_type: event.schedule_type,
    schedule_value: event.schedule_value,
    actions: event.actions,
    recipients: event.recipients,
    on_end_actions: event.on_end_actions,
  }
  dialogVisible.value = true
}

async function handleSave() {
  if (!form.value.name || !form.value.schedule_value || !form.value.actions) {
    ElMessage.warning('名称、调度值和动作为必填项')
    return
  }
  saving.value = true
  try {
    if (editingEvent.value) {
      await eventApi.update(editingEvent.value.id, form.value)
      ElMessage.success('事件已更新')
    } else {
      await eventApi.create(form.value)
      ElMessage.success('事件已创建')
    }
    dialogVisible.value = false
    await fetchEvents()
  } catch {
    ElMessage.error('保存事件失败')
  } finally {
    saving.value = false
  }
}

async function handleTrigger(event: GameEvent) {
  try {
    await ElMessageBox.confirm(`确定立即触发事件"${event.name}"？`, '确认', { type: 'info' })
    await eventApi.trigger(event.id)
    ElMessage.success('事件已触发')
    await fetchEvents()
  } catch { /* cancelled */ }
}

async function handleDelete(event: GameEvent) {
  try {
    await ElMessageBox.confirm(`确定删除事件"${event.name}"？`, '确认', { type: 'warning' })
    await eventApi.remove(event.id)
    ElMessage.success('事件已删除')
    await fetchEvents()
  } catch { /* cancelled */ }
}

onMounted(() => fetchEvents())
</script>

<style scoped>
.events-page { color: var(--text-primary); }
.page-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 16px; }
.page-title { margin: 0; font-size: 22px; color: var(--accent); }
.event-name { font-weight: 600; }
</style>
