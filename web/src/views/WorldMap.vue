// WorldMap.vue — hook-only 世界地图
<template>
  <div class="worldmap-page">
    <div class="page-header">
      <h2 class="page-title">世界地图</h2>
      <div class="header-controls">
        <el-checkbox v-model="showPlayers" border size="small">玩家</el-checkbox>
        <el-checkbox v-model="showBossTower" border size="small">Boss塔</el-checkbox>
        <el-checkbox v-model="showFastTravel" border size="small">快速旅行</el-checkbox>
        <el-tag type="info" size="small" style="margin-left: 8px">{{ players.length }} 在线</el-tag>
        <el-button type="primary" size="small" @click="fetchPlayers" :loading="loading" style="margin-left: 8px">刷新</el-button>
      </div>
    </div>

    <div class="map-wrapper">
      <l-map
        ref="mapRef"
        style="width: 100%; height: 100%"
        crs="Simple"
        :zoom="2"
        :use-global-leaflet="false"
        :center="[-128, 128]"
        :min-zoom="0"
        :max-zoom="6"
        :options="{ zoomControl: true, attributionControl: false }"
        @mousemove="onMouseMove"
      >
        <l-tile-layer
          url="map/tiles/{z}/{x}/{y}.png"
          :no-wrap="true"
          layer-type="base"
          :options="{ bounds: [[0, 0], [-256, 256]] }"
        />

        <template v-if="showPlayers">
          <l-marker
            v-for="p in players"
            :key="'p-' + p.name"
            :lat-lng="toMapPos(p.location_x, p.location_y)"
          >
            <l-icon :icon-url="playerIcon" :icon-size="[24, 24]" :icon-anchor="[12, 12]" />
            <l-tooltip :options="{ permanent: false, direction: 'top', offset: [0, -12] }">
              <div style="font-weight:bold">{{ p.name }}</div>
              <div>等级: {{ p.level }} | 延迟: {{ Math.round(p.ping || 0) }}ms</div>
            </l-tooltip>
          </l-marker>
        </template>

        <template v-if="showBossTower">
          <l-marker
            v-for="(poi, i) in bossTowers"
            :key="'bt-' + i"
            :lat-lng="toMapPos(poi.x, poi.y)"
          >
            <l-icon :icon-url="bossIcon" :icon-size="[28, 28]" :icon-anchor="[14, 14]" />
            <l-tooltip>{{ poi.name || 'Boss塔' }}</l-tooltip>
          </l-marker>
        </template>

        <template v-if="showFastTravel">
          <l-marker
            v-for="(poi, i) in fastTravels"
            :key="'ft-' + i"
            :lat-lng="toMapPos(poi.x, poi.y)"
          >
            <l-icon :icon-url="ftIcon" :icon-size="[20, 20]" :icon-anchor="[10, 10]" />
            <l-tooltip>{{ poi.name || '快速旅行' }}</l-tooltip>
          </l-marker>
        </template>
      </l-map>

      <div class="coord-overlay">
        X: {{ Math.round(worldCoords[0]) }} Y: {{ Math.round(worldCoords[1]) }}
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'
import 'leaflet/dist/leaflet.css'
import { LMap, LTileLayer, LMarker, LIcon, LTooltip } from '@vue-leaflet/vue-leaflet'
import http from '@/api'

const LANDSCAPE = [349400, 724400, -1099400, -724400]

const playerIcon = 'map/player.webp'
const bossIcon = 'map/boss_tower.webp'
const ftIcon = 'map/fast_travel.webp'

interface MapPlayer {
  name: string
  user_id: string
  level: number
  ping: number
  location_x: number
  location_y: number
}

interface POI { name: string; type: string; x: number; y: number }

const players = ref<MapPlayer[]>([])
const loading = ref(false)
const showPlayers = ref(true)
const showBossTower = ref(true)
const showFastTravel = ref(true)
const worldCoords = ref([0, 0])
const mapRef = ref<any>(null)
const bossTowers = ref<POI[]>([])
const fastTravels = ref<POI[]>([])

function toMapPos(worldX: number, worldY: number): [number, number] {
  if (worldX >= -256 && worldX <= 256) return [worldX, worldY]
  const x = -256 + (256 * (worldX - LANDSCAPE[2])) / (LANDSCAPE[0] - LANDSCAPE[2])
  const y = (256 * (worldY - LANDSCAPE[3])) / (LANDSCAPE[1] - LANDSCAPE[3])
  return [x, y]
}

function fromMapPos(lat: number, lng: number): [number, number] {
  const wx = ((lat + 256) * (LANDSCAPE[0] - LANDSCAPE[2])) / 256 + LANDSCAPE[2]
  const wy = (lng * (LANDSCAPE[1] - LANDSCAPE[3])) / 256 + LANDSCAPE[3]
  return [wx, wy]
}

function onMouseMove(e: any) {
  if (e.latlng) {
    worldCoords.value = fromMapPos(e.latlng.lat, e.latlng.lng)
  }
}

async function fetchPlayers() {
  loading.value = true
  try {
    const res2 = await http.get<any>('/palhook/players', { timeout: 60000 })
    players.value = (res2.data?.players ?? []).map((p: any) => ({
      name: p.name ?? '',
      user_id: p.uid ?? '',
      level: p.level ?? 0,
      ping: 0,
      location_x: p.x ?? 0,
      location_y: p.y ?? 0,
    }))
  } catch {
    players.value = []
  } finally {
    loading.value = false
  }
}

async function loadPOI() {
  try {
    const res = await fetch('map/points.json')
    const data = await res.json()
    if (Array.isArray(data.boss_tower)) {
      bossTowers.value = data.boss_tower.map((coords: number[]) => ({ name: 'Boss塔', type: 'boss_tower', x: coords[0], y: coords[1] }))
    }
    if (Array.isArray(data.fast_travel)) {
      fastTravels.value = data.fast_travel.map((coords: number[]) => ({ name: '快速旅行', type: 'fast_travel', x: coords[0], y: coords[1] }))
    }
  } catch { /* POI not available */ }
}

let timer: ReturnType<typeof setInterval> | null = null

onMounted(() => {
  fetchPlayers()
  loadPOI()
  timer = setInterval(fetchPlayers, 20000)
})

onUnmounted(() => {
  if (timer) clearInterval(timer)
})
</script>

<style scoped>
.worldmap-page {
  color: var(--text-primary);
  height: calc(100vh - 120px);
  display: flex;
  flex-direction: column;
}
.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 12px;
  flex-wrap: wrap;
  gap: 8px;
}
.page-title { margin: 0; font-size: 22px; color: var(--accent); }
.header-controls { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.map-wrapper {
  flex: 1;
  position: relative;
  border-radius: 8px;
  overflow: hidden;
  border: 1px solid var(--border-color, #2a2a2a);
}
.coord-overlay {
  position: absolute;
  bottom: 8px;
  left: 8px;
  z-index: 1000;
  background: rgba(0,0,0,0.7);
  color: #22d3ee;
  padding: 4px 10px;
  border-radius: 4px;
  font-family: 'Fira Code', monospace;
  font-size: 12px;
}
:deep(.leaflet-container) { background: #0a0e14; }
:deep(.leaflet-control-zoom a) { background: #1a1a1a; color: #e0e0e0; border-color: #2a2a2a; }
</style>
