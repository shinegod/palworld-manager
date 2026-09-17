// 游戏数据: 物品/帕鲁 中英文对照 (从 public/data 加载)
export interface GameItem {
  key: string   // StaticItemId
  nameZh: string
  nameEn: string
}

export interface GamePal {
  key: string   // CharacterID
  nameZh: string
  nameEn: string
}

let itemsCache: GameItem[] | null = null
let palsCache: GamePal[] | null = null

export async function loadItems(): Promise<GameItem[]> {
  if (itemsCache) return itemsCache
  const data = await fetch('/data/items.json').then((r) => r.json())
  const enMap = new Map<string, string>()
  for (const it of data.en ?? []) enMap.set(it.key || it.id, it.name)
  itemsCache = []
  for (const it of data.zh ?? []) {
    const key = it.key || it.id
    itemsCache.push({
      key,
      nameZh: it.name || key,
      nameEn: enMap.get(key) || key,
    })
  }
  // 补齐只有英文没有中文的
  for (const it of data.en ?? []) {
    const key = it.key || it.id
    if (!itemsCache.find((i) => i.key === key)) {
      itemsCache.push({ key, nameZh: it.name || key, nameEn: it.name || key })
    }
  }
  return itemsCache
}

export async function loadPals(): Promise<GamePal[]> {
  if (palsCache) return palsCache
  const data = await fetch('/data/pals.json').then((r) => r.json())
  const en = data.en ?? {}
  const zh = data.zh ?? {}
  palsCache = Object.entries(en)
    .filter(([k]) => !k.startsWith('RAID_') && !k.startsWith('GYM_') && !k.startsWith('BOSS_'))
    .map(([k, v]) => ({ key: k, nameZh: (zh as any)[k] || (v as string), nameEn: v as string }))
  return palsCache
}

export function label(item: GameItem | GamePal): string {
  return item.nameZh + ' (' + item.key + ')'
}