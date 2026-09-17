<template>
  <el-card class="gauge-card" shadow="never">
    <div class="gauge-card__header">
      <span class="gauge-card__title">{{ title }}</span>
    </div>
    <div ref="chartRef" class="gauge-card__chart"></div>
    <div class="gauge-card__value" :style="{ color }">
      {{ displayValue }}{{ unit }}
    </div>
  </el-card>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted, shallowRef } from 'vue'
import * as echarts from 'echarts/core'
import { GaugeChart } from 'echarts/charts'
import { CanvasRenderer } from 'echarts/renderers'

echarts.use([GaugeChart, CanvasRenderer])

const props = withDefaults(defineProps<{
  title: string
  value: number
  max?: number
  unit?: string
  color?: string
  icon?: string
}>(), {
  max: 100,
  unit: '',
  color: '#22d3ee',
  icon: '',
})

const chartRef = ref<HTMLElement>()
const chart = shallowRef<echarts.ECharts>()

const displayValue = computed(() => {
  if (Number.isInteger(props.value)) return props.value
  return Math.round(props.value * 10) / 10
})

const ratio = computed(() =>
  props.max > 0 ? Math.min(props.value / props.max, 1) : 0,
)

function buildOption(): echarts.EChartsCoreOption {
  return {
    series: [
      {
        type: 'gauge',
        startAngle: 225,
        endAngle: -45,
        radius: '90%',
        center: ['50%', '55%'],
        min: 0,
        max: props.max,
        progress: {
          show: true,
          width: 10,
          roundCap: true,
          itemStyle: { color: props.color },
        },
        axisLine: {
          lineStyle: { width: 10, color: [[1, '#2a2a2a']] },
        },
        axisTick: { show: false },
        splitLine: { show: false },
        axisLabel: { show: false },
        pointer: { show: false },
        title: { show: false },
        detail: { show: false },
        data: [{ value: ratio.value * props.max }],
        animationDuration: 800,
      },
    ],
  }
}

onMounted(() => {
  if (!chartRef.value) return
  chart.value = echarts.init(chartRef.value)
  chart.value.setOption(buildOption())
})

watch(
  () => [props.value, props.max, props.color],
  () => {
    chart.value?.setOption(buildOption())
  },
)

function handleResize() {
  chart.value?.resize()
}

onMounted(() => {
  window.addEventListener('resize', handleResize)
})

onUnmounted(() => {
  window.removeEventListener('resize', handleResize)
  chart.value?.dispose()
})
</script>

<style scoped>
.gauge-card {
  text-align: center;
  height: 100%;
}
.gauge-card__header {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  margin-bottom: 4px;
}
.gauge-card__icon {
  font-size: 16px;
}
.gauge-card__title {
  font-size: 13px;
  color: var(--text-secondary);
}
.gauge-card__chart {
  width: 100%;
  height: 100px;
}
.gauge-card__value {
  font-size: 22px;
  font-weight: 700;
  margin-top: -8px;
}
</style>
