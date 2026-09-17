import { defineStore } from 'pinia'
import { ref } from 'vue'
import {
  palhookApi,
  type ServerInfo,
  type RealtimeMetrics,
  type OnlinePlayer,
} from '@/api'

// hook-only: 全部数据来自 PalHook
export const useServerStore = defineStore('server', () => {
  const serverInfo = ref<ServerInfo | null>(null)
  const realtimeMetrics = ref<RealtimeMetrics | null>(null)
  const onlinePlayers = ref<OnlinePlayer[]>([])
  const connectionStatus = ref<'connected' | 'disconnected' | 'connecting'>('disconnected')

  async function fetchInfo() {
    try {
      const h = await palhookApi.health()
      serverInfo.value = {
        version: h.data.version ?? '',
        servername: 'PalHook 服务器',
        description: '',
        worldguid: '',
      }
    } catch {
      serverInfo.value = null
      connectionStatus.value = 'disconnected'
    }
  }

  async function fetchRealtime() {
    try {
      connectionStatus.value = 'connecting'
      const [m, p] = await Promise.all([palhookApi.metrics(), palhookApi.players()])
      const md = m.data
      realtimeMetrics.value = {
        serverfps: md.fps ?? 0,
        currentplayernum: md.player_count ?? 0,
        serverframetime: md.fps > 0 ? Math.round(1000 / md.fps) : 0,
        maxplayernum: 32,
        uptime: md.uptime_sec ?? 0,
        days: Math.floor((md.uptime_sec ?? 0) / 86400),
        serverfpsaverage: md.fps ?? 0,
        basecampnum: 0,
      }
      onlinePlayers.value = (p.data?.players ?? []).map((pl: any) => ({
        name: pl.name ?? '',
        playerId: pl.uid ?? '',
        odss_id: '',
        odss_token: '',
        level: pl.level ?? 0,
        hp: 0,
        maxHp: 0,
        shieldHp: 0,
        maxShieldHp: 0,
        exp: pl.exp ?? 0,
        statusFlags: [],
        ip: '',
        ping: 0,
        location_x: pl.x ?? 0,
        location_y: pl.y ?? 0,
        location_z: pl.z ?? 0,
      }))
      connectionStatus.value = 'connected'
    } catch {
      connectionStatus.value = 'disconnected'
    }
  }

  return {
    serverInfo,
    realtimeMetrics,
    onlinePlayers,
    connectionStatus,
    fetchInfo,
    fetchRealtime,
  }
})
