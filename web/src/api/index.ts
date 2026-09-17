import axios, { type AxiosResponse } from 'axios'
import router from '@/router'

const http = axios.create({
  baseURL: '/api',
  timeout: 15000,
})

http.interceptors.request.use((config) => {
  const token = localStorage.getItem('token')
  if (token) {
    config.headers.Authorization = `Bearer ${token}`
  }
  return config
})

http.interceptors.response.use(
  (res: AxiosResponse) => res,
  (err) => {
    if (err.response?.status === 401) {
      localStorage.removeItem('token')
      router.push({ name: 'Login' })
    }
    return Promise.reject(err)
  },
)

// ── Auth ──

export interface LoginReq {
  username: string
  password: string
}

export interface LoginRes {
  token: string
}

export const authApi = {
  login: (data: LoginReq) => http.post<LoginRes>('/auth/login', data),
}

// ── Dashboard ──

export interface RealtimeMetrics {
  serverfps: number
  currentplayernum: number
  serverframetime: number
  maxplayernum: number
  uptime: number
  days: number
  serverfpsaverage: number
  basecampnum: number
}

export interface OnlinePlayer {
  name: string
  playerId: string
  odss_id: string
  odss_token: string
  level: number
  hp: number
  maxHp: number
  shieldHp: number
  maxShieldHp: number
  exp: number
  statusFlags: string[]
  ip: string
  ping: number
  location_x: number
  location_y: number
  location_z: number
}

export interface RealtimeResponse {
  metrics: RealtimeMetrics
  players: OnlinePlayer[]
}

export interface ServerInfo {
  version: string
  servername: string
  description: string
  worldguid: string
}



export interface Alert {
  id: number
  level: string
  category: string
  message: string
  created_at: string
}

export const dashboardApi = {
  realtime: () => http.get<RealtimeResponse>('/dashboard/realtime'),
  info: () => http.get<ServerInfo>('/dashboard/info'),
  alerts: () => http.get<Alert[]>('/dashboard/alerts'),
  ackAlert: (id: number) => http.post('/dashboard/alerts/ack', { id }),
  clearAlerts: () => http.post('/dashboard/alerts/clear'),
}

// ── Server Control ──

export const serverApi = {
  settings: () => http.get<Record<string, unknown>>('/server/settings'),
}

// ── Players ──

export interface PlayerHistoryItem {
  playerId: string
  name: string
  level: number
  lastLogin: string
  lastLogout: string
  totalOnlineTime: number
}

export const playerApi = {
  online: () => http.get<OnlinePlayer[]>('/players/online'),
  history: (limit: number, offset: number) =>
    http.get<PlayerHistoryItem[]>('/players/history', { params: { limit, offset } }),
  kick: (userId: string, message: string) =>
    http.post('/players/kick', { userId, message }),
  ban: (userId: string, message: string) =>
    http.post('/players/ban', { userId, message }),
  unban: (userId: string) => http.post('/players/unban', { userId }),
}

// ── Console ──



// ── Map ──



// ── Events ──

export interface GameEvent {
  id: number
  name: string
  description: string
  enabled: boolean
  schedule_type: string
  schedule_value: string
  actions: string
  recipients: string
  on_end_actions: string
  last_run: string | null
  created_at: string
}

export interface EventCreateReq {
  name: string
  description: string
  schedule_type: string
  schedule_value: string
  actions: string
  recipients: string
  on_end_actions: string
}

export const eventApi = {
  list: () => http.get<GameEvent[]>('/events'),
  create: (data: EventCreateReq) => http.post('/events', data),
  update: (id: number, data: EventCreateReq) => http.put('/events/' + id, data),
  remove: (id: number) => http.delete('/events/' + id),
  trigger: (id: number) => http.post('/events/' + id + '/trigger'),
}

// ── PalHook (在线刷道具/经验/帕鲁/传送) ──

export interface PalHookHealth {
  status: string
  version: string
  pid: number
  initialized: boolean
  engine_found: boolean
}

export const palhookApi = {
  health: () => http.get<PalHookHealth>('/palhook/health', { timeout: 30000 }),
  giveItem: (item_id: string, count: number) =>
    http.post('/palhook/give-item', { item_id, count }, { timeout: 60000 }),
  giveExp: (exp: number) =>
    http.post('/palhook/give-exp', { exp }, { timeout: 60000 }),
  giveMoney: (amount: number) =>
    http.post('/palhook/give-money', { amount }, { timeout: 60000 }),
  spawnPal: (pal_id: string, level: number, capture: boolean) =>
    http.post('/palhook/spawn-pal', { pal_id, level, capture: capture ? 1 : 0 }, { timeout: 90000 }),
  teleport: (x: number, y: number, z: number) =>
    http.post('/palhook/teleport', { x, y, z }, { timeout: 60000 }),
  teleportTo: (to: string, move?: string) =>
    http.post('/palhook/teleport', { to, name: move ?? '' }, { timeout: 60000 }),
  setTechPoints: (tech: number, boss_tech: number) =>
    http.post('/palhook/set-tech-points', { tech, boss_tech }, { timeout: 60000 }),
  setLevel: (level: number) =>
    http.post('/palhook/set-level', { level }, { timeout: 60000 }),
  players: () => http.get<{ players: PalHookPlayer[] }>('/palhook/players', { timeout: 60000 }),
  announce: (message: string) =>
    http.post('/palhook/announce', { message }, { timeout: 60000 }),
  kick: (name: string) =>
    http.post('/palhook/kick', { name }, { timeout: 60000 }),
  metrics: () => http.get<PalHookMetrics>('/palhook/metrics', { timeout: 60000 }),
  chat: (message: string, name?: string) =>
    http.post('/palhook/chat', { message, name: name ?? '' }, { timeout: 60000 }),
}

export interface PalHookPlayer {
  name: string
  uid: string
  level: number
  exp: number
  x: number
  y: number
  z: number
  character: string
  playerstate: string
}

export interface PalHookMetrics {
  status: string
  player_count: number
  fps: number
  uptime_sec: number
  version: string
}

export default http
