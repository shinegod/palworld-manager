import { createRouter, createWebHistory, type RouteRecordRaw } from 'vue-router'

const routes: RouteRecordRaw[] = [
  {
    path: '/login',
    name: 'Login',
    component: () => import('@/views/Login.vue'),
    meta: { requiresAuth: false },
  },
  {
    path: '/',
    component: () => import('@/components/layout/AppLayout.vue'),
    meta: { requiresAuth: true },
    children: [
      { path: '', redirect: '/dashboard' },
      { path: 'dashboard', name: 'Dashboard', component: () => import('@/views/Dashboard.vue') },
      { path: 'players', name: 'Players', component: () => import('@/views/Players.vue') },
      { path: 'worldmap', name: 'WorldMap', component: () => import('@/views/WorldMap.vue') },
      { path: 'guilds', name: 'Guilds', component: () => import('@/views/Guilds.vue') },
      { path: 'events', name: 'Events', component: () => import('@/views/Events.vue') },
      { path: 'anti-cheat', name: 'AntiCheat', component: () => import('@/views/AntiCheat.vue') },
      { path: 'settings', name: 'Settings', component: () => import('@/views/Settings.vue') },
      { path: 'backups', name: 'Backups', component: () => import('@/views/Backups.vue') },
      { path: 'monitor', name: 'Monitor', component: () => import('@/views/Monitor.vue') },
    ],
  },
]

const router = createRouter({
  history: createWebHistory(),
  routes,
})

router.beforeEach((to, _from, next) => {
  const token = localStorage.getItem('token')
  if (to.meta.requiresAuth !== false && !token) {
    next({ name: 'Login' })
  } else {
    next()
  }
})

export default router
