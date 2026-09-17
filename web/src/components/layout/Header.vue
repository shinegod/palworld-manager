<template>
  <div class="header-bar">
    <el-icon class="collapse-btn" @click="$emit('toggle')" :size="20">
      <Fold v-if="!collapsed" />
      <Expand v-else />
    </el-icon>
    <div class="header-status">
      <span class="status-dot" :class="serverConnected ? 'online' : 'offline'" />
      <span class="server-name">{{ serverName }}</span>
      <el-tag v-if="serverVersion" size="small" type="info" class="version-tag">
        v{{ serverVersion }}
      </el-tag>
    </div>
    <div class="header-spacer" />
    <el-badge :value="playerCount" :hidden="playerCount === 0" type="primary" class="player-badge">
      <el-tag size="default" effect="dark">
        <el-icon><User /></el-icon>
        玩家
      </el-tag>
    </el-badge>
    <el-button text @click="handleLogout">退出</el-button>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { useRouter } from 'vue-router'
import { Fold, Expand, User } from '@element-plus/icons-vue'
import { useServerStore } from '@/stores/server'

defineProps<{ collapsed: boolean }>()
defineEmits<{ toggle: [] }>()

const router = useRouter()
const serverStore = useServerStore()

const serverConnected = computed(() => serverStore.connectionStatus === 'connected')
const serverName = computed(() => serverStore.serverInfo?.servername ?? 'PalManager')
const serverVersion = computed(() => serverStore.serverInfo?.version ?? '')
const playerCount = computed(() => serverStore.onlinePlayers?.length ?? 0)

function handleLogout() {
  localStorage.removeItem('token')
  router.push({ name: 'Login' })
}

onMounted(() => {
  serverStore.fetchInfo()
})
</script>

<style scoped>
.header-bar {
  display: flex;
  align-items: center;
  width: 100%;
  gap: 12px;
}
.collapse-btn {
  cursor: pointer;
  color: var(--text-secondary);
}
.collapse-btn:hover {
  color: var(--accent);
}
.header-status {
  display: flex;
  align-items: center;
  gap: 8px;
}
.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
}
.status-dot.online {
  background: var(--success);
  box-shadow: 0 0 6px var(--success);
}
.status-dot.offline {
  background: var(--danger);
}
.server-name {
  color: var(--text-primary);
  font-weight: 500;
}
.version-tag {
  margin-left: 4px;
}
.header-spacer {
  flex: 1;
}
.player-badge {
  margin-right: 8px;
}
</style>
