<template>
  <el-container class="app-layout">
    <el-aside :width="collapsed ? '64px' : '220px'" class="app-aside">
      <Sidebar :collapsed="collapsed" @toggle="collapsed = !collapsed" />
    </el-aside>
    <el-container>
      <el-header class="app-header" height="56px">
        <Header :collapsed="collapsed" @toggle="collapsed = !collapsed" />
      </el-header>
      <el-main class="app-main">
        <router-view />
      </el-main>
    </el-container>
  </el-container>
</template>

<script setup lang="ts">
import { ref } from 'vue'
import Sidebar from './Sidebar.vue'
import Header from './Header.vue'

const collapsed = ref(false)
</script>

<style scoped>
.app-layout {
  height: 100vh;
  overflow: hidden;
}
.app-aside {
  transition: width 0.3s;
  overflow: hidden;
}
.app-header {
  display: flex;
  align-items: center;
  padding: 0 16px;
}
.app-main {
  overflow-y: auto;
  padding: 20px;
}
</style>
