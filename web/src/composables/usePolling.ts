// 轮询: 页面切到后台时自动暂停, 回到前台立刻补一次
// 原先各页面自己 setInterval, 标签页隐藏也照样打接口, 白烧服务器
import { onMounted, onUnmounted } from 'vue'

export interface PollingOptions {
  /** 挂载后是否立即执行一次 (默认 true) */
  immediate?: boolean
}

export function usePolling(
  fn: () => void | Promise<void>,
  intervalMs: number,
  options: PollingOptions = {},
) {
  const { immediate = true } = options
  let timer: ReturnType<typeof setInterval> | null = null

  function start() {
    if (timer) return
    timer = setInterval(fn, intervalMs)
  }

  function stop() {
    if (!timer) return
    clearInterval(timer)
    timer = null
  }

  function onVisibilityChange() {
    if (document.hidden) {
      stop()
    } else {
      void fn() // 回到前台先补一次, 避免看到过期数据
      start()
    }
  }

  onMounted(() => {
    if (immediate) void fn()
    if (!document.hidden) start()
    document.addEventListener('visibilitychange', onVisibilityChange)
  })

  onUnmounted(() => {
    stop()
    document.removeEventListener('visibilitychange', onVisibilityChange)
  })

  return { start, stop }
}
