<template>
  <div class="sidebar-wrap">
    <div class="sidebar-logo" @click="$emit('toggle')">
      <span v-if="!collapsed" class="logo-text">幻兽管家</span>
      <span v-else class="logo-text-sm">幻兽</span>
    </div>
    <el-menu
      :default-active="route.path"
      :collapse="collapsed"
      router
      class="sidebar-menu"
    >
      <el-menu-item index="/dashboard">
        <el-icon><Odometer /></el-icon>
        <template #title>仪表盘</template>
      </el-menu-item>
      <el-menu-item index="/players">
        <el-icon><User /></el-icon>
        <template #title>玩家管理</template>
      </el-menu-item>
      <el-menu-item index="/worldmap">
        <el-icon><MapLocation /></el-icon>
        <template #title>世界地图</template>
      </el-menu-item>
      <el-menu-item index="/guilds">
        <el-icon><Flag /></el-icon>
        <template #title>公会管理</template>
      </el-menu-item>
      <el-menu-item index="/events">
        <el-icon><Calendar /></el-icon>
        <template #title>事件系统</template>
      </el-menu-item>
      <el-menu-item index="/anti-cheat">
        <el-icon><Warning /></el-icon>
        <template #title>反作弊</template>
      </el-menu-item>
      <el-menu-item index="/settings">
        <el-icon><Setting /></el-icon>
        <template #title>设置</template>
      </el-menu-item>
      <el-menu-item index="/backups">
        <el-icon><FolderOpened /></el-icon>
        <template #title>备份管理</template>
      </el-menu-item>
      <el-menu-item index="/monitor">
        <el-icon><DataLine /></el-icon>
        <template #title>监控中心</template>
      </el-menu-item>
    </el-menu>
  </div>
</template>

<script setup lang="ts">
import { useRoute } from 'vue-router'
import {
  Odometer, User, MapLocation, Flag,
  Calendar, Warning, Setting, FolderOpened, DataLine,
} from '@element-plus/icons-vue'

defineProps<{ collapsed: boolean }>()
defineEmits<{ toggle: [] }>()

const route = useRoute()
</script>

<style scoped>
.sidebar-wrap {
  height: 100%;
  display: flex;
  flex-direction: column;
}
.sidebar-logo {
  height: 56px;
  display: flex;
  align-items: center;
  justify-content: center;
  cursor: pointer;
  border-bottom: 1px solid var(--border-color);
}
.logo-text {
  font-size: 18px;
  font-weight: 700;
  color: var(--accent);
}
.logo-text-sm {
  font-size: 18px;
  font-weight: 700;
  color: var(--accent);
}
.sidebar-menu {
  flex: 1;
  border-right: none !important;
}
</style>
