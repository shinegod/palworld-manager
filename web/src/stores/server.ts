import { defineStore } from 'pinia'
import { ref } from 'vue'
import {
  http,
  type ServerInfo,
  type RealtimeMetrics,
  type RealtimeResponse,
  type OnlinePlayer,
  type PalHookMetrics,
} from '@/api'

// hook-only: 全部数据来自 PalHook
export const useServerStore = defineStore('server', () => {
  const serverInfo = ref<ServerInfo | null>(null)
  const realtimeMetrics = ref<RealtimeMetrics | null>(null)
  const onlinePlayers = ref<OnlinePlayer[]>([])
  const connectionStatus = ref<'connected' | 'disconnected' | 'connecting'>('disconnected')

  async function fetchInfo() {
    try {
      // 服务器名走后端 /dashboard/info (透传 PalHook /health 的 server_name)
      const res = await http.get<ServerInfo>('/dashboard/info')
      serverInfo.value = {
        version: res.data.version ?? '',
        servername: res.data.servername ?? '',
        description: res.data.description ?? '',
      }
    } catch {
      serverInfo.value = null
      connectionStatus.value = 'disconnected'
    }
  }

  async function fetchRealtime() {
    try {
      connectionStatus.value = 'connecting'
      // 后端 /dashboard/realtime 已并行合并 PalHook 的 /metrics + /players,
      // 前端只发一个请求, 少一半往返
      const res = await http.get<RealtimeResponse>('/dashboard/realtime')
      const md = (res.data?.metrics ?? {}) as Partial<PalHookMetrics> & Partial<RealtimeMetrics>
      const fps = md.fps ?? md.serverfps ?? 0
      const uptime = md.uptime_sec ?? md.uptime ?? 0
      realtimeMetrics.value = {
        serverfps: fps,
        currentplayernum: md.player_count ?? md.currentplayernum ?? 0,
        serverframetime: fps > 0 ? Math.round(1000 / fps) : 0,
        maxplayernum: md.maxplayernum ?? 32,
        uptime,
        days: Math.floor(uptime / 86400),
        serverfpsaverage: fps,
        basecampnum: md.basecampnum ?? 0,
      }
      onlinePlayers.value = (res.data?.players ?? []).map((pl: any) => ({
        name: pl.name ?? '',
        playerId: pl.uid ?? pl.playerId ?? '',
        level: pl.level ?? 0,
        exp: pl.exp ?? 0,
        ip: pl.ip ?? '',
        ping: pl.ping ?? 0,
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
