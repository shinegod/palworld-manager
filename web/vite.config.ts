import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { resolve } from 'path'

export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: {
      '@': resolve(__dirname, 'src'),
    },
  },
  server: {
    port: 3000,
    proxy: {
      '/api': {
        target: 'http://localhost:8080',
        changeOrigin: true,
      },
      '/ws': {
        target: 'ws://localhost:8080',
        ws: true,
      },
    },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    rollupOptions: {
      output: {
        // leaflet 被 @vue-leaflet 动态导入, 默认会打出两份(共300KB)
        // 手动固定成一个 chunk, 顺手把体积大的第三方库拆开便于缓存
        manualChunks: {
          leaflet: ['leaflet', '@vue-leaflet/vue-leaflet'],
          echarts: ['echarts/core', 'echarts/charts', 'echarts/renderers'],
          vendor: ['vue', 'vue-router', 'pinia', 'axios'],
        },
      },
    },
  },
})
