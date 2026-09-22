/*
 * PalHook - 幻兽帕鲁服务端注入库 v0.4.0
 * 通过LD_PRELOAD加载到PalServer进程中
 *
 * 编译: gcc -shared -fPIC -O2 -o libpalhook.so palhook.c -lpthread -ldl
 * 使用: LD_PRELOAD=/path/to/libpalhook.so ./PalServer.sh
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/uio.h>   /* process_vm_readv: 读无效地址返回EFAULT而非触发SIGSEGV */

#define PALHOOK_PORT 13335
#define PALHOOK_VERSION "0.9.16"
#define MAX_REQUEST 16384
#define MAX_RESPONSE 262144
#define LOG_PREFIX "[PalHook] "

/* ========== 全局状态 ========== */

static int g_running = 0;
static int g_initialized = 0;
static int g_init_pid = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* PalServer二进制段信息 */
typedef struct {
    uintptr_t start;
    uintptr_t end;
    char perms[8];
} MemSegment;

#define MAX_SEGMENTS 64
static MemSegment g_bin_segments[MAX_SEGMENTS];
static int g_bin_segment_count = 0;

static uintptr_t g_base_addr = 0;   /* ELF加载基地址 (第一个段) */
static size_t g_text_size = 0;

/* 所有进程rw-p区域 (包括匿名mmap, heap等) */
#define MAX_RW_REGIONS 1024
static uintptr_t g_all_rw[MAX_RW_REGIONS][2];
static int g_all_rw_count = 0;
static size_t g_total_rw_size = 0;

/* UE全局对象指针 */
static void** g_GEngine = NULL;
static void*  g_ConsoleManager = NULL;

/* 认证密码 (从PalWorldSettings.ini读取AdminPassword) */
static char g_admin_password[128] = {0};
/* 服务器名 (从同一配置读取ServerName, 面板展示用) */
static char g_server_name[256] = {0};

/* 测试VPS加载基址 (所有硬编码地址以它为基准换算成偏移, 生产环境ASLR安全) */
#define VPS_BASE_ADDR 0x200000

/*
 * vtable符号虚拟地址 (readelf -s, 测试VPS基址0x200000下的绝对值)
 * _ZTV布局: [0]=offset-to-top, [8]=RTTI(null), [16]=first vfunc
 * UObject->vtable 指向 _ZTV + 16
 *
 * 关键: 幻兽帕鲁的GEngine实际类型是UPalGameEngine，不是UGameEngine！
 * 生产环境: 实际地址 = g_base_addr + 偏移 (同一版本二进制布局一致)
 */
#define OFF_ZTV_UPalGameEngine   (0x1a20da8 - VPS_BASE_ADDR)
#define OFF_ZTV_UGameEngine      (0x20ef378 - VPS_BASE_ADDR)
#define OFF_ZTV_FConsoleManager  (0x1a258d8 - VPS_BASE_ADDR)

#define OFF_VTABLE_UPalGameEngine  (OFF_ZTV_UPalGameEngine + 16)
#define OFF_VTABLE_UGameEngine     (OFF_ZTV_UGameEngine + 16)
#define OFF_VTABLE_FConsoleManager (OFF_ZTV_FConsoleManager + 16)

/* ProcessEvent合法地址范围 (text段): 测试VPS绝对范围 0x43c4000~0xbcc4000 换算成偏移 */
#define OFF_PE_LO (0x43c4000 - VPS_BASE_ADDR)
#define OFF_PE_HI (0xbcc4000 - VPS_BASE_ADDR)
#define OFF_VTABLE_UCHEATMANAGER (0x2066418 - VPS_BASE_ADDR)

static uintptr_t VTABLE_UPalGameEngine = 0;
static uintptr_t VTABLE_UGameEngine = 0;
static uintptr_t VTABLE_FConsoleManager = 0;

/* UClass vtable (实测, 测试VPS基址0x200000): 原生C++类=0x1a50e08, BP生成类=0x204f060
 * 生产环境ASLR基址可能不同 -> 统一用 基址+偏移 (同一游戏版本二进制布局一致) */
#define OFF_VTABLE_UCLASS_NATIVE  (0x1a50e08 - VPS_BASE_ADDR)
#define OFF_VTABLE_UCLASS_BP      (0x204f060 - VPS_BASE_ADDR)
#define OFF_VTABLE_UFUNCTION      (0x1a51218 - VPS_BASE_ADDR)
#define OFF_FNAMEPOOL             (0xc0b24b0 - VPS_BASE_ADDR)

static int is_uclass_vtable(uintptr_t vt) {
    if (g_base_addr > 0) {
        return vt == g_base_addr + OFF_VTABLE_UCLASS_NATIVE || vt == g_base_addr + OFF_VTABLE_UCLASS_BP;
    }
    /* 兜底: 基址未知时按测试VPS绝对地址 (仅测试环境) */
    return vt == 0x1a50e08 || vt == 0x204f060;
}

/* 合理vtable范围: 镜像内的rodata段 (ASLR安全: 相对基址判断) */
static int is_plausible_vtable(uintptr_t vt) {
    if (g_base_addr > 0) {
        return vt >= g_base_addr && vt < g_base_addr + g_text_size + 0x4000000;
    }
    /* 兜底: 测试VPS绝对范围 */
    return vt >= 0x1000000 && vt < 0x4000000;
}

/* 玩家Character缓存 (校验通过直接复用, 省20秒全扫描) */
static uintptr_t g_cache_character = 0;

/* 帧计数器 (nanosleep hook内更新, /metrics用) */
static volatile int g_frame_count = 0;
static volatile time_t g_frame_ts = 0;
static volatile int g_fps = 0;
static time_t g_start_ts = 0;

/* 扫描统计 */
static size_t g_scan_bytes = 0;
static size_t g_scan_ptrs_checked = 0;
static size_t g_scan_deref_ok = 0;
static size_t g_scan_deref_fail = 0;

/* ========== 安全内存访问 ========== */

/* 内存区域白名单方案: 读之前先查地址是否在已知rw区域内,
 * 不用SIGSEGV handler (多线程下会吞掉游戏线程的栈探测fault导致死循环) */
static pthread_mutex_t g_maps_lock = PTHREAD_MUTEX_INITIALIZER;

/* 二分查找地址是否在某个rw区域内 (g_all_rw按start升序, 锁保护)
 * 也检查二进制自身段 (rodata的vtable等, r--p也在此列表) */
static int region_contains(uintptr_t addr) {
    pthread_mutex_lock(&g_maps_lock);
    int found = 0;
    int lo = 0, hi = g_all_rw_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (addr >= g_all_rw[mid][0] && addr < g_all_rw[mid][1]) { found = 1; break; }
        if (addr < g_all_rw[mid][0]) hi = mid - 1;
        else lo = mid + 1;
    }
    if (!found) {
        for (int i = 0; i < g_bin_segment_count; i++) {
            if (addr >= g_bin_segments[i].start && addr < g_bin_segments[i].end) { found = 1; break; }
        }
    }
    pthread_mutex_unlock(&g_maps_lock);
    return found;
}

/* 安全读取任意地址的8字节。
 *
 * 为什么不能直接 *(uintptr_t*)addr:
 * region_contains() 查的是 parse_proc_maps() 的快照, 而 UE 运行中会持续
 * mmap/munmap。"检查通过" 到 "实际读取" 之间那块内存可能已被 unmap ——
 * 裸读即 SIGSEGV, 直接带走整个游戏进程 (历史上的 Signal 11 就是这么来的)。
 *
 * process_vm_readv 读自己的进程: 地址无效时返回 EFAULT, 不产生信号。
 * region_contains 保留作为廉价预筛, 挡掉绝大多数明显无效的地址, 避免每次都进内核。 */
static int safe_read_ptr(uintptr_t addr, uintptr_t* out) {
    if (addr < 0x10000) return -1;
    /* 注意: 不能要求8字节对齐 —— FName表条目从entry+2读(2字节对齐),
     * UPROPERTY偏移也可能是任意值, process_vm_readv本身就支持非对齐地址 */
    if (!region_contains(addr) || !region_contains(addr + 7)) return -1;

    uintptr_t tmp = 0;
    struct iovec local = { .iov_base = &tmp, .iov_len = sizeof(tmp) };
    struct iovec remote = { .iov_base = (void*)addr, .iov_len = sizeof(tmp) };
    if (process_vm_readv(g_init_pid ? g_init_pid : getpid(), &local, 1, &remote, 1, 0)
            != (ssize_t)sizeof(tmp)) {
        return -1;
    }
    *out = tmp;
    return 0;
}

/* 区域列表快照 (扫描器用, 避免与parse_proc_maps并发撕裂) */
static int snapshot_rw(uintptr_t (*dst)[2], int max_n) {
    pthread_mutex_lock(&g_maps_lock);
    int n = g_all_rw_count < max_n ? g_all_rw_count : max_n;
    memcpy(dst, g_all_rw, sizeof(uintptr_t) * 2 * (size_t)n);
    pthread_mutex_unlock(&g_maps_lock);
    return n;
}

/* 空实现, 仅保留函数名兼容36处历史调用点。
 * 安全性不再依赖信号处理器: safe_read_ptr 用 process_vm_readv 从根上
 * 消除了越界读, 无需捕获 SIGSEGV。注入库里装全局 SIGSEGV handler 会干扰
 * UE 自己的崩溃处理链, 不能这么做。 */
static void install_segv_handler(void) {
    (void)0;
}

/* ========== 日志 ========== */

static void palhook_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, LOG_PREFIX);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(args);
}

/* ========== 认证 ========== */

static void finalize_password_value(const char* raw, int raw_len);

/* 从ini文件读取AdminPassword, 成功返回1 */
static int read_ini_password(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        /* ServerName: 与AdminPassword同一行, 且在它前面 */
        if (!g_server_name[0]) {
            char* sn = strstr(line, "ServerName=\"");
            if (sn) {
                sn += 12;
                char* end = strchr(sn, '"');
                if (end && (end - sn) < 255 && end > sn) {
                    int rl = (int)(end - sn);
                    memcpy(g_server_name, sn, (size_t)rl);
                    g_server_name[rl] = '\0';
                    palhook_log("loaded ServerName from %s (%d chars)", path, (int)strlen(g_server_name));
                }
            }
        }
        char* ap = strstr(line, "AdminPassword=\"");
        if (!ap) continue;
        ap += 15;
        char* end = strchr(ap, '"');
        if (end && (end - ap) < 127 && end > ap) {
            char raw[128];
            int rl = (int)(end - ap);
            memcpy(raw, ap, (size_t)rl);
            raw[rl] = '\0';
            finalize_password_value(raw, rl);
            palhook_log("loaded AdminPassword from %s (%d chars)", path, (int)strlen(g_admin_password));
            fclose(f);
            return 1;
        }
        break;
    }
    fclose(f);
    return 0;
}

static int base64_decode(const char* in, int in_len, char* out, int out_max);

/* 值整理: 游戏缓存里的值可能是base64编码, 能解出可打印ASCII就用解码结果 */
static void finalize_password_value(const char* raw, int raw_len) {
    if (raw_len <= 0 || raw_len >= 127) return;
    /* 尝试base64解码 (值全为base64字符且有=结尾或长度%4==0) */
    int b64ish = 1;
    for (int i = 0; i < raw_len; i++) {
        char c = raw[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) {
            b64ish = 0;
            break;
        }
    }
    if (b64ish && (raw[raw_len - 1] == '=' || raw_len % 4 == 0)) {
        char decoded[128] = {0};
        int dl = base64_decode(raw, raw_len, decoded, 127);
        int printable = dl > 0;
        for (int i = 0; i < dl; i++) {
            if ((unsigned char)decoded[i] < 0x20 || (unsigned char)decoded[i] > 0x7E) {
                printable = 0;
                break;
            }
        }
        if (printable) {
            memcpy(g_admin_password, decoded, (size_t)dl + 1);
            palhook_log("loaded AdminPassword from memory (base64 decoded, %d chars)", dl);
            return;
        }
    }
    memcpy(g_admin_password, raw, (size_t)raw_len);
    g_admin_password[raw_len] = '\0';
    palhook_log("loaded AdminPassword from memory (%d chars)", raw_len);
}

/* 内存扫描GConfig缓存里的ini原文 (游戏启动后配置原文常驻内存, 无需知道文件路径)
 * 同时提取AdminPassword与ServerName */
static int utf16_to_utf8(const uint16_t* in, int in_len, char* out, int out_size);

static int scan_memory_for_admin_password(void) {
    if (g_all_rw_count <= 0) return 0;
    static const char pat_narrow[] = "AdminPassword=\"";
    static const char pat_narrow_sn[] = "ServerName=\"";
    static const uint8_t pat_wide[] = {
        'A',0,'d',0,'m',0,'i',0,'n',0,'P',0,'a',0,'s',0,'s',0,
        'w',0,'o',0,'r',0,'d',0,'=',0,'"',0
    };
    static const uint8_t pat_wide_sn[] = {
        'S',0,'e',0,'r',0,'v',0,'e',0,'r',0,'N',0,'a',0,'m',0,'e',0,'=',0,'"',0
    };
    uintptr_t segs[MAX_RW_REGIONS][2];
    int seg_count = snapshot_rw(segs, MAX_RW_REGIONS);

    /* 第一遍: 宽字符模式 (UE FConfigFile原文是UTF-16 FString, 值未编码) */
    for (int seg = 0; seg < seg_count; seg++) {
        uint8_t* base = (uint8_t*)segs[seg][0];
        size_t size = segs[seg][1] - segs[seg][0];
        if (size < 64) continue;
        /* ServerName (支持中文: UTF-16转UTF-8) */
        if (!g_server_name[0]) {
            uint8_t* sf = (uint8_t*)memmem(base, size, pat_wide_sn, sizeof(pat_wide_sn));
            if (sf) {
                uint8_t* p = sf + sizeof(pat_wide_sn);
                uint16_t ubuf[128];
                int un = 0;
                while (un < 127 && (uintptr_t)(p + 1) < segs[seg][1]) {
                    uint16_t ch = (uint16_t)(p[0] | (p[1] << 8));
                    if (ch == '"' || ch == 0) break;
                    ubuf[un++] = ch;
                    p += 2;
                }
                if (un > 0 && utf16_to_utf8(ubuf, un, g_server_name, sizeof(g_server_name)) > 0) {
                    palhook_log("loaded ServerName from memory wide (%s)", g_server_name);
                }
            }
        }
        /* AdminPassword */
        if (!g_admin_password[0]) {
            uint8_t* wf = (uint8_t*)memmem(base, size, pat_wide, sizeof(pat_wide));
            if (wf) {
                uint8_t* p = wf + sizeof(pat_wide);
                char out[128]; int o = 0;
                while (o < 127 && (uintptr_t)(p + 1) < segs[seg][1]) {
                    uint16_t ch = (uint16_t)(p[0] | (p[1] << 8));
                    if (ch == '"' || ch == 0) break;
                    if (ch >= 0x80) break;
                    out[o++] = (char)ch;
                    p += 2;
                }
                out[o] = 0;
                if (o > 0) {
                    finalize_password_value(out, o);
                }
            }
        }
    }

    /* 第二遍: 窄字节模式 (某些缓存放未编码ASCII) */
    for (int seg = 0; seg < seg_count; seg++) {
        uint8_t* base = (uint8_t*)segs[seg][0];
        size_t size = segs[seg][1] - segs[seg][0];
        if (size < 64) continue;
        /* ServerName */
        if (!g_server_name[0]) {
            uint8_t* sf = (uint8_t*)memmem(base, size, pat_narrow_sn, sizeof(pat_narrow_sn) - 1);
            if (sf) {
                uint8_t* p = sf + sizeof(pat_narrow_sn) - 1;
                int o = 0;
                while (o < 255 && (uintptr_t)p < segs[seg][1] && *p != '"' && *p != 0) {
                    g_server_name[o++] = (char)*p++;
                }
                g_server_name[o] = 0;
                if (o > 0) palhook_log("loaded ServerName from memory narrow (%s)", g_server_name);
            }
        }
        /* AdminPassword */
        if (!g_admin_password[0]) {
            uint8_t* found = (uint8_t*)memmem(base, size, pat_narrow, sizeof(pat_narrow) - 1);
            if (found) {
                uint8_t* p = found + sizeof(pat_narrow) - 1;
                char out[128]; int o = 0;
                while (o < 127 && (uintptr_t)p < segs[seg][1] && *p != '"' && *p != 0) {
                    out[o++] = (char)*p++;
                }
                out[o] = 0;
                if (o > 0) {
                    finalize_password_value(out, o);
                }
            }
        }
    }
    return g_admin_password[0] ? 1 : 0;
}


/* 自动寻找PalWorldSettings.ini并读取AdminPassword
 * 顺序: 环境变量 -> 工作目录/常见路径 -> find搜索 -> 内存扫描 */
static void load_admin_password(void) {
    if (g_admin_password[0]) return;

    /* 1. 环境变量覆盖 */
    const char* env = getenv("PALHOOK_PASSWORD");
    if (env && env[0]) {
        strncpy(g_admin_password, env, 127);
        palhook_log("loaded password from PALHOOK_PASSWORD env");
        return;
    }

    /* 2. 候选路径: 硬编码常见位置 + 进程工作目录 (每台机器路径不同, cwd最可靠) */
    char cand[16][640];
    int nc = 0;
    const char* fixed[] = {
        "/palworld/Pal/Saved/Config/LinuxServer/PalWorldSettings.ini",
        "./Pal/Saved/Config/LinuxServer/PalWorldSettings.ini",
        "/workspace/Pal/Saved/Config/LinuxServer/PalWorldSettings.ini",
        NULL
    };
    for (int i = 0; fixed[i] && nc < 16; i++) {
        snprintf(cand[nc], sizeof(cand[nc]), "%s", fixed[i]);
        nc++;
    }
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(cand[nc], sizeof(cand[nc]), "%s/Pal/Saved/Config/LinuxServer/PalWorldSettings.ini", cwd);
        nc++;
    }
    char link[512];
    ssize_t ll = readlink("/proc/self/cwd", link, sizeof(link) - 1);
    if (ll > 0 && nc < 16) {
        link[ll] = 0;
        snprintf(cand[nc], sizeof(cand[nc]), "%s/Pal/Saved/Config/LinuxServer/PalWorldSettings.ini", link);
        nc++;
    }
    for (int i = 0; i < nc && !g_admin_password[0]; i++) {
        read_ini_password(cand[i]);
    }

    /* 3. find 搜索常见根目录 (最多9层深, 取前5个结果) */
    if (!g_admin_password[0]) {
        FILE* fp = popen(
            "find /palworld /home /workspace /data /srv /opt /app /server /game /gameserver "
            "-maxdepth 9 -name PalWorldSettings.ini 2>/dev/null | head -5", "r");
        if (fp) {
            char line[640];
            while (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\n")] = 0;
                if (line[0] && read_ini_password(line)) break;
            }
            pclose(fp);
        }
    }

    /* 4. 内存扫描 (游戏启动60秒后配置缓存在内存里, delayed_init会重试一次) */
    if (!g_admin_password[0]) {
        scan_memory_for_admin_password();
    }

    if (!g_admin_password[0]) {
        palhook_log("WARNING: no AdminPassword found yet (API unprotected until found)");
    }
}

/* Base64解码 */
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static int base64_decode(const char* in, int in_len, char* out, int out_max) {
    int j = 0;
    for (int i = 0; i + 3 < in_len; i += 4) {
        int a = b64_val(in[i]), b = b64_val(in[i+1]);
        int c = (in[i+2] != '=') ? b64_val(in[i+2]) : 0;
        int d = (in[i+3] != '=') ? b64_val(in[i+3]) : 0;
        if (a < 0 || b < 0) break;
        int n = (a << 18) | (b << 12) | (c << 6) | d;
        if (j < out_max) out[j++] = (n >> 16) & 0xFF;
        if (j < out_max && in[i+2] != '=') out[j++] = (n >> 8) & 0xFF;
        if (j < out_max && in[i+3] != '=') out[j++] = n & 0xFF;
    }
    if (j < out_max) out[j] = 0;
    return j;
}

/* 检查HTTP Basic Auth, 返回0=通过, -1=失败 */
/* 定长比较: 不因首个不同字节就短路, 避免泄漏密码前缀 */
static int pwd_equal(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (la == lb) ? 0 : 1;
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

/* 裸内存原语: 无密码时绝不开放 (任意读写=进程RCE) */
static int is_dangerous_path(const char* path) {
    static const char* deny[] = {
        "/writemem", "/readmem", "/call-function", "/cheat",
        "/search-bytes", "/find-value", "/scan",
        "/maps", "/meminfo", "/find-class", "/find-vtable",
        "/find-players", "/dump-guilds", "/get-npc-manager", NULL
    };
    for (int i = 0; deny[i]; i++) {
        size_t n = strlen(deny[i]);
        if (strncmp(path, deny[i], n) == 0 && (path[n] == 0 || path[n] == '?')) return 1;
    }
    return 0;
}

static int check_auth(int fd, const char* req) {
    /* 懒重试: 密码还没找到时(游戏刚启动), 隔几秒重扫一次 */
    static time_t g_pw_retry_ts = 0;
    if (!g_admin_password[0] && time(NULL) - g_pw_retry_ts > 10) {
        g_pw_retry_ts = time(NULL);
        load_admin_password();
    }

    char path[64] = {0};
    sscanf(req, "%*s %63s", path);

    /* /health不需要认证 */
    if (strcmp(path, "/health") == 0 || strcmp(path, "/") == 0) return 0;

    if (!g_admin_password[0]) {
        /* 无密码: 普通接口放行(兼容未设AdminPassword的服务器), 但内存原语一律拒绝 */
        if (is_dangerous_path(path)) {
            const char* resp = "HTTP/1.1 403 Forbidden\r\n"
                "Content-Length: 58\r\n\r\n"
                "{\"error\":\"memory endpoints need AdminPassword to be set\"}\r\n";
            send(fd, resp, strlen(resp), MSG_NOSIGNAL);
            return -1;
        }
        return 0;
    }

    const char* auth = strstr(req, "Authorization: Basic ");
    if (!auth) {
        const char* resp = "HTTP/1.1 401 Unauthorized\r\n"
            "WWW-Authenticate: Basic realm=\"PalHook\"\r\n"
            "Content-Length: 14\r\n\r\nUnauthorized\r\n";
        send(fd, resp, strlen(resp), MSG_NOSIGNAL);
        return -1;
    }
    auth += 21;
    char b64[256] = {0};
    int bi = 0;
    while (auth[bi] && auth[bi] != '\r' && auth[bi] != '\n' && bi < 255) {
        b64[bi] = auth[bi]; bi++;
    }
    b64[bi] = '\0';

    char decoded[256] = {0};
    base64_decode(b64, bi, decoded, 255);

    /* decoded = "user:password", 只验证password部分 */
    char* colon = strchr(decoded, ':');
    const char* pwd = colon ? colon + 1 : decoded;

    if (!pwd_equal(pwd, g_admin_password)) {
        const char* resp = "HTTP/1.1 401 Unauthorized\r\n"
            "Content-Length: 14\r\n\r\nUnauthorized\r\n";
        send(fd, resp, strlen(resp), MSG_NOSIGNAL);
        return -1;
    }
    return 0;
}

/* ========== 内存映射解析 ========== */

static int parse_proc_maps(void) {
    pthread_mutex_lock(&g_maps_lock);
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) {
        pthread_mutex_unlock(&g_maps_lock);
        return -1;
    }

    g_bin_segment_count = 0;
    g_all_rw_count = 0;
    g_total_rw_size = 0;
    g_base_addr = 0;
    g_text_size = 0;

    char line[512];
    int found_first = 0;
    int found_text = 0;

    while (fgets(line, sizeof(line), f)) {
        unsigned long start, end;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;

        /* 收集PalServer二进制的段 */
        if (strstr(line, "PalServer-Linux-Shipping")) {
            if (g_bin_segment_count < MAX_SEGMENTS) {
                g_bin_segments[g_bin_segment_count].start = start;
                g_bin_segments[g_bin_segment_count].end = end;
                strncpy(g_bin_segments[g_bin_segment_count].perms, perms, 7);
                g_bin_segment_count++;
            }
            if (!found_first) {
                g_base_addr = start;
                found_first = 1;
            }
            if (strstr(perms, "r-xp") && !found_text) {
                g_text_size = end - start;
                found_text = 1;
            }
        }

        /* 收集所有rw-p段 (匿名+文件映射都要) */
        if (strstr(perms, "rw-p") && g_all_rw_count < MAX_RW_REGIONS) {
            size_t seg_size = end - start;
            /* 跳过太小的段(<4KB)和太大的段(>2GB，可能是映射错误) */
            if (seg_size >= 4096 && seg_size < 0x80000000UL) {
                g_all_rw[g_all_rw_count][0] = start;
                g_all_rw[g_all_rw_count][1] = end;
                g_all_rw_count++;
                g_total_rw_size += seg_size;
            }
        }
    }
    fclose(f);
    pthread_mutex_unlock(&g_maps_lock);
    return found_first ? 0 : -1;
}

/* ========== UE对象发现 ========== */

static int discover_ue_globals(void) {
    palhook_log("=== UE globals discovery start ===");
    palhook_log("  base=0x%lx text=%zu MB", g_base_addr, g_text_size/(1024*1024));
    palhook_log("  binary segments: %d", g_bin_segment_count);
    palhook_log("  all rw regions: %d (total %zu MB)", g_all_rw_count, g_total_rw_size/(1024*1024));

    /* ASLR安全: 基址+偏移 (g_base_addr在parse_proc_maps中设置, 调用前必>0) */
    if (g_base_addr == 0) {
        palhook_log("  ERROR: base addr unknown, discovery aborted");
        return -1;
    }
    VTABLE_UPalGameEngine = g_base_addr + OFF_VTABLE_UPalGameEngine;
    VTABLE_UGameEngine = g_base_addr + OFF_VTABLE_UGameEngine;
    VTABLE_FConsoleManager = g_base_addr + OFF_VTABLE_FConsoleManager;

    palhook_log("  vtable UPalGameEngine = 0x%lx (primary target)", VTABLE_UPalGameEngine);
    palhook_log("  vtable UGameEngine = 0x%lx (fallback)", VTABLE_UGameEngine);
    palhook_log("  vtable FConsoleManager = 0x%lx", VTABLE_FConsoleManager);

    /* 验证vtable地址处的内容 */
    uintptr_t vt_content = 0;
    if (safe_read_ptr(VTABLE_UPalGameEngine, &vt_content) == 0) {
        palhook_log("  vtable UPalGameEngine[0] = 0x%lx (readable OK)", vt_content);
    } else {
        palhook_log("  ERROR: vtable UPalGameEngine not readable!");
        g_initialized = 1;
        return -1;
    }

    install_segv_handler();

    g_GEngine = NULL;
    g_ConsoleManager = NULL;
    g_scan_bytes = 0;
    g_scan_ptrs_checked = 0;
    g_scan_deref_ok = 0;
    g_scan_deref_fail = 0;

    /*
     * 扫描策略:
     * GEngine是一个全局指针 -> 指向UGameEngine对象 -> 对象的[0]是vtable
     * 扫描所有rw段，找到值为某个指针P的位置，
     * 其中P指向的第一个qword == VTABLE_UGameEngine
     */

    palhook_log("  scanning %d rw regions for GEngine/ConsoleManager...", g_all_rw_count);

    uintptr_t segs[MAX_RW_REGIONS][2];
    int seg_count = snapshot_rw(segs, MAX_RW_REGIONS);
    for (int seg = 0; seg < seg_count; seg++) {
        uintptr_t scan_start = segs[seg][0];
        uintptr_t scan_end = segs[seg][1];
        size_t seg_size = scan_end - scan_start;
        uintptr_t* ptr = (uintptr_t*)scan_start;
        size_t count = seg_size / sizeof(uintptr_t);

        g_scan_bytes += seg_size;

        for (size_t i = 0; i < count; i++) {
            uintptr_t val = ptr[i];

            /* 快速过滤: 值必须看起来像一个合理的指针 */
            if (val < 0x100000 || val > 0x800000000000UL) continue;

            g_scan_ptrs_checked++;

            uintptr_t first_qword = 0;
            if (safe_read_ptr(val, &first_qword) != 0) {
                g_scan_deref_fail++;
                continue;
            }
            g_scan_deref_ok++;

            if ((first_qword == VTABLE_UPalGameEngine || first_qword == VTABLE_UGameEngine) && !g_GEngine) {
                g_GEngine = (void**)&ptr[i];
                palhook_log("  >>> FOUND GEngine at 0x%lx -> obj=0x%lx (vt=0x%lx, region %d: 0x%lx-0x%lx)",
                    (uintptr_t)g_GEngine, val, first_qword, seg, scan_start, scan_end);
            }
            if (first_qword == VTABLE_FConsoleManager && !g_ConsoleManager) {
                g_ConsoleManager = (void*)val;
                palhook_log("  >>> FOUND ConsoleManager obj at 0x%lx (in rw region %d: 0x%lx-0x%lx)",
                    val, seg, scan_start, scan_end);
            }

            if (g_GEngine && g_ConsoleManager) goto done;
        }
    }

done:
    g_initialized = 1;
    palhook_log("=== discovery complete ===");
    palhook_log("  scanned: %zu MB, ptrs checked: %zu, deref ok: %zu, deref fail: %zu",
        g_scan_bytes/(1024*1024), g_scan_ptrs_checked, g_scan_deref_ok, g_scan_deref_fail);
    palhook_log("  GEngine=%p ConsoleManager=%p",
        g_GEngine ? *g_GEngine : NULL, g_ConsoleManager);

    if (g_GEngine) {
        /* 尝试进一步探索: GEngine -> GameInstance -> World */
        uintptr_t engine_obj = (uintptr_t)*g_GEngine;
        palhook_log("  GEngine object at 0x%lx", engine_obj);

        /* UGameEngine继承UEngine, UObject大小约48字节
         * GameViewport通常在偏移较前的位置 */
        for (int off = 0; off < 2048; off += 8) {
            uintptr_t field = 0;
            if (safe_read_ptr(engine_obj + off, &field) != 0) break;
            if (field == 0) continue;

            /* 检查field是否指向UWorld对象 (vtable在rodata段) */
            uintptr_t maybe_vt = 0;
            if (safe_read_ptr(field, &maybe_vt) != 0) continue;

            /* vtable应该在rodata段 (0x200000 - 0x43c3000) */
            if (maybe_vt >= g_base_addr && maybe_vt < g_base_addr + 0x4000000) {
                palhook_log("  GEngine+0x%x -> 0x%lx (vt=0x%lx)", off, field, maybe_vt);
            }
        }
    }

    return (g_GEngine != NULL) ? 0 : -1;
}

/* ========== JSON工具 ========== */

static int json_get_string(const char* json, const char* key, char* value, size_t value_size) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos != '"') return -1;
    pos++;
    size_t i = 0;
    while (*pos && *pos != '"' && i < value_size - 1) {
        value[i++] = *pos++;
    }
    value[i] = '\0';
    return 0;
}

static int json_get_int(const char* json, const char* key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ' || *pos == '\t') pos++;
    return atoi(pos);
}

static double json_get_double(const char* json, const char* key, double def) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* pos = strstr(json, search);
    if (!pos) return def;
    pos += strlen(search);
    while (*pos == ' ' || *pos == '\t') pos++;
    return atof(pos);
}

/* ========== HTTP服务器 ========== */

static void http_respond(int fd, int status, const char* ctype, const char* body) {
    char hdr[512];
    int blen = body ? (int)strlen(body) : 0;
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "Connection: close\r\n\r\n",
        status, status == 200 ? "OK" : (status == 404 ? "Not Found" : "Error"),
        ctype, blen);
    send(fd, hdr, hlen, MSG_NOSIGNAL);
    if (body && blen > 0) send(fd, body, blen, MSG_NOSIGNAL);
}

static void json_ok(int fd, const char* json) { http_respond(fd, 200, "application/json", json); }
static void json_err(int fd, int status, const char* msg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    http_respond(fd, status, "application/json", buf);
}

static int read_body(int fd, const char* req, char* body, size_t bsize) {
    const char* cl = strstr(req, "Content-Length:");
    if (!cl) return 0;
    int clen = atoi(cl + 15);
    if (clen <= 0 || clen >= (int)bsize) return 0;
    const char* bs = strstr(req, "\r\n\r\n");
    if (!bs) return 0;
    bs += 4;
    int got = (int)strlen(bs);
    if (got >= clen) { memcpy(body, bs, clen); body[clen] = 0; return clen; }
    memcpy(body, bs, got);
    int rem = clen - got;
    int n = recv(fd, body + got, rem, 0);
    if (n > 0) got += n;
    body[got] = 0;
    return got;
}

/* ========== API Handlers ========== */

static void api_health(int fd) {
    char buf[1280];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"ok\",\"version\":\"%s\",\"pid\":%d,"
        "\"server_name\":\"%s\","
        "\"base_addr\":\"0x%lx\",\"text_size_mb\":%zu,"
        "\"rw_regions\":%d,\"rw_total_mb\":%zu,"
        "\"engine_found\":%s,\"console_found\":%s,\"initialized\":%s,"
        "\"scan_stats\":{\"bytes_mb\":%zu,\"ptrs\":%zu,\"deref_ok\":%zu,\"deref_fail\":%zu}}",
        PALHOOK_VERSION, getpid(),
        g_server_name[0] ? g_server_name : "",
        g_base_addr, g_text_size/(1024*1024),
        g_all_rw_count, g_total_rw_size/(1024*1024),
        g_GEngine ? "true" : "false",
        g_ConsoleManager ? "true" : "false",
        g_initialized ? "true" : "false",
        g_scan_bytes/(1024*1024), g_scan_ptrs_checked,
        g_scan_deref_ok, g_scan_deref_fail);
    json_ok(fd, buf);
}

static void api_maps(int fd) {
    if (g_bin_segment_count == 0) parse_proc_maps();

    char* buf = (char*)malloc(MAX_RESPONSE);
    if (!buf) { json_err(fd, 500, "malloc failed"); return; }

    int pos = snprintf(buf, MAX_RESPONSE,
        "{\"base\":\"0x%lx\",\"text_mb\":%zu,"
        "\"bin_segments\":%d,\"rw_regions\":%d,\"rw_total_mb\":%zu,"
        "\"binary\":[",
        g_base_addr, g_text_size/(1024*1024),
        g_bin_segment_count, g_all_rw_count, g_total_rw_size/(1024*1024));

    for (int i = 0; i < g_bin_segment_count && pos < MAX_RESPONSE - 200; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, MAX_RESPONSE - pos,
            "{\"start\":\"0x%lx\",\"end\":\"0x%lx\",\"size_kb\":%zu,\"perms\":\"%s\"}",
            g_bin_segments[i].start, g_bin_segments[i].end,
            (g_bin_segments[i].end - g_bin_segments[i].start) / 1024,
            g_bin_segments[i].perms);
    }

    pos += snprintf(buf + pos, MAX_RESPONSE - pos, "],\"rw_regions\":[");

    /* 只输出前50个rw区域避免response太大 */
    int show = g_all_rw_count < 50 ? g_all_rw_count : 50;
    for (int i = 0; i < show && pos < MAX_RESPONSE - 200; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, MAX_RESPONSE - pos,
            "{\"start\":\"0x%lx\",\"end\":\"0x%lx\",\"size_kb\":%zu}",
            g_all_rw[i][0], g_all_rw[i][1],
            (g_all_rw[i][1] - g_all_rw[i][0]) / 1024);
    }

    snprintf(buf + pos, MAX_RESPONSE - pos, "]}");
    json_ok(fd, buf);
    free(buf);
}

static void api_scan(int fd) {
    palhook_log("manual scan triggered via API");

    if (parse_proc_maps() != 0) {
        json_err(fd, 500, "cannot parse /proc/self/maps");
        return;
    }
    discover_ue_globals();

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"scan_complete\","
        "\"base\":\"0x%lx\",\"text_mb\":%zu,"
        "\"rw_regions\":%d,\"rw_total_mb\":%zu,"
        "\"engine\":\"%p\",\"console\":\"%p\","
        "\"engine_found\":%s,\"console_found\":%s,"
        "\"scan_bytes_mb\":%zu,\"ptrs_checked\":%zu}",
        g_base_addr, g_text_size/(1024*1024),
        g_all_rw_count, g_total_rw_size/(1024*1024),
        g_GEngine ? *g_GEngine : NULL,
        g_ConsoleManager,
        g_GEngine ? "true" : "false",
        g_ConsoleManager ? "true" : "false",
        g_scan_bytes/(1024*1024), g_scan_ptrs_checked);
    json_ok(fd, buf);
}

static void api_meminfo(int fd) {
    char buf[2048];
    char vmrss[64] = "?", vmsize[64] = "?", threads[64] = "?";
    FILE* f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "VmRSS:", 6) == 0) sscanf(line + 6, "%63s", vmrss);
            if (strncmp(line, "VmSize:", 7) == 0) sscanf(line + 7, "%63s", vmsize);
            if (strncmp(line, "Threads:", 8) == 0) sscanf(line + 8, "%63s", threads);
        }
        fclose(f);
    }
    snprintf(buf, sizeof(buf),
        "{\"vm_rss_kb\":\"%s\",\"vm_size_kb\":\"%s\",\"threads\":%s,\"pid\":%d}",
        vmrss, vmsize, threads, getpid());
    json_ok(fd, buf);
}

/* API: 读取进程内存 (调试用) */
static void api_readmem(int fd, const char* req) {
    /* 从URL参数解析: /readmem?addr=0x1234&count=16 */
    const char* addr_str = strstr(req, "addr=");
    const char* count_str = strstr(req, "count=");

    if (!addr_str) {
        json_err(fd, 400, "addr parameter required, e.g. /readmem?addr=0x22ef378&count=16");
        return;
    }

    uintptr_t addr = strtoull(addr_str + 5, NULL, 0);
    int count = count_str ? atoi(count_str + 6) : 16;
    if (count < 1) count = 1;
    if (count > 256) count = 256;

    install_segv_handler();

    char* buf = (char*)malloc(32768);
    if (!buf) { json_err(fd, 500, "malloc failed"); return; }

    int pos = snprintf(buf, 32768,
        "{\"addr\":\"0x%lx\",\"count\":%d,\"qwords\":[", addr, count);

    for (int i = 0; i < count && pos < 32000; i++) {
        uintptr_t val = 0;
        uintptr_t target = addr + (uintptr_t)i * 8;
        int ok = (safe_read_ptr(target, &val) == 0);

        if (i > 0) buf[pos++] = ',';
        if (ok) {
            pos += snprintf(buf + pos, 32768 - pos,
                "{\"off\":%d,\"addr\":\"0x%lx\",\"val\":\"0x%lx\"}", i*8, target, val);
        } else {
            pos += snprintf(buf + pos, 32768 - pos,
                "{\"off\":%d,\"addr\":\"0x%lx\",\"val\":\"FAULT\"}", i*8, target);
        }
    }

    snprintf(buf + pos, 32768 - pos, "]}");
    json_ok(fd, buf);
    free(buf);
}

static void api_help(int fd) {
    json_ok(fd,
        "{\"endpoints\":["
        "{\"method\":\"GET\",\"path\":\"/health\",\"desc\":\"健康检查+扫描统计\"},"
        "{\"method\":\"GET\",\"path\":\"/meminfo\",\"desc\":\"进程内存信息\"},"
        "{\"method\":\"GET\",\"path\":\"/maps\",\"desc\":\"内存映射详情\"},"
        "{\"method\":\"POST\",\"path\":\"/scan\",\"desc\":\"扫描UE全局对象\"},"
        "{\"method\":\"GET\",\"path\":\"/readmem\",\"desc\":\"读进程内存(addr,count)\"},"
        "{\"method\":\"GET\",\"path\":\"/find-vtable\",\"desc\":\"搜索指定vtable的对象(vt)\"},"
        "{\"method\":\"GET\",\"path\":\"/search-bytes\",\"desc\":\"搜索字节模式(hex,max)\"},"
        "{\"method\":\"GET\",\"path\":\"/fname\",\"desc\":\"解析FName(idx)\"},"
        "{\"method\":\"GET\",\"path\":\"/help\",\"desc\":\"API列表\"},"
        "{\"method\":\"GET\",\"path\":\"/find-class\",\"desc\":\"按类名搜索对象(name,max)\"},"
        "{\"method\":\"GET\",\"path\":\"/find-players\",\"desc\":\"发现在线玩家(state/inv/ctrl)\"},"
        "{\"method\":\"POST\",\"path\":\"/give-item\",\"desc\":\"刷道具(item_id,count,inv)\"},"
        "{\"method\":\"POST\",\"path\":\"/give-exp\",\"desc\":\"加经验(exp) v3真实坐标\"},"
        "{\"method\":\"POST\",\"path\":\"/give-money\",\"desc\":\"加金币(amount,ctrl)\"},"
        "{\"method\":\"POST\",\"path\":\"/teleport\",\"desc\":\"传送(x,y,z,ctrl)\"},"
        "{\"method\":\"POST\",\"path\":\"/spawn-pal\",\"desc\":\"刷帕鲁(pal_id,level,capture,x,y,z)\"},"
        "{\"method\":\"GET\",\"path\":\"/get-npc-manager\",\"desc\":\"获取NPCManager(调试)\"},"
        "{\"method\":\"POST\",\"path\":\"/set-exp\",\"desc\":\"设置经验/等级(exp,level)\"},"
        "{\"method\":\"POST\",\"path\":\"/cheat\",\"desc\":\"作弊命令(cmd,ctrl)\"},"
        "{\"method\":\"POST\",\"path\":\"/set-tech-points\",\"desc\":\"设置科技点(tech,boss_tech)\"}"
        "]}");
}

/* FNamePool全局变量地址 (运行时发现) */
static uintptr_t g_FNamePool = 0;

/* 前置声明 */
static int resolve_fname(int idx, char* out, int out_size);
static int find_fname_index(const char* target_name);
#define FPROP_OFFSET_NEXT     0x20
#define FPROP_OFFSET_NAME     0x28
#define FPROP_OFFSET_ELEMSIZE 0x38
#define FPROP_OFFSET_INTERNAL 0x48

static uintptr_t resolve_ctrl(const char* body);
static uintptr_t get_pal_utility(void);
static int actor_get_location(uintptr_t actor, double* out);
static uintptr_t find_property_in_struct(uintptr_t ustruct, const char* prop_name);

/* 玩家列表缓存 (定义在v0.9.0面板接口区, 这里前置声明供teleport用) */
#define MAX_PLAYER_CACHE 16
typedef struct {
    uintptr_t character;
    uintptr_t playerstate;
    uintptr_t controller;
    char name[64];
    char uid[64];
    char ip[64];
    char platform[32];
    float ping;
    int level;
    int64_t exp;
    double x, y, z;
} PlayerEntry;
static PlayerEntry g_players[MAX_PLAYER_CACHE];
static int g_players_count = 0;
static time_t g_players_ts = 0;
static int collect_all_players(void);
static uintptr_t find_gamestate(void);
static int get_connected_playerstates(uintptr_t* out, int max_n);
static void fill_player_details(void);
static int call_and_wait(void* obj, void* ufunc, void* params, int timeout_ms, int* waited_out);
static int prop_get_name(uintptr_t prop, char* out, int out_size);
static int prop_get_size(uintptr_t prop);
static int prop_get_offset(uintptr_t prop);
static uintptr_t find_player_character(void);

/* API: 在所有rw段搜索字节模式 */
static void api_search_bytes(int fd, const char* req) {
    const char* hex_str = strstr(req, "hex=");
    if (!hex_str) {
        json_err(fd, 400, "hex parameter required, e.g. /search-bytes?hex=08004e6f6e65&max=10");
        return;
    }
    hex_str += 4;

    /* 解析hex字符串 */
    uint8_t pattern[64];
    int pat_len = 0;
    for (int i = 0; hex_str[i] && hex_str[i] != '&' && pat_len < 64; i += 2) {
        char byte_str[3] = { hex_str[i], hex_str[i+1], 0 };
        if (!hex_str[i+1] || hex_str[i+1] == '&') break;
        pattern[pat_len++] = (uint8_t)strtoul(byte_str, NULL, 16);
    }
    if (pat_len < 2) {
        json_err(fd, 400, "pattern too short, need at least 2 bytes");
        return;
    }

    const char* max_str = strstr(req, "max=");
    int max_results = max_str ? atoi(max_str + 4) : 20;
    if (max_results < 1) max_results = 1;
    if (max_results > 100) max_results = 100;

    if (g_all_rw_count == 0) parse_proc_maps();
    install_segv_handler();

    palhook_log("search-bytes: pattern=%d bytes, scanning %d rw regions", pat_len, g_all_rw_count);

    char* buf = (char*)malloc(65536);
    if (!buf) { json_err(fd, 500, "malloc failed"); return; }

    int pos = snprintf(buf, 65536, "{\"pattern_len\":%d,\"results\":[", pat_len);
    int found = 0;

    for (int seg = 0; seg < g_all_rw_count && found < max_results; seg++) {
        uintptr_t scan_start = g_all_rw[seg][0];
        uintptr_t scan_end = g_all_rw[seg][1];
        size_t seg_size = scan_end - scan_start;
        if (seg_size < (size_t)pat_len) continue;

        const uint8_t* mem = (const uint8_t*)scan_start;
        for (size_t i = 0; i <= seg_size - pat_len && found < max_results; i++) {
            if (mem[i] == pattern[0] && memcmp(mem + i, pattern, pat_len) == 0) {
                uintptr_t match_addr = scan_start + i;
                if (found > 0) buf[pos++] = ',';
                /* 输出匹配地址和后续16字节的hex */
                pos += snprintf(buf + pos, 65536 - pos,
                    "{\"addr\":\"0x%lx\",\"region\":%d", match_addr, seg);
                /* 显示上下文字节 */
                pos += snprintf(buf + pos, 65536 - pos, ",\"context\":\"");
                int ctx = (i + 32 <= seg_size) ? 32 : (int)(seg_size - i);
                for (int j = 0; j < ctx && pos < 65000; j++) {
                    pos += snprintf(buf + pos, 65536 - pos, "%02x", mem[i + j]);
                }
                pos += snprintf(buf + pos, 65536 - pos, "\"}");
                found++;
            }
        }
    }

    /* 也搜索rodata段 (FNamePool blocks可能在mmap区域) */
    /* 搜索text段前面的rodata */
    if (found < max_results && g_base_addr > 0) {
        uintptr_t ro_start = g_base_addr;  /* 0x200000 */
        uintptr_t ro_end = g_base_addr + g_text_size + 0x4000000; /* 包含rodata */
        /* 不搜rodata了太大了 */
    }

    snprintf(buf + pos, 65536 - pos, "],\"total_found\":%d}", found);
    json_ok(fd, buf);
    free(buf);
}

/* 尝试发现FNamePool */
static int discover_fnamepool(void) {
    /*
     * UE5 FNameEntry: uint16 header + string (no null terminator in most cases)
     * header >> 6 = string length, header & 1 = bIsWide
     * Low 6 bits contain hash/other info
     *
     * Block[0] starts with entries for built-in names:
     * Entry 0: "None" (len=4), then "ByteProperty" (len=12), "IntProperty" (len=11), etc.
     *
     * Strategy: search for "None" followed within ~20 bytes by "ByteProperty"
     * This combination is unique to FNamePool blocks
     */

    palhook_log("discovering FNamePool...");
    install_segv_handler();

    /* Search all rw regions for "None" string followed by "ByteProperty" */
    const uint8_t none_str[] = "None";
    const uint8_t byte_prop[] = "ByteProperty";

    for (int seg = 0; seg < g_all_rw_count; seg++) {
        uintptr_t start = g_all_rw[seg][0];
        uintptr_t end = g_all_rw[seg][1];
        size_t size = end - start;
        if (size < 64) continue;

        const uint8_t* mem = (const uint8_t*)start;
        for (size_t i = 0; i < size - 32; i++) {
            /* Look for "None" at this position */
            if (memcmp(mem + i, none_str, 4) != 0) continue;

            /* Check header before "None": 2 bytes before, header>>6 should be 4 */
            if (i < 2) continue;
            uint16_t h = *(uint16_t*)(mem + i - 2);
            if ((h >> 6) != 4) continue;

            /* Now look for "ByteProperty" within next 20 bytes */
            for (size_t j = i + 4; j < i + 20 && j + 12 <= size; j++) {
                if (memcmp(mem + j, byte_prop, 12) != 0) continue;
                /* Verify header before "ByteProperty" */
                if (j < 2) continue;
                uint16_t h2 = *(uint16_t*)(mem + j - 2);
                if ((h2 >> 6) != 12) continue;

                /* Found it! Block[0] starts at header of "None" entry */
                uintptr_t block0_start = start + i - 2;
                palhook_log("  found FName block: 'None' at 0x%lx, 'ByteProperty' at 0x%lx",
                    start + i, start + j);
                palhook_log("  Block[0] = 0x%lx", block0_start);

                /* Now find pointer to block0_start in BSS/rw regions */
                for (int bseg = 0; bseg < g_all_rw_count; bseg++) {
                    uintptr_t bs = g_all_rw[bseg][0];
                    uintptr_t be = g_all_rw[bseg][1];
                    uintptr_t* ptr = (uintptr_t*)bs;
                    size_t cnt = (be - bs) / sizeof(uintptr_t);
                    for (size_t k = 0; k < cnt; k++) {
                        if (ptr[k] == block0_start) {
                            uintptr_t blocks_ptr = (uintptr_t)&ptr[k];
                            g_FNamePool = blocks_ptr - 16; /* -16 for Lock + CurrentBlock/Cursor */
                            palhook_log("  FOUND FNamePool: Blocks[0] ptr at 0x%lx", blocks_ptr);
                            palhook_log("  FNamePool at 0x%lx", g_FNamePool);

                            /* Verify by reading CurrentBlock */
                            uintptr_t cb_val = 0;
                            if (safe_read_ptr(g_FNamePool + 8, &cb_val) == 0) {
                                uint32_t cur_block = cb_val & 0xFFFFFFFF;
                                uint32_t cur_cursor = (cb_val >> 32) & 0xFFFFFFFF;
                                palhook_log("  CurrentBlock=%u, CurrentByteCursor=%u", cur_block, cur_cursor);
                                if (cur_block > 0 && cur_block < 10000) {
                                    return 0;
                                }
                            }
                            /* Verification failed, keep searching */
                            g_FNamePool = 0;
                        }
                    }
                }

                /* If no pointer found in BSS, the Blocks array might be at a different offset */
                palhook_log("  WARNING: Block[0] found but no Blocks[] pointer in BSS");
            }
        }
    }

    palhook_log("  FNamePool not found");
    return -1;
}

/* API: 解析FName索引到字符串 */
static void api_fname(int fd, const char* req) {
    const char* idx_str = strstr(req, "idx=");
    if (!idx_str) {
        json_err(fd, 400, "idx parameter required, e.g. /fname?idx=350893");
        return;
    }
    int fname_idx = atoi(idx_str + 4);

    if (g_FNamePool == 0) {
        if (parse_proc_maps() == 0) discover_fnamepool();
    }

    if (g_FNamePool == 0) {
        json_err(fd, 503, "FNamePool not discovered yet");
        return;
    }

    /*
     * FNamePool layout:
     * +0x00: Lock (8 bytes)
     * +0x08: CurrentBlock(u32) + CurrentByteCursor(u32)
     * +0x10: Blocks[0], Blocks[1], ...
     *
     * FName index: block = idx >> 16, offset = idx & 0xFFFF
     * Entry address = Blocks[block] + offset * 2
     *
     * FNameEntry: uint16 header, then string
     * len = header >> 6, is_wide = header & 1
     */

    int block = fname_idx >> 16;
    int offset = fname_idx & 0xFFFF;

    install_segv_handler();

    uintptr_t blocks_array = g_FNamePool + 16;
    uintptr_t block_ptr = 0;
    if (safe_read_ptr(blocks_array + block * 8, &block_ptr) != 0 || block_ptr == 0) {
        json_err(fd, 404, "FName block not accessible");
        return;
    }

    uintptr_t entry_addr = block_ptr + (uintptr_t)offset * 2;

    uintptr_t raw = 0;
    if (safe_read_ptr(entry_addr, &raw) != 0) {
        json_err(fd, 500, "cannot read FNameEntry");
        return;
    }

    uint16_t header = *(uint16_t*)entry_addr;
    int name_len = header >> 6;
    int is_wide = header & 1;

    char name[512] = {0};
    if (name_len > 0 && name_len < 500 && !is_wide) {
        memcpy(name, (void*)(entry_addr + 2), name_len);
    }

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"index\":%d,\"block\":%d,\"offset\":%d,"
        "\"header\":\"0x%04x\",\"len\":%d,\"is_wide\":%d,"
        "\"name\":\"%s\",\"entry_addr\":\"0x%lx\"}",
        fname_idx, block, offset, header, name_len, is_wide,
        name, entry_addr);
    json_ok(fd, buf);
}

/* API: 按名字搜索FName index */
static void api_fname_search(int fd, const char* req) {
    const char* q_str = strstr(req, "name=");
    if (!q_str) {
        json_err(fd, 400, "name parameter required, e.g. /fname-search?name=Wood");
        return;
    }
    q_str += 5;
    char search_name[256] = {0};
    int si = 0;
    while (q_str[si] && q_str[si] != '&' && q_str[si] != ' ' && q_str[si] != '\r' && si < 255) {
        search_name[si] = q_str[si];
        si++;
    }
    search_name[si] = '\0';
    int search_len = si;

    if (g_FNamePool == 0) {
        json_err(fd, 503, "FNamePool not discovered");
        return;
    }

    install_segv_handler();

    /* 读CurrentBlock */
    uintptr_t cb_val = 0;
    safe_read_ptr(g_FNamePool + 8, &cb_val);
    int max_block = (int)(cb_val & 0xFFFFFFFF);
    if (max_block <= 0 || max_block > 10000) max_block = 100;

    uintptr_t blocks_array = g_FNamePool + 16;
    char* buf = (char*)malloc(8192);
    if (!buf) { json_err(fd, 500, "malloc failed"); return; }

    int pos = snprintf(buf, 8192,
        "{\"search\":\"%s\",\"results\":[", search_name);
    int found = 0;

    for (int blk = 0; blk < max_block && found < 10; blk++) {
        uintptr_t block_ptr = 0;
        if (safe_read_ptr(blocks_array + blk * 8, &block_ptr) != 0 || block_ptr == 0) continue;

        /* 每个block最多65536字节，stride=2, 所以最多32768个offsets */
        /* 扫描block中的FNameEntry */
        for (int off = 0; off < 32768 && found < 10; off++) {
            uintptr_t entry = block_ptr + (uintptr_t)off * 2;
            uintptr_t raw = 0;
            if (safe_read_ptr(entry, &raw) != 0) break;

            uint16_t header = *(uint16_t*)entry;
            int elen = header >> 6;
            if (elen != search_len) continue;
            if (header & 1) continue; /* skip wide */

            /* 比较字符串 */
            const char* estr = (const char*)(entry + 2);
            if (memcmp(estr, search_name, search_len) == 0) {
                int fname_idx = (blk << 16) | off;
                if (found > 0) buf[pos++] = ',';
                pos += snprintf(buf + pos, 8192 - pos,
                    "{\"index\":%d,\"block\":%d,\"offset\":%d}", fname_idx, blk, off);
                found++;
                palhook_log("fname-search: '%s' -> index %d (block %d, offset %d)",
                    search_name, fname_idx, blk, off);
            }
        }
    }

    snprintf(buf + pos, 8192 - pos, "],\"total_found\":%d}", found);
    json_ok(fd, buf);
    free(buf);
}

/* API: 在所有rw段搜索vtable匹配的对象 */
static void api_find_vtable(int fd, const char* req) {
    const char* vt_str = strstr(req, "vt=");
    if (!vt_str) {
        json_err(fd, 400, "vt parameter required, e.g. /find-vtable?vt=0x2259620");
        return;
    }
    uintptr_t target_vt = strtoull(vt_str + 3, NULL, 0);
    if (target_vt == 0) {
        json_err(fd, 400, "invalid vtable address");
        return;
    }

    /* 可选: max参数限制返回数量 */
    const char* max_str = strstr(req, "max=");
    int max_results = max_str ? atoi(max_str + 4) : 20;
    if (max_results < 1) max_results = 1;
    if (max_results > 100) max_results = 100;

    if (g_all_rw_count == 0) parse_proc_maps();
    install_segv_handler();

    palhook_log("find-vtable: searching for vt=0x%lx in %d rw regions", target_vt, g_all_rw_count);

    char* buf = (char*)malloc(65536);
    if (!buf) { json_err(fd, 500, "malloc failed"); return; }

    int pos = snprintf(buf, 65536,
        "{\"target_vt\":\"0x%lx\",\"results\":[", target_vt);

    int found = 0;
    for (int seg = 0; seg < g_all_rw_count && found < max_results; seg++) {
        uintptr_t scan_start = g_all_rw[seg][0];
        uintptr_t scan_end = g_all_rw[seg][1];
        uintptr_t* ptr = (uintptr_t*)scan_start;
        size_t count = (scan_end - scan_start) / sizeof(uintptr_t);

        for (size_t i = 0; i < count && found < max_results; i++) {
            uintptr_t val = ptr[i];
            if (val < 0x10000 || val > 0x800000000000UL) continue;

            uintptr_t first_qword = 0;
            if (safe_read_ptr(val, &first_qword) != 0) continue;

            if (first_qword == target_vt) {
                uintptr_t ptr_addr = (uintptr_t)&ptr[i];
                if (found > 0) buf[pos++] = ',';
                pos += snprintf(buf + pos, 65536 - pos,
                    "{\"ptr_at\":\"0x%lx\",\"obj\":\"0x%lx\",\"region\":%d}",
                    ptr_addr, val, seg);
                found++;
                palhook_log("  found: ptr=0x%lx -> obj=0x%lx (region %d)", ptr_addr, val, seg);
            }
        }
    }

    pos += snprintf(buf + pos, 65536 - pos,
        "],\"total_found\":%d}", found);
    json_ok(fd, buf);
    free(buf);
}

/*
 * /find-class — 按UClass名称搜索对象实例 (Blueprint类专用，不依赖vtable)
 * GET /find-class?name=BP_PalPlayerInventoryData_C&max=10
 *
 * 原理: 遍历rw内存，对每个指针P:
 *   1. 读 P+0x10 获取UClass地址 C
 *   2. 读 C+0x18 获取UClass的FName
 *   3. FName index == target则匹配
 *   4. 检查P+0x18不是"Default__"开头(跳过CDO)
 */
static void api_find_class(int fd, const char* req) {
    const char* name_str = strstr(req, "name=");
    if (!name_str) {
        json_err(fd, 400, "name parameter required, e.g. /find-class?name=BP_PalPlayerInventoryData_C");
        return;
    }
    name_str += 5;
    char class_name[256] = {0};
    int ci = 0;
    while (name_str[ci] && name_str[ci] != '&' && name_str[ci] != ' ' && ci < 255) {
        class_name[ci] = name_str[ci]; ci++;
    }
    class_name[ci] = '\0';
    if (!class_name[0]) {
        json_err(fd, 400, "empty class name");
        return;
    }

    const char* max_str = strstr(req, "max=");
    int max_results = max_str ? atoi(max_str + 4) : 10;
    if (max_results < 1) max_results = 1;
    if (max_results > 50) max_results = 50;

    if (g_FNamePool == 0) {
        json_err(fd, 503, "FNamePool not discovered yet");
        return;
    }
    if (g_all_rw_count == 0) parse_proc_maps();
    install_segv_handler();

    /* Step1: 把class name解析成FName index */
    int target_fname_idx = find_fname_index(class_name);
    if (target_fname_idx < 0) {
        char err[256];
        snprintf(err, sizeof(err), "class '%s' not found in FNamePool", class_name);
        json_err(fd, 404, err);
        return;
    }
    palhook_log("find-class: '%s' -> FName index %d, scanning %d rw regions",
        class_name, target_fname_idx, g_all_rw_count);

    char* buf = (char*)malloc(65536);
    if (!buf) { json_err(fd, 500, "malloc failed"); return; }

    int pos = snprintf(buf, 65536,
        "{\"class\":\"%s\",\"fname_idx\":%d,\"results\":[", class_name, target_fname_idx);

    int found = 0;
    for (int seg = 0; seg < g_all_rw_count && found < max_results; seg++) {
        uintptr_t scan_start = g_all_rw[seg][0];
        uintptr_t scan_end = g_all_rw[seg][1];
        uintptr_t* ptr = (uintptr_t*)scan_start;
        size_t count = (scan_end - scan_start) / sizeof(uintptr_t);

        for (size_t i = 0; i < count && found < max_results; i++) {
            uintptr_t val = ptr[i];
            if (val < 0x10000 || val > 0x800000000000UL) continue;

            /* 读 UObject+0x10 = UClass* */
            uintptr_t uclass = 0;
            if (safe_read_ptr(val + 0x10, &uclass) != 0) continue;
            if (uclass < 0x10000 || uclass > 0x800000000000UL) continue;

            /* 读 UClass+0x18 = FName */
            uintptr_t cls_fname_raw = 0;
            if (safe_read_ptr(uclass + 0x18, &cls_fname_raw) != 0) continue;
            int cls_fname_idx = (int)(cls_fname_raw & 0xFFFFFFFF);

            if (cls_fname_idx != target_fname_idx) continue;

            /* 匹配！读对象自身的FName (过滤CDO) */
            uintptr_t obj_fname_raw = 0;
            safe_read_ptr(val + 0x18, &obj_fname_raw);
            int obj_fname_idx = (int)(obj_fname_raw & 0xFFFFFFFF);
            char obj_name[256] = {0};
            resolve_fname(obj_fname_idx, obj_name, sizeof(obj_name));

            /* 跳过Default__开头的CDO */
            if (strncmp(obj_name, "Default__", 9) == 0) continue;

            uintptr_t ptr_addr = (uintptr_t)&ptr[i];
            if (found > 0) buf[pos++] = ',';
            pos += snprintf(buf + pos, 65536 - pos,
                "{\"ptr_at\":\"0x%lx\",\"obj\":\"0x%lx\",\"name\":\"%s\",\"class_addr\":\"0x%lx\"}",
                ptr_addr, val, obj_name, uclass);
            found++;

            palhook_log("  found: 0x%lx name='%s' class=0x%lx", val, obj_name, uclass);
        }
    }

    pos += snprintf(buf + pos, 65536 - pos,
        "],\"total_found\":%d}", found);
    json_ok(fd, buf);
    free(buf);
}

/* ========== GameThread命令队列 ========== */

#define CMD_QUEUE_SIZE 64
#define PE_VTABLE_INDEX 77

typedef void (*ProcessEventFn)(void* obj, void* func, void* params);

typedef struct {
    void* obj;
    void* ufunc;
    void* params;
    volatile int ready;
    volatile int done;
    volatile int result;
} PendingCmd;

static PendingCmd g_cmd_queue[CMD_QUEUE_SIZE];
static volatile int g_cmd_head = 0;
static volatile int g_cmd_tail = 0;
static pthread_mutex_t g_cmd_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_hook_active = 0;

/* 原始nanosleep函数指针 */
static int (*g_real_nanosleep)(const struct timespec*, struct timespec*) = NULL;

/* 处理命令队列 (在GameThread中调用) */
static void process_cmd_queue(void) {
    if (g_cmd_head == g_cmd_tail) return;

    pthread_mutex_lock(&g_cmd_lock);
    while (g_cmd_head != g_cmd_tail) {
        PendingCmd* cmd = &g_cmd_queue[g_cmd_head];
        if (cmd->ready && !cmd->done) {
            palhook_log("GameThread: executing ProcessEvent obj=0x%lx func=0x%lx",
                (uintptr_t)cmd->obj, (uintptr_t)cmd->ufunc);

            /* 恢复默认信号处理 (不能用PalHook的SIGSEGV handler) */
            struct sigaction sa_old, sa_default;
            memset(&sa_default, 0, sizeof(sa_default));
            sa_default.sa_handler = SIG_DFL;
            sigaction(SIGSEGV, &sa_default, &sa_old);
            sigaction(SIGBUS, &sa_default, NULL);

            /* 获取ProcessEvent函数地址 */
            uintptr_t obj_vt = *(uintptr_t*)cmd->obj;
            uintptr_t pe_addr = ((uintptr_t*)obj_vt)[PE_VTABLE_INDEX];

            uintptr_t pe_lo = (g_base_addr > 0 ? g_base_addr : VPS_BASE_ADDR) + OFF_PE_LO;
            uintptr_t pe_hi = (g_base_addr > 0 ? g_base_addr : VPS_BASE_ADDR) + OFF_PE_HI;
            if (pe_addr > pe_lo && pe_addr < pe_hi) {
                ProcessEventFn pe = (ProcessEventFn)pe_addr;
                pe(cmd->obj, cmd->ufunc, cmd->params);
                cmd->result = 0;
                palhook_log("GameThread: ProcessEvent completed OK");
            } else {
                palhook_log("GameThread: invalid ProcessEvent addr 0x%lx", pe_addr);
                cmd->result = -1;
            }

            /* 恢复PalHook的信号处理 */
            sigaction(SIGSEGV, &sa_old, NULL);
            install_segv_handler();

            cmd->done = 1;
        }
        g_cmd_head = (g_cmd_head + 1) % CMD_QUEUE_SIZE;
    }
    pthread_mutex_unlock(&g_cmd_lock);
}

/*
 * Hook nanosleep — UE5的FPlatformProcess::Sleep每帧调用nanosleep
 * 这保证我们在GameThread上下文中执行命令
 * 只在PalServer主线程（g_init_pid线程）中处理队列
 */
int nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!g_real_nanosleep) {
        g_real_nanosleep = (int(*)(const struct timespec*, struct timespec*))dlsym(RTLD_NEXT, "nanosleep");
    }

    /* 只在PalServer进程的主线程中处理命令队列 */
    if (g_init_pid != 0 && getpid() == g_init_pid && g_initialized) {
        /* 简单的线程ID检查: nanosleep从GameThread调用时处理队列 */
        static __thread int is_game_thread = -1;
        if (is_game_thread == -1) {
            /* 第一次调用，检测是否是频繁调用nanosleep的线程 */
            static volatile int first_thread_set = 0;
            if (!first_thread_set) {
                first_thread_set = 1;
                is_game_thread = 1;  /* 假设第一个调用nanosleep的是GameThread */
                g_hook_active = 1;
                palhook_log("nanosleep hook: GameThread detected (tid=%ld)", (long)syscall(SYS_gettid));
            } else {
                is_game_thread = 0;
            }
        }

        if (is_game_thread == 1) {
            /* 帧计数: GameThread每帧调一次nanosleep, 每秒结算为fps */
            time_t now = time(NULL);
            if (g_frame_ts == 0) g_frame_ts = now;
            if (now != g_frame_ts) {
                g_fps = g_frame_count;
                g_frame_count = 0;
                g_frame_ts = now;
            }
            g_frame_count++;
            process_cmd_queue();
        }
    }

    return g_real_nanosleep(req, rem);
}

/* 入队命令 (从HTTP线程调用) */
static int enqueue_cmd(void* obj, void* ufunc, void* params) {
    pthread_mutex_lock(&g_cmd_lock);
    int next = (g_cmd_tail + 1) % CMD_QUEUE_SIZE;
    if (next == g_cmd_head) {
        pthread_mutex_unlock(&g_cmd_lock);
        return -1;
    }
    g_cmd_queue[g_cmd_tail].obj = obj;
    g_cmd_queue[g_cmd_tail].ufunc = ufunc;
    g_cmd_queue[g_cmd_tail].params = params;
    g_cmd_queue[g_cmd_tail].ready = 1;
    g_cmd_queue[g_cmd_tail].done = 0;
    g_cmd_queue[g_cmd_tail].result = -1;
    g_cmd_tail = next;
    pthread_mutex_unlock(&g_cmd_lock);
    return 0;
}

/* ========== ProcessEvent调用核心 ========== */

#define UFUNC_OFFSET_CHILDPROPS  0x50
#define UFUNC_OFFSET_PARMSSIZE   0x58
#define UFUNC_OFFSET_FLAGS       0xB0
#define UFUNC_OFFSET_NATIVEFUNC  0xD8
#define UCLASS_OFFSET_SUPER      0x40
#define UCLASS_OFFSET_CHILDREN   0x48

/* 内部FName解析 (不走HTTP) */
static int resolve_fname(int idx, char* out, int out_size) {
    if (g_FNamePool == 0 || idx < 0) return -1;
    int block = idx >> 16;
    int offset = idx & 0xFFFF;

    uintptr_t blocks_array = g_FNamePool + 16;
    uintptr_t block_ptr = 0;
    if (safe_read_ptr(blocks_array + block * 8, &block_ptr) != 0 || block_ptr == 0) return -1;

    uintptr_t entry = block_ptr + (uintptr_t)offset * 2;
    uintptr_t raw = 0;
    if (safe_read_ptr(entry, &raw) != 0) return -1;

    uint16_t header = *(uint16_t*)entry;
    int len = header >> 6;
    if (len <= 0 || len >= out_size) return -1;

    /* 区域校验: 字符串字节必须在映射内 (无信号处理器兜底了) */
    if (!region_contains(entry + 2) || !region_contains(entry + 2 + (uintptr_t)len)) return -1;

    memcpy(out, (void*)(entry + 2), len);
    out[len] = '\0';
    return len;
}

/* FName索引缓存 (find_fname_index全池扫描要15-20秒, 必须缓存) */
#define FNAME_CACHE_SIZE 128
static struct { char name[128]; int idx; } g_fname_cache[FNAME_CACHE_SIZE];
static int g_fname_cache_count = 0;

/* 在FNamePool中按字符串查找FName索引 (memmem搜索，支持窄/宽字符串, 带缓存) */
static int find_fname_index(const char* target_name) {
    if (g_FNamePool == 0 || !target_name) return -1;

    /* 查缓存 */
    for (int i = 0; i < g_fname_cache_count; i++) {
        if (strcmp(g_fname_cache[i].name, target_name) == 0) return g_fname_cache[i].idx;
    }

    int target_len = strlen(target_name);
    if (target_len <= 0 || target_len > 255) return -1;

    install_segv_handler();
    uintptr_t blocks_array = g_FNamePool + 16;

    uintptr_t cb_val = 0;
    if (safe_read_ptr(g_FNamePool + 8, &cb_val) != 0) return -1;
    int max_block = (cb_val & 0xFFFFFFFF) + 1;
    if (max_block > 8192) max_block = 8192;

    for (int block = 0; block < max_block; block++) {
        uintptr_t block_ptr = 0;
        if (safe_read_ptr(blocks_array + block * 8, &block_ptr) != 0 || block_ptr == 0)
            continue;

        /* FName offset最大65535，stride=2，所以block最大65535*2=131070字节 */
        /* 先探测block实际可读大小 */
        size_t block_size = 0;
        for (size_t probe = 0; probe < 0x20000; probe += 0x1000) {
            uintptr_t test = 0;
            if (safe_read_ptr(block_ptr + probe, &test) != 0) break;
            block_size = probe + 0x1000;
        }
        if (block_size < 64) continue;

        uint8_t* base = (uint8_t*)block_ptr;
        uint8_t* search = base;
        size_t remain = block_size;

        while (remain > (size_t)(target_len + 2)) {
            uint8_t* found = (uint8_t*)memmem(search, remain, target_name, target_len);
            if (!found) break;

            /* 验证header: found-2处的uint16, header>>6应该等于target_len */
            if (found >= base + 2) {
                uint16_t header = *(uint16_t*)(found - 2);
                int hdr_len = header >> 6;
                int is_wide = header & 1;

                if (hdr_len == target_len && !is_wide) {
                    uintptr_t entry_addr = (uintptr_t)(found - 2);
                    int byte_offset = (int)(entry_addr - block_ptr);
                    int fname_offset = byte_offset / 2;
                    int fname_index = (block << 16) | fname_offset;
                    /* 写缓存 */
                    if (g_fname_cache_count < FNAME_CACHE_SIZE) {
                        strncpy(g_fname_cache[g_fname_cache_count].name, target_name, 127);
                        g_fname_cache[g_fname_cache_count].idx = fname_index;
                        g_fname_cache_count++;
                    }
                    return fname_index;
                }
            }

            /* 继续搜索 */
            size_t skip = (found - search) + 1;
            search = found + 1;
            remain -= skip;
        }
    }

    /* 第二阶段: 宽字符串搜索 (UTF-16LE) */
    if (target_len * 2 + 2 < 0x40000) {
        uint8_t wide_pat[512];
        for (int i = 0; i < target_len; i++) {
            wide_pat[i * 2] = (uint8_t)target_name[i];
            wide_pat[i * 2 + 1] = 0;
        }
        for (int block = 0; block < max_block; block++) {
            uintptr_t block_ptr = 0;
            if (safe_read_ptr(blocks_array + block * 8, &block_ptr) != 0 || block_ptr == 0)
                continue;
            size_t block_size = 0;
            for (size_t probe = 0; probe < 0x20000; probe += 0x1000) {
                uintptr_t test = 0;
                if (safe_read_ptr(block_ptr + probe, &test) != 0) break;
                block_size = probe + 0x1000;
            }
            if (block_size < 64) continue;

            uint8_t* base = (uint8_t*)block_ptr;
            uint8_t* search = base;
            size_t remain = block_size;

            while (remain > (size_t)(target_len * 2 + 2)) {
                uint8_t* found = (uint8_t*)memmem(search, remain, wide_pat, target_len * 2);
                if (!found) break;
                if (found >= base + 2) {
                    uint16_t header = *(uint16_t*)(found - 2);
                    int hdr_len = header >> 6;
                    int is_wide = header & 1;
                    if (hdr_len == target_len && is_wide) {
                        uintptr_t entry_addr = (uintptr_t)(found - 2);
                        int byte_offset = (int)(entry_addr - block_ptr);
                        int fname_offset = byte_offset / 2;
                        int fname_index = (block << 16) | fname_offset;
                        palhook_log("fname '%s' = %d (wide)", target_name, fname_index);
                        if (g_fname_cache_count < FNAME_CACHE_SIZE) {
                            strncpy(g_fname_cache[g_fname_cache_count].name, target_name, 127);
                            g_fname_cache[g_fname_cache_count].idx = fname_index;
                            g_fname_cache_count++;
                        }
                        return fname_index;
                    }
                }
                size_t skip = (found - search) + 1;
                search = found + 1;
                remain -= skip;
            }
        }
    }
    return -1;
}

/* 在UClass的Children链表中按名字找UFunction */
static void* find_ufunction_in_class(uintptr_t uclass, const char* func_name) {
    if (uclass == 0 || !func_name) return NULL;
    install_segv_handler();

    /* 遍历继承链 */
    uintptr_t cls = uclass;
    for (int depth = 0; depth < 10 && cls; depth++) {
        uintptr_t children = 0;
        if (safe_read_ptr(cls + UCLASS_OFFSET_CHILDREN, &children) != 0) break;

        uintptr_t curr = children;
        int count = 0;
        while (curr && curr > 0x10000 && count < 500) {
            count++;
            uintptr_t vt = 0;
            if (safe_read_ptr(curr, &vt) != 0) break;

            /* 只处理UFunction (vtable, ASLR安全) */
            if (vt == (g_base_addr > 0 ? g_base_addr + OFF_VTABLE_UFUNCTION : 0x1a51218)) {
                uintptr_t name_raw = 0;
                if (safe_read_ptr(curr + 0x18, &name_raw) == 0) {
                    int name_idx = name_raw & 0xFFFFFFFF;
                    char name[256] = {0};
                    if (resolve_fname(name_idx, name, sizeof(name)) > 0) {
                        if (strcmp(name, func_name) == 0) {
                            return (void*)curr;
                        }
                    }
                }
            }

            /* Next field at +0x28 */
            uintptr_t next = 0;
            if (safe_read_ptr(curr + 0x28, &next) != 0) break;
            if (next == curr) break;
            curr = next;
        }

        /* Walk up to SuperStruct */
        uintptr_t super = 0;
        if (safe_read_ptr(cls + UCLASS_OFFSET_SUPER, &super) != 0) break;
        if (super < 0x10000 || super == cls) break;
        cls = super;
    }
    return NULL;
}

/* 调用ProcessEvent */
static int call_process_event(void* obj, void* ufunc, void* params) {
    if (!obj || !ufunc) return -1;
    install_segv_handler();

    /* 读对象的vtable */
    uintptr_t vtable = 0;
    if (safe_read_ptr((uintptr_t)obj, &vtable) != 0) return -1;

    /* 读vtable[PE_VTABLE_INDEX] */
    uintptr_t pe_addr = 0;
    if (safe_read_ptr(vtable + PE_VTABLE_INDEX * 8, &pe_addr) != 0) return -1;

    /* 验证是合法的text段地址 (ASLR安全) */
    uintptr_t pe_lo2 = (g_base_addr > 0 ? g_base_addr : VPS_BASE_ADDR) + OFF_PE_LO;
    uintptr_t pe_hi2 = (g_base_addr > 0 ? g_base_addr : VPS_BASE_ADDR) + OFF_PE_HI;
    if (pe_addr < pe_lo2 || pe_addr > pe_hi2) {
        palhook_log("ERROR: ProcessEvent ptr 0x%lx not in text segment", pe_addr);
        return -1;
    }

    palhook_log("calling ProcessEvent: obj=0x%lx func=0x%lx params=%p pe=0x%lx",
        (uintptr_t)obj, (uintptr_t)ufunc, params, pe_addr);

    ProcessEventFn pe = (ProcessEventFn)pe_addr;
    pe(obj, ufunc, params);

    palhook_log("ProcessEvent returned successfully");
    return 0;
}

/* API: 调用任意UFunction */
static void api_call_function(int fd, const char* req) {
    if (!g_GEngine || g_FNamePool == 0) {
        json_err(fd, 503, "not initialized, run /scan first");
        return;
    }

    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));

    char obj_addr_str[64] = {0};
    char func_name[256] = {0};
    char obj_type[64] = {0};  /* "cheatmanager", "world", "gamestate" etc */

    json_get_string(body, "obj", obj_addr_str, sizeof(obj_addr_str));
    json_get_string(body, "func", func_name, sizeof(func_name));
    json_get_string(body, "type", obj_type, sizeof(obj_type));

    if (!func_name[0]) {
        json_err(fd, 400, "func is required (function name)");
        return;
    }

    install_segv_handler();

    /* 确定目标对象 */
    void* target_obj = NULL;
    uintptr_t target_class = 0;

    if (obj_addr_str[0]) {
        /* 直接指定对象地址 */
        target_obj = (void*)strtoull(obj_addr_str, NULL, 0);
        uintptr_t cls = 0;
        if (safe_read_ptr((uintptr_t)target_obj + 0x10, &cls) == 0) {
            target_class = cls;
        }
    } else if (strcmp(obj_type, "cheatmanager") == 0) {
        /* 查找CheatManager实例 */
        uintptr_t cm_vt = (g_base_addr > 0 ? g_base_addr : VPS_BASE_ADDR) + OFF_VTABLE_UCHEATMANAGER;  /* UCheatManager vtable+16 */
        for (int seg = 0; seg < g_all_rw_count && !target_obj; seg++) {
            uintptr_t* ptr = (uintptr_t*)g_all_rw[seg][0];
            size_t cnt = (g_all_rw[seg][1] - g_all_rw[seg][0]) / sizeof(uintptr_t);
            for (size_t i = 0; i < cnt; i++) {
                uintptr_t val = ptr[i];
                if (val < 0x10000 || val > 0x800000000000UL) continue;
                uintptr_t vt = 0;
                if (safe_read_ptr(val, &vt) == 0 && vt == cm_vt) {
                    target_obj = (void*)val;
                    uintptr_t cls = 0;
                    safe_read_ptr(val + 0x10, &cls);
                    target_class = cls;
                    break;
                }
            }
        }
    } else if (strcmp(obj_type, "engine") == 0 && g_GEngine) {
        target_obj = *g_GEngine;
        safe_read_ptr((uintptr_t)target_obj + 0x10, &target_class);
    }

    if (!target_obj) {
        json_err(fd, 404, "target object not found");
        return;
    }
    if (!target_class) {
        json_err(fd, 500, "cannot read target class");
        return;
    }

    palhook_log("call-function: obj=0x%lx class=0x%lx func='%s'",
        (uintptr_t)target_obj, target_class, func_name);

    /* 在UClass中查找函数 */
    void* ufunc = find_ufunction_in_class(target_class, func_name);
    if (!ufunc) {
        char err[256];
        snprintf(err, sizeof(err), "function '%s' not found in class hierarchy", func_name);
        json_err(fd, 404, err);
        return;
    }

    palhook_log("  found UFunction '%s' at 0x%lx", func_name, (uintptr_t)ufunc);

    /* 检查参数: 读ParmsSize */
    uintptr_t parms_raw = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &parms_raw);
    int parms_size = parms_raw & 0xFFFFFFFF;

    /* 对于小参数(<=256字节)，分配零初始化缓冲区 */
    void* params_buf = NULL;
    if (parms_size > 0 && parms_size <= 256) {
        params_buf = calloc(1, parms_size);

        /* 从请求体读取参数值 */
        /* 支持: "int_param": 1000 (写入前4字节作为int32) */
        /* 支持: "hex_params": "e8030000..." (直接写入hex字节) */
        int int_param = json_get_int(body, "int_param");
        if (int_param > 0) {
            *(int32_t*)params_buf = int_param;
            palhook_log("  int_param=%d", int_param);
        }

        char hex_params[512] = {0};
        if (json_get_string(body, "hex_params", hex_params, sizeof(hex_params)) == 0 && hex_params[0]) {
            /* 解析hex字符串写入params_buf */
            int hex_len = strlen(hex_params);
            for (int i = 0; i < hex_len && i/2 < parms_size; i += 2) {
                char byte_str[3] = { hex_params[i], hex_params[i+1], 0 };
                ((uint8_t*)params_buf)[i/2] = (uint8_t)strtoul(byte_str, NULL, 16);
            }
            palhook_log("  hex_params=%s (%d bytes)", hex_params, hex_len/2);
        }
    } else if (parms_size > 256) {
        char err[256];
        snprintf(err, sizeof(err),
            "function '%s' requires %d bytes of parameters (too large)",
            func_name, parms_size);
        json_err(fd, 400, err);
        return;
    }

    /* 调用ProcessEvent (无参函数) */
    /* 注意: ProcessEvent必须在GameThread调用，当前是HTTP线程 */
    /* 暂时先记录信息，后续实现GameThread队列 */

    /* 读NativeFunc地址 */
    uintptr_t native_func = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_NATIVEFUNC, &native_func);

    if (!g_hook_active) {
        json_err(fd, 503, "GameThread hook not active yet (nanosleep not called)");
        return;
    }

    /* 入队到GameThread执行 */
    if (enqueue_cmd(target_obj, ufunc, params_buf) != 0) {
        if (params_buf) free(params_buf);
        json_err(fd, 503, "command queue full");
        return;
    }

    /* 等待执行完成 (最多15秒) */
    int wait_ms = 0;
    PendingCmd* last_cmd = &g_cmd_queue[(g_cmd_tail - 1 + CMD_QUEUE_SIZE) % CMD_QUEUE_SIZE];
    while (!last_cmd->done && wait_ms < 15000) {
        usleep(10000);  /* 10ms */
        wait_ms += 10;
    }

    char buf[512];
    if (last_cmd->done) {
        uint8_t ret_byte = params_buf ? ((uint8_t*)params_buf)[0] : 0;
        snprintf(buf, sizeof(buf),
            "{\"status\":\"%s\",\"obj\":\"0x%lx\",\"func\":\"%s\",\"ufunc\":\"0x%lx\","
            "\"parms_size\":%d,\"native_func\":\"0x%lx\",\"wait_ms\":%d,\"ret_byte\":%d}",
            last_cmd->result == 0 ? "success" : "failed",
            (uintptr_t)target_obj, func_name, (uintptr_t)ufunc,
            parms_size, native_func, wait_ms, ret_byte);
    } else {
        snprintf(buf, sizeof(buf),
            "{\"status\":\"timeout\",\"obj\":\"0x%lx\",\"func\":\"%s\",\"ufunc\":\"0x%lx\","
            "\"parms_size\":%d,\"message\":\"GameThread did not process within 15s\"}",
            (uintptr_t)target_obj, func_name, (uintptr_t)ufunc, parms_size);
    }
    if (params_buf) free(params_buf);
    json_ok(fd, buf);
}

/*
 * /give-item — 核心刷道具接口
 * POST body: {"item_id":"Stone", "count":99, "inv":"0x..."}
 *
 * item_id: 物品静态ID (FName字符串，如 Stone, Wood, Fiber, PalSphere, Arrow)
 * count: 数量 (默认1)
 * inv: InventoryData对象地址 (必须提前通过外部脚本获取)
 *
 * 调用 AddItem_ServerInternal(StaticItemId, Count, IsAssignPassive, LogDelay, bNotifyLog)
 * ParmsSize=24 字节布局:
 *   +0x00: FName StaticItemId (8 bytes: index u32 + number u32)
 *   +0x08: int32 Count
 *   +0x0C: bool IsAssignPassive
 *   +0x10: float LogDelay
 *   +0x14: bool bNotifyLog
 *   +0x15: bool ReturnValue (output)
 */
static void give_item_impl(int fd, const char* body);

static void api_give_item(int fd, const char* req) {
    if (!g_GEngine || g_FNamePool == 0) {
        json_err(fd, 503, "not initialized, run /scan first");
        return;
    }
    if (!g_hook_active) {
        json_err(fd, 503, "GameThread hook not active yet");
        return;
    }

    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));
    give_item_impl(fd, body);
}

static void give_item_impl(int fd, const char* body) {
    char item_id[128] = {0};
    char inv_addr_str[64] = {0};
    int count = json_get_int(body, "count");
    json_get_string(body, "item_id", item_id, sizeof(item_id));
    json_get_string(body, "inv", inv_addr_str, sizeof(inv_addr_str));

    if (!item_id[0]) {
        json_err(fd, 400, "item_id is required (e.g. Stone, Wood, Fiber)");
        return;
    }
    if (count <= 0) count = 1;
    if (count > 9999) count = 9999;

    install_segv_handler();

    char player_name[64] = {0};
    json_get_string(body, "name", player_name, sizeof(player_name));

    uintptr_t inv_obj = 0;
    if (inv_addr_str[0]) {
        inv_obj = strtoull(inv_addr_str, NULL, 0);
    }

    /* 指定玩家: 通过PlayerState::GetInventoryData()拿到该玩家的背包 */
    if (inv_obj < 0x10000 && player_name[0]) {
        if (g_players_ts == 0 || time(NULL) - g_players_ts > 20) {
            collect_all_players();
        }
        fill_player_details();
        for (int i = 0; i < g_players_count && !inv_obj; i++) {
            if (strcmp(g_players[i].name, player_name) == 0 && g_players[i].playerstate) {
                uintptr_t ps = g_players[i].playerstate;
                uintptr_t pcls = 0;
                safe_read_ptr(ps + 0x10, &pcls);
                void* gif = find_ufunction_in_class(pcls, "GetInventoryData");
                if (gif) {
                    uintptr_t pr0 = 0;
                    safe_read_ptr((uintptr_t)gif + UFUNC_OFFSET_PARMSSIZE, &pr0);
                    int psz = (int)(pr0 & 0xFFFFFFFF);
                    if (psz <= 0 || psz > 64) psz = 8;
                    uint8_t* pp = (uint8_t*)calloc(1, psz);
                    int waited0 = 0;
                    int rr = call_and_wait((void*)ps, gif, pp, 15000, &waited0);
                    if (rr == 0) {
                        uintptr_t rvp = find_property_in_struct((uintptr_t)gif, "ReturnValue");
                        int roff = rvp ? prop_get_offset(rvp) : 0;
                        if (roff < 0) roff = 0;
                        if (roff + 8 <= psz) inv_obj = *(uintptr_t*)(pp + roff);
                    }
                    free(pp);
                }
            }
        }
        if (!inv_obj) {
            char e[128];
            snprintf(e, sizeof(e), "player '%s' inventory not found", player_name);
            json_err(fd, 404, e);
            return;
        }
        palhook_log("give-item: target player '%s' inv=0x%lx", player_name, inv_obj);
    }

    /* inv没传或无效时，自动查找第一个玩家的InventoryData */
    if (inv_obj < 0x10000) {
        int inv_fname_idx = find_fname_index("BP_PalPlayerInventoryData_C");
        if (inv_fname_idx > 0 && g_all_rw_count > 0) {
            for (int seg = 0; seg < g_all_rw_count && inv_obj == 0; seg++) {
                uintptr_t* ptr = (uintptr_t*)g_all_rw[seg][0];
                size_t cnt = (g_all_rw[seg][1] - g_all_rw[seg][0]) / sizeof(uintptr_t);
                for (size_t i = 0; i < cnt; i++) {
                    uintptr_t val = ptr[i];
                    if (val < 0x10000 || val > 0x800000000000UL) continue;
                    uintptr_t uc = 0;
                    if (safe_read_ptr(val + 0x10, &uc) != 0) continue;
                    uintptr_t fn = 0;
                    if (safe_read_ptr(uc + 0x18, &fn) != 0) continue;
                    if ((int)(fn & 0xFFFFFFFF) == inv_fname_idx) {
                        uintptr_t on = 0;
                        safe_read_ptr(val + 0x18, &on);
                        char nm[64] = {0};
                        resolve_fname((int)(on & 0xFFFFFFFF), nm, sizeof(nm));
                        if (strncmp(nm, "Default__", 9) != 0) {
                            inv_obj = val;
                            palhook_log("give-item: auto-found inv=0x%lx", inv_obj);
                            break;
                        }
                    }
                }
            }
        }
        if (inv_obj == 0) {
            json_err(fd, 404, "no player InventoryData found, pass 'inv' or ensure a player is online");
            return;
        }
    }

    palhook_log("give-item: item='%s' count=%d inv=0x%lx", item_id, count, inv_obj);

    /* 验证inv对象可读 */
    uintptr_t inv_class = 0;
    if (safe_read_ptr(inv_obj + 0x10, &inv_class) != 0 || inv_class == 0) {
        json_err(fd, 400, "cannot read InventoryData object at given address");
        return;
    }

    /* 查找物品FName索引 */
    int item_fname_idx = find_fname_index(item_id);
    if (item_fname_idx < 0) {
        char err[256];
        snprintf(err, sizeof(err), "item '%s' not found in FNamePool", item_id);
        json_err(fd, 404, err);
        return;
    }
    palhook_log("  item FName index: %d (0x%x)", item_fname_idx, item_fname_idx);

    /* 查找 AddItem_ServerInternal 函数 */
    void* ufunc = find_ufunction_in_class(inv_class, "AddItem_ServerInternal");
    if (!ufunc) {
        json_err(fd, 404, "AddItem_ServerInternal not found on InventoryData class");
        return;
    }
    palhook_log("  AddItem_ServerInternal at 0x%lx", (uintptr_t)ufunc);

    /* 构造24字节参数 */
    uint8_t* params = (uint8_t*)calloc(1, 24);
    if (!params) {
        json_err(fd, 500, "malloc failed");
        return;
    }

    /* +0x00: FName StaticItemId (index + number) */
    *(uint32_t*)(params + 0) = (uint32_t)item_fname_idx;
    *(uint32_t*)(params + 4) = 0;  /* FName number = 0 */

    /* +0x08: int32 Count */
    *(int32_t*)(params + 8) = count;

    /* +0x0C: bool IsAssignPassive = false */
    params[12] = 0;

    /* +0x10: float LogDelay = 0.0 */
    *(float*)(params + 16) = 0.0f;

    /* +0x14: bool bNotifyLog = true */
    params[20] = 1;

    /* 入队到GameThread执行 */
    if (enqueue_cmd((void*)inv_obj, ufunc, params) != 0) {
        free(params);
        json_err(fd, 503, "command queue full");
        return;
    }

    /* 等待执行完成 */
    int wait_ms = 0;
    PendingCmd* last_cmd = &g_cmd_queue[(g_cmd_tail - 1 + CMD_QUEUE_SIZE) % CMD_QUEUE_SIZE];
    while (!last_cmd->done && wait_ms < 15000) {
        usleep(10000);
        wait_ms += 10;
    }

    char buf[512];
    if (last_cmd->done && last_cmd->result == 0) {
        uint8_t ret_val = params[21]; /* ReturnValue at offset 0x15 */
        snprintf(buf, sizeof(buf),
            "{\"status\":\"success\",\"item_id\":\"%s\",\"fname_idx\":%d,"
            "\"count\":%d,\"inv\":\"0x%lx\",\"return_value\":%d,\"wait_ms\":%d}",
            item_id, item_fname_idx, count, inv_obj, ret_val, wait_ms);
    } else if (last_cmd->done) {
        snprintf(buf, sizeof(buf),
            "{\"status\":\"failed\",\"item_id\":\"%s\",\"message\":\"ProcessEvent returned error\"}",
            item_id);
    } else {
        snprintf(buf, sizeof(buf),
            "{\"status\":\"timeout\",\"item_id\":\"%s\",\"message\":\"GameThread did not process within 15s\"}",
            item_id);
    }
    free(params);
    json_ok(fd, buf);
}

/*
 * /find-players — 发现所有在线玩家对象
 * 返回每个玩家的PlayerState、InventoryData、PlayerController地址
 *
 * 工作原理:
 *   1. find-class查找所有BP_PalPlayerState_C实例
 *   2. 扫描每个PlayerState内存，找class含"Inventory"的指针 => InventoryData
 *   3. 扫描找class含"Controller"的指针 => PlayerController
 */
static void api_find_players(int fd, const char* req) {
    (void)req;
    if (g_FNamePool == 0) {
        json_err(fd, 503, "FNamePool not discovered yet");
        return;
    }
    if (g_all_rw_count == 0) parse_proc_maps();
    install_segv_handler();

    /* 找BP_PalPlayerState_C的FName索引 */
    int ps_fname_idx = find_fname_index("BP_PalPlayerState_C");
    if (ps_fname_idx < 0) {
        json_err(fd, 404, "BP_PalPlayerState_C class not found in FNamePool");
        return;
    }

    /* 也预查InventoryData和Controller的class FName */
    int inv_fname_idx = find_fname_index("BP_PalPlayerInventoryData_C");
    int ctrl_fname_idx = find_fname_index("BP_PalPlayerController_C");

    palhook_log("find-players: PS=%d, Inv=%d, Ctrl=%d", ps_fname_idx, inv_fname_idx, ctrl_fname_idx);

    char* buf = (char*)malloc(65536);
    if (!buf) { json_err(fd, 500, "malloc failed"); return; }
    int pos = snprintf(buf, 65536, "{\"players\":[");

    int player_count = 0;
    uintptr_t seen_objs[20] = {0};  /* 去重：同一个对象只记录一次 */

    /* 扫描所有rw区域找PlayerState对象 */
    for (int seg = 0; seg < g_all_rw_count && player_count < 20; seg++) {
        uintptr_t* ptr = (uintptr_t*)g_all_rw[seg][0];
        size_t count = (g_all_rw[seg][1] - g_all_rw[seg][0]) / sizeof(uintptr_t);

        for (size_t i = 0; i < count && player_count < 20; i++) {
            uintptr_t val = ptr[i];
            if (val < 0x10000 || val > 0x800000000000UL) continue;

            uintptr_t uclass = 0;
            if (safe_read_ptr(val + 0x10, &uclass) != 0) continue;
            if (uclass < 0x10000 || uclass > 0x800000000000UL) continue;

            uintptr_t cls_fname_raw = 0;
            if (safe_read_ptr(uclass + 0x18, &cls_fname_raw) != 0) continue;
            if ((int)(cls_fname_raw & 0xFFFFFFFF) != ps_fname_idx) continue;

            /* 跳过CDO */
            uintptr_t obj_fname_raw = 0;
            safe_read_ptr(val + 0x18, &obj_fname_raw);
            int obj_fname_idx = (int)(obj_fname_raw & 0xFFFFFFFF);
            char obj_name[256] = {0};
            resolve_fname(obj_fname_idx, obj_name, sizeof(obj_name));
            if (strncmp(obj_name, "Default__", 9) == 0) continue;

            /* 去重检查 */
            int dup = 0;
            for (int d = 0; d < player_count; d++) {
                if (seen_objs[d] == val) { dup = 1; break; }
            }
            if (dup) continue;
            seen_objs[player_count] = val;

            /* 找到一个真实的PlayerState！扫描其内存找Inventory和Controller */
            uintptr_t inv_addr = 0, ctrl_addr = 0;

            /* 扫描PlayerState +0x28 到 +0x800 */
            for (int off = 5; off < 256; off++) {
                uintptr_t field = 0;
                if (safe_read_ptr(val + off * 8, &field) != 0) continue;
                if (field < 0x10000 || field > 0x800000000000UL) continue;

                uintptr_t fc = 0;
                if (safe_read_ptr(field + 0x10, &fc) != 0) continue;
                if (fc < 0x10000) continue;

                uintptr_t fcn = 0;
                if (safe_read_ptr(fc + 0x18, &fcn) != 0) continue;
                int fcni = (int)(fcn & 0xFFFFFFFF);

                if (inv_fname_idx > 0 && fcni == inv_fname_idx && !inv_addr)
                    inv_addr = field;
                if (ctrl_fname_idx > 0 && fcni == ctrl_fname_idx && !ctrl_addr)
                    ctrl_addr = field;

                if (inv_addr && ctrl_addr) break;
            }

            if (player_count > 0) buf[pos++] = ',';
            pos += snprintf(buf + pos, 65536 - pos,
                "{\"name\":\"%s\",\"state\":\"0x%lx\",\"inv\":\"0x%lx\",\"ctrl\":\"0x%lx\"}",
                obj_name, val, inv_addr, ctrl_addr);
            player_count++;

            palhook_log("  player '%s': state=0x%lx inv=0x%lx ctrl=0x%lx",
                obj_name, val, inv_addr, ctrl_addr);
        }
    }

    pos += snprintf(buf + pos, 65536 - pos,
        "],\"count\":%d}", player_count);
    json_ok(fd, buf);
    free(buf);
}

/*
 * /give-exp — 给玩家加经验
 * POST {"exp":1000, "ctrl":"0x..."}
 * ctrl = PlayerController地址 (从/find-players获取)
 * 调用 Debug_AddPlayerExp_ToServer(int32 Exp)
 */
static void api_give_exp(int fd, const char* req) {
    if (!g_GEngine || g_FNamePool == 0 || !g_hook_active) {
        json_err(fd, 503, "not ready");
        return;
    }

    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));

    int exp = json_get_int(body, "exp");
    if (exp <= 0) exp = 100;

    install_segv_handler();
    uintptr_t ctrl = resolve_ctrl(body);
    if (!ctrl) {
        json_err(fd, 404, "PlayerController not found, pass 'ctrl' or ensure player is online");
        return;
    }

    uintptr_t ctrl_class = 0;
    if (safe_read_ptr(ctrl + 0x10, &ctrl_class) != 0 || ctrl_class == 0) {
        json_err(fd, 400, "cannot read PlayerController");
        return;
    }

    void* ufunc = find_ufunction_in_class(ctrl_class, "Debug_AddPlayerExp_ToServer");
    if (!ufunc) {
        json_err(fd, 404, "Debug_AddPlayerExp_ToServer not found");
        return;
    }

    /* ParmsSize通常是4 (int32 Exp) */
    uintptr_t parms_raw = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &parms_raw);
    int parms_size = parms_raw & 0xFFFFFFFF;
    if (parms_size <= 0 || parms_size > 64) parms_size = 4;

    uint8_t* params = (uint8_t*)calloc(1, parms_size);
    *(int32_t*)params = exp;

    if (enqueue_cmd((void*)ctrl, ufunc, params) != 0) {
        free(params); json_err(fd, 503, "queue full"); return;
    }

    int wait_ms = 0;
    PendingCmd* last = &g_cmd_queue[(g_cmd_tail - 1 + CMD_QUEUE_SIZE) % CMD_QUEUE_SIZE];
    while (!last->done && wait_ms < 15000) { usleep(10000); wait_ms += 10; }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"exp\":%d,\"ctrl\":\"0x%lx\",\"wait_ms\":%d}",
        (last->done && last->result == 0) ? "success" : "failed", exp, ctrl, wait_ms);
    free(params);
    json_ok(fd, buf);
}

/*
 * /give-money — 给玩家加金币
 * POST {"amount":10000, "ctrl":"0x..."}
 * 调用 Debug_AddMoney_ToServer(int64 Amount)
 */
static void api_give_money(int fd, const char* req) {
    if (!g_GEngine || g_FNamePool == 0 || !g_hook_active) {
        json_err(fd, 503, "not ready");
        return;
    }

    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));

    int amount = json_get_int(body, "amount");
    if (amount <= 0) amount = 1000;
    if (amount > 9999999) amount = 9999999;

    /* 钱包金币 = Money物品(Gold Coin)的堆叠数, 直接走刷道具通道
     * (旧版Debug_AddMoney_ToServer已在新版游戏移除, 实测给Money物品钱包99->100) */
    char name[64] = {0};
    json_get_string(body, "name", name, sizeof(name));
    char fake[512];
    snprintf(fake, sizeof(fake), "{\"item_id\":\"Money\",\"count\":%d,\"name\":\"%s\"}", amount, name);
    give_item_impl(fd, fake);
}

/* 通用: 从body中找ctrl，没传就自动查找第一个玩家的Controller */
static uintptr_t resolve_ctrl(const char* body) {
    char ctrl_str[64] = {0};
    json_get_string(body, "ctrl", ctrl_str, sizeof(ctrl_str));
    if (ctrl_str[0]) {
        uintptr_t c = strtoull(ctrl_str, NULL, 0);
        if (c > 0x10000) return c;
    }
    /* 自动查找 */
    int ctrl_fname = find_fname_index("BP_PalPlayerController_C");
    if (ctrl_fname <= 0) return 0;
    uintptr_t segs[MAX_RW_REGIONS][2];
    int seg_count = snapshot_rw(segs, MAX_RW_REGIONS);
    for (int seg = 0; seg < seg_count; seg++) {
        uintptr_t* ptr = (uintptr_t*)segs[seg][0];
        size_t cnt = (segs[seg][1] - segs[seg][0]) / sizeof(uintptr_t);
        for (size_t i = 0; i < cnt; i++) {
            uintptr_t val = ptr[i];
            if (val < 0x10000 || val > 0x800000000000UL) continue;
            uintptr_t ov = 0;
            if (safe_read_ptr(val, &ov) != 0 || !is_plausible_vtable(ov)) continue;
            uintptr_t uc = 0;
            if (safe_read_ptr(val + 0x10, &uc) != 0) continue;
            uintptr_t ucv = 0;
            if (safe_read_ptr(uc, &ucv) != 0 || !is_uclass_vtable(ucv)) continue;
            uintptr_t fn = 0;
            if (safe_read_ptr(uc + 0x18, &fn) != 0) continue;
            if ((int)(fn & 0xFFFFFFFF) == ctrl_fname) {
                uintptr_t on = 0;
                safe_read_ptr(val + 0x18, &on);
                char nm[64] = {0};
                resolve_fname((int)(on & 0xFFFFFFFF), nm, sizeof(nm));
                if (strncmp(nm, "Default__", 9) != 0) return val;
            }
        }
    }
    return 0;
}

/* 通用: 调用无参/简单参数函数并等待结果 */
static int call_func_simple(uintptr_t obj, const char* func_name, void* params, int parms_size) {
    uintptr_t cls = 0;
    if (safe_read_ptr(obj + 0x10, &cls) != 0 || cls == 0) return -1;
    void* ufunc = find_ufunction_in_class(cls, func_name);
    if (!ufunc) return -2;

    /* 如果没给params但有ParmsSize，自动分配 */
    int allocated = 0;
    if (!params && parms_size == 0) {
        uintptr_t pr = 0;
        safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &pr);
        parms_size = pr & 0xFFFFFFFF;
    }
    if (!params && parms_size > 0) {
        params = calloc(1, parms_size);
        allocated = 1;
    }

    if (enqueue_cmd((void*)obj, ufunc, params) != 0) {
        if (allocated && params) free(params);
        return -3;
    }

    int wait_ms = 0;
    PendingCmd* last = &g_cmd_queue[(g_cmd_tail - 1 + CMD_QUEUE_SIZE) % CMD_QUEUE_SIZE];
    while (!last->done && wait_ms < 15000) { usleep(10000); wait_ms += 10; }

    int result = (last->done && last->result == 0) ? 0 : -4;
    if (allocated && params) free(params);
    return result;
}

/*
 * /teleport — 传送玩家
 * POST {"x":1000.0, "y":2000.0, "z":300.0}
 * 在玩家Character上调用 K2_TeleportTo(FVector, FRotator) ParmsSize=56
 * UE5.1 FVector/FRotator = 3 * double = 24字节
 * 自动查找BP_Player_Female_C或BP_Player_Male_C
 */
static uintptr_t find_player_character(void) {
    /* 缓存校验: 角色地址在玩家重新登录前不变, 校验通过直接复用 (省20秒全扫描) */
    if (g_cache_character > 0x10000) {
        uintptr_t ov = 0;
        if (safe_read_ptr(g_cache_character, &ov) == 0 && is_plausible_vtable(ov)) {
            uintptr_t uc = 0;
            if (safe_read_ptr(g_cache_character + 0x10, &uc) == 0) {
                uintptr_t ucv = 0;
                if (safe_read_ptr(uc, &ucv) == 0 && is_uclass_vtable(ucv)) {
                    uintptr_t fn = 0;
                    safe_read_ptr(uc + 0x18, &fn);
                    char cname[64] = {0};
                    resolve_fname((int)(fn & 0xFFFFFFFF), cname, sizeof(cname));
                    if (strcmp(cname, "BP_Player_Female_C") == 0 ||
                        strcmp(cname, "BP_Player_Male_C") == 0) {
                        return g_cache_character;
                    }
                }
            }
        }
    }
    const char* classes[] = {"BP_Player_Female_C", "BP_Player_Male_C", NULL};
    uintptr_t segs[MAX_RW_REGIONS][2];
    int seg_count = snapshot_rw(segs, MAX_RW_REGIONS);
    for (int c = 0; classes[c]; c++) {
        int fi = find_fname_index(classes[c]);
        if (fi <= 0) continue;
        for (int seg = 0; seg < seg_count; seg++) {
            uintptr_t* ptr = (uintptr_t*)segs[seg][0];
            size_t cnt = (segs[seg][1] - segs[seg][0]) / sizeof(uintptr_t);
            for (size_t i = 0; i < cnt; i++) {
                uintptr_t val = ptr[i];
                if (val < 0x10000 || val > 0x800000000000UL) continue;

                /* 验证对象自身vtable合法 (过滤垃圾对象) */
                uintptr_t ov = 0;
                if (safe_read_ptr(val, &ov) != 0 || !is_plausible_vtable(ov)) continue;

                uintptr_t uc = 0;
                if (safe_read_ptr(val + 0x10, &uc) != 0) continue;

                /* 验证val+0x10真的是UClass (vtable检查, 过滤存了对象指针的垃圾结构) */
                uintptr_t ucv = 0;
                if (safe_read_ptr(uc, &ucv) != 0 || !is_uclass_vtable(ucv)) continue;

                uintptr_t fn = 0;
                if (safe_read_ptr(uc + 0x18, &fn) != 0) continue;
                if ((int)(fn & 0xFFFFFFFF) == fi) {
                    uintptr_t on = 0;
                    safe_read_ptr(val + 0x18, &on);
                    char nm[64] = {0};
                    resolve_fname((int)(on & 0xFFFFFFFF), nm, sizeof(nm));
                    if (strncmp(nm, "Default__", 9) != 0) {
                        palhook_log("find_player_character: found 0x%lx name='%s' vt=0x%lx", val, nm, ov);
                        g_cache_character = val;
                        return val;
                    }
                }
            }
        }
    }
    return 0;
}

static void api_teleport(int fd, const char* req) {
    if (!g_GEngine || g_FNamePool == 0 || !g_hook_active) {
        json_err(fd, 503, "not ready");
        return;
    }

    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));
    install_segv_handler();

    /* 支持三种模式:
     * 1. {x,y,z} 坐标传送
     * 2. {"to":"玩家名"} 传送到某玩家身边 (name=移动谁, 默认第一个在线玩家)
     * 3. {"name":"A","to":"B"} 把A传到B身边 */
    char move_name[64] = {0};
    char to_name[64] = {0};
    json_get_string(body, "name", move_name, sizeof(move_name));
    json_get_string(body, "to", to_name, sizeof(to_name));

    /* 玩家列表 (含名字/坐标) */
    if (g_players_ts == 0 || time(NULL) - g_players_ts > 20) {
        collect_all_players();
    }
    fill_player_details();

    uintptr_t character = 0;
    if (move_name[0]) {
        for (int i = 0; i < g_players_count; i++) {
            if (strcmp(g_players[i].name, move_name) == 0) {
                character = g_players[i].character;
                break;
            }
        }
        if (!character) {
            char e[128];
            snprintf(e, sizeof(e), "player '%s' not found", move_name);
            json_err(fd, 404, e);
            return;
        }
    } else {
        character = find_player_character();
        if (!character) {
            json_err(fd, 404, "player character not found");
            return;
        }
    }

    double x = 0, y = 0, z = 0;
    const char* xp = strstr(body, "\"x\"");
    const char* yp = strstr(body, "\"y\"");
    const char* zp = strstr(body, "\"z\"");
    if (xp) { xp = strchr(xp, ':'); if (xp) x = strtod(xp + 1, NULL); }
    if (yp) { yp = strchr(yp, ':'); if (yp) y = strtod(yp + 1, NULL); }
    if (zp) { zp = strchr(zp, ':'); if (zp) z = strtod(zp + 1, NULL); }

    /* 传送到玩家身边: 取目标实时坐标 + 300横向偏移 (避免重叠) */
    if (to_name[0]) {
        uintptr_t to_char = 0;
        for (int i = 0; i < g_players_count; i++) {
            if (strcmp(g_players[i].name, to_name) == 0) {
                to_char = g_players[i].character;
                break;
            }
        }
        if (!to_char) {
            char e[128];
            snprintf(e, sizeof(e), "target player '%s' not found", to_name);
            json_err(fd, 404, e);
            return;
        }
        double tloc[3] = {0, 0, 0};
        if (actor_get_location(to_char, tloc) == 0) {
            x = tloc[0] + 300.0;
            y = tloc[1];
            z = tloc[2] + 100.0;
        }
    }

    palhook_log("teleport: x=%.1f y=%.1f z=%.1f char=0x%lx move='%s' to='%s'",
        x, y, z, character, move_name, to_name);

    /* 游戏自己的传送: PalUtility::Teleport(Target, Location, Rotation, bNoCheck, bAroundCheck)
     * bAroundCheck=true 会检查周边找有效落脚点 (不掉血) */
    uintptr_t pal_util = get_pal_utility();
    uintptr_t pcls = 0;
    safe_read_ptr(pal_util + 0x10, &pcls);
    void* ufunc = find_ufunction_in_class(pcls, "Teleport");
    if (!ufunc) {
        /* 兜底: 老方式 K2_TeleportTo */
        uintptr_t cls = 0;
        safe_read_ptr(character + 0x10, &cls);
        ufunc = find_ufunction_in_class(cls, "K2_TeleportTo");
        if (!ufunc) { json_err(fd, 404, "Teleport function not found"); return; }
        uint8_t* params56 = (uint8_t*)calloc(1, 56);
        *(double*)(params56 + 0) = x;
        *(double*)(params56 + 8) = y;
        *(double*)(params56 + 16) = z;
        int w2 = 0;
        int r2 = call_and_wait((void*)character, ufunc, params56, 15000, &w2);
        free(params56);
        char b2[256];
        snprintf(b2, sizeof(b2),
            "{\"status\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"method\":\"K2_TeleportTo\",\"waited_ms\":%d}",
            r2 == 0 ? "success" : "failed", x, y, z, w2);
        json_ok(fd, b2);
        return;
    }

    uintptr_t pr = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &pr);
    int parms_size = (int)(pr & 0xFFFFFFFF);
    if (parms_size <= 0 || parms_size > 256) parms_size = 128;

    uint8_t* params = (uint8_t*)calloc(1, parms_size);

    uintptr_t head = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_CHILDPROPS, &head);
    uintptr_t prop = head;
    int count = 0;
    while (prop && prop > 0x10000 && count < 12) {
        count++;
        char pname[128] = {0};
        prop_get_name(prop, pname, sizeof(pname));
        int sz = prop_get_size(prop);
        int off = prop_get_offset(prop);
        palhook_log("  Teleport param '%s' size=%d offset=0x%x", pname, sz, off);
        if (off >= 0 && off + sz <= parms_size) {
            if (strstr(pname, "Target") && sz == 8) {
                *(uintptr_t*)(params + off) = character;
            } else if (strstr(pname, "Location") && sz == 24) {
                *(double*)(params + off + 0) = x;
                *(double*)(params + off + 8) = y;
                *(double*)(params + off + 16) = z;
            } else if (strstr(pname, "Rotation") && sz == 32) {
                /* FQuat单位四元数: W=1 */
                *(double*)(params + off + 24) = 1.0;
            } else if (strstr(pname, "Rotation") && sz == 24) {
                /* FRotator: 保持全零 */
            } else if (strcmp(pname, "bAroundCheck") == 0 && sz == 1) {
                params[off] = 1; /* 周边检查找落脚点 */
            } else if (strcmp(pname, "bNoCheck") == 0 && sz == 1) {
                params[off] = 0;
            }
        }
        uintptr_t next = 0;
        if (safe_read_ptr(prop + FPROP_OFFSET_NEXT, &next) != 0) break;
        if (next == prop) break;
        prop = next;
    }

    int wait_ms = 0;
    int r = call_and_wait((void*)pal_util, ufunc, params, 15000, &wait_ms);
    free(params);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"method\":\"PalUtility::Teleport\","
        "\"move\":\"%s\",\"to\":\"%s\",\"waited_ms\":%d}",
        r == 0 ? "success" : "failed", x, y, z, move_name, to_name, wait_ms);
    json_ok(fd, buf);
}

/*
 * /spawn-pal — 在玩家附近刷稀有帕鲁
 * POST {"type":"rare"|"predator", "ctrl":"0x..."(可选)}
 * 调用 Debug_ForceSpawnRarePal_ToServer (无参数) 或
 *      Debug_ForceSpawnPredatorPal_ToServer (无参数)
 */
static void api_spawn_pal(int fd, const char* req) {
    if (!g_GEngine || g_FNamePool == 0 || !g_hook_active) {
        json_err(fd, 503, "not ready");
        return;
    }

    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));
    install_segv_handler();

    uintptr_t ctrl = resolve_ctrl(body);
    if (!ctrl) {
        json_err(fd, 404, "PlayerController not found");
        return;
    }

    char type[32] = {0};
    json_get_string(body, "type", type, sizeof(type));
    if (!type[0]) strcpy(type, "rare");

    const char* func_name = "Debug_ForceSpawnRarePal_ToServer";
    if (strcmp(type, "predator") == 0)
        func_name = "Debug_ForceSpawnPredatorPal_ToServer";

    palhook_log("spawn-pal: type=%s ctrl=0x%lx func=%s", type, ctrl, func_name);

    int ret = call_func_simple(ctrl, func_name, NULL, 0);

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"type\":\"%s\",\"func\":\"%s\",\"ctrl\":\"0x%lx\"}",
        ret == 0 ? "success" : "failed", type, func_name, ctrl);
    json_ok(fd, buf);
}

/*
 * /cheat — 执行作弊命令
 * POST {"cmd":"GiveItem ...", "ctrl":"0x..."(可选)}
 * 调用 Debug_CheatCommand_ToServer(FString Command)
 * FString: TArray<TCHAR> = {TCHAR* Data, int32 Num, int32 Max}
 * 在内存中: 8字节指针 + 4字节Num + 4字节Max = 16字节
 */
static void api_cheat(int fd, const char* req) {
    if (!g_GEngine || g_FNamePool == 0 || !g_hook_active) {
        json_err(fd, 503, "not ready");
        return;
    }

    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));
    install_segv_handler();

    uintptr_t ctrl = resolve_ctrl(body);
    if (!ctrl) {
        json_err(fd, 404, "PlayerController not found");
        return;
    }

    char cmd[512] = {0};
    json_get_string(body, "cmd", cmd, sizeof(cmd));
    if (!cmd[0]) {
        json_err(fd, 400, "cmd is required");
        return;
    }

    palhook_log("cheat: cmd='%s' ctrl=0x%lx", cmd, ctrl);

    /* 构造FString参数 (16字节): Data指针(8) + Num(4) + Max(4) */
    int cmd_len = strlen(cmd);

    /* 分配宽字符缓冲区 (UTF-16LE, UE用TCHAR=wchar_t在Linux上是4字节) */
    /* 实际上Linux UE5 TCHAR可能是char16_t(2字节)或wchar_t(4字节) */
    /* 先试char16_t (2字节) */
    int char_size = 2; /* UTF-16 */
    uint8_t* str_buf = (uint8_t*)calloc(1, (cmd_len + 1) * char_size);
    for (int i = 0; i < cmd_len; i++) {
        *(uint16_t*)(str_buf + i * char_size) = (uint16_t)cmd[i];
    }

    /* FString结构: {Data*, Num, Max} */
    uint8_t* params = (uint8_t*)calloc(1, 16);
    *(uintptr_t*)(params + 0) = (uintptr_t)str_buf;  /* Data pointer */
    *(int32_t*)(params + 8) = cmd_len + 1;            /* Num (including null) */
    *(int32_t*)(params + 12) = cmd_len + 1;           /* Max */

    uintptr_t cls = 0;
    safe_read_ptr(ctrl + 0x10, &cls);
    void* ufunc = find_ufunction_in_class(cls, "Debug_CheatCommand_ToServer");
    if (!ufunc) {
        free(params); free(str_buf);
        json_err(fd, 404, "Debug_CheatCommand_ToServer not found");
        return;
    }

    if (enqueue_cmd((void*)ctrl, ufunc, params) != 0) {
        free(params); free(str_buf);
        json_err(fd, 503, "queue full");
        return;
    }

    int wait_ms = 0;
    PendingCmd* last = &g_cmd_queue[(g_cmd_tail - 1 + CMD_QUEUE_SIZE) % CMD_QUEUE_SIZE];
    while (!last->done && wait_ms < 15000) { usleep(10000); wait_ms += 10; }

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"cmd\":\"%s\",\"ctrl\":\"0x%lx\",\"wait_ms\":%d}",
        (last->done && last->result == 0) ? "success" : "failed", cmd, ctrl, wait_ms);
    free(params);
    free(str_buf);
    json_ok(fd, buf);
}

/*
 * /set-tech-points — 直接写内存设置科技点
 * POST {"tech":100, "boss_tech":50}
 * 通过找PalTechnologyData对象，直接写 +0x150(TechnologyPoint) 和 +0x154(bossTechnologyPoint)
 */
static void api_set_tech_points(int fd, const char* req) {
    if (g_FNamePool == 0) { json_err(fd, 503, "not ready"); return; }

    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));
    install_segv_handler();

    int tech = json_get_int(body, "tech");
    int boss_tech = json_get_int(body, "boss_tech");
    int set_tech = (strstr(body, "\"tech\"") != NULL);
    int set_boss = (strstr(body, "\"boss_tech\"") != NULL);

    if (!set_tech && !set_boss) {
        json_err(fd, 400, "tech and/or boss_tech required");
        return;
    }

    /* 找PlayerState (指定玩家直接用玩家缓存里的playerstate) */
    char pname[64] = {0};
    json_get_string(body, "name", pname, sizeof(pname));
    uintptr_t ps = 0;
    if (pname[0]) {
        if (g_players_ts == 0 || time(NULL) - g_players_ts > 20) collect_all_players();
        fill_player_details();
        for (int i = 0; i < g_players_count && !ps; i++) {
            if (strcmp(g_players[i].name, pname) == 0) ps = g_players[i].playerstate;
        }
        if (!ps) {
            char e[128];
            snprintf(e, sizeof(e), "player '%s' not found", pname);
            json_err(fd, 404, e);
            return;
        }
    }
    int ps_fname = find_fname_index("BP_PalPlayerState_C");
    if (ps_fname <= 0) { json_err(fd, 404, "PlayerState class not found"); return; }

    for (int seg = 0; seg < g_all_rw_count && !ps; seg++) {
        uintptr_t* ptr = (uintptr_t*)g_all_rw[seg][0];
        size_t cnt = (g_all_rw[seg][1] - g_all_rw[seg][0]) / sizeof(uintptr_t);
        for (size_t i = 0; i < cnt; i++) {
            uintptr_t val = ptr[i];
            if (val < 0x10000 || val > 0x800000000000UL) continue;
            uintptr_t uc = 0;
            if (safe_read_ptr(val + 0x10, &uc) != 0) continue;
            uintptr_t fn = 0;
            if (safe_read_ptr(uc + 0x18, &fn) != 0) continue;
            if ((int)(fn & 0xFFFFFFFF) != ps_fname) continue;
            uintptr_t on = 0;
            safe_read_ptr(val + 0x18, &on);
            char nm[64] = {0};
            resolve_fname((int)(on & 0xFFFFFFFF), nm, sizeof(nm));
            if (strncmp(nm, "Default__", 9) != 0) { ps = val; break; }
        }
    }
    if (!ps) { json_err(fd, 404, "no player online"); return; }

    /* 读TechnologyData指针 at PlayerState+0x648 */
    uintptr_t tech_data = 0;
    if (safe_read_ptr(ps + 0x648, &tech_data) != 0 || tech_data < 0x10000) {
        json_err(fd, 500, "cannot read TechnologyData from PlayerState");
        return;
    }

    /* 读当前值 */
    int32_t old_tech = 0, old_boss = 0;
    safe_read_ptr(tech_data + 0x150, (uintptr_t*)&old_tech);
    old_tech = *(int32_t*)(void*)&old_tech; /* 只要低32位 */
    uintptr_t tmp = 0;
    safe_read_ptr(tech_data + 0x150, &tmp);
    old_tech = (int32_t)(tmp & 0xFFFFFFFF);
    safe_read_ptr(tech_data + 0x154, &tmp);
    old_boss = (int32_t)(tmp & 0xFFFFFFFF);

    /* 直接写内存 */
    if (set_tech && region_contains(tech_data + 0x150)) {
        *(int32_t*)(tech_data + 0x150) = tech;
        palhook_log("set-tech: TechnologyPoint %d -> %d", old_tech, tech);
    }
    if (set_boss && region_contains(tech_data + 0x154)) {
        *(int32_t*)(tech_data + 0x154) = boss_tech;
        palhook_log("set-tech: bossTechnologyPoint %d -> %d", old_boss, boss_tech);
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"success\",\"tech\":{\"old\":%d,\"new\":%d},\"boss_tech\":{\"old\":%d,\"new\":%d}}",
        old_tech, set_tech ? tech : old_tech,
        old_boss, set_boss ? boss_tech : old_boss);
    json_ok(fd, buf);
}

/*
 * /set-exp — 直接写内存设置经验值/等级
 * POST {"level":50}
 * 找到玩家Character的level属性直接改
 * 暂时通过PlayerState属性链搜索
 */
/*
 * /give-exp — 给经验 (正确方式: PalUtility::GiveExpToAroundPlayerCharacter)
 * POST {"exp":10000}
 *
 * 参考AdminCommands mod: PalUtility是UBlueprintFunctionLibrary子类
 * GiveExpToAroundPlayerCharacter(UObject* WorldCtx, FVector Location, float Radius, int32 Exp, bool bShare)
 * 在CDO(Default__PalUtility)上调ProcessEvent即可
 */
static void api_give_exp_v2(int fd, const char* req) {
    if (g_FNamePool == 0 || !g_hook_active) { json_err(fd, 503, "not ready"); return; }

    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));
    install_segv_handler();

    int exp = json_get_int(body, "exp");
    if (exp <= 0) exp = 1000;

    /* 找PalUtility的CDO (Default__PalUtility) */
    int pal_util_fname = find_fname_index("PalUtility");
    if (pal_util_fname <= 0) {
        json_err(fd, 404, "PalUtility class not found in FNamePool");
        return;
    }

    uintptr_t pal_util_cdo = 0;
    for (int seg = 0; seg < g_all_rw_count && !pal_util_cdo; seg++) {
        uintptr_t* ptr = (uintptr_t*)g_all_rw[seg][0];
        size_t cnt = (g_all_rw[seg][1] - g_all_rw[seg][0]) / sizeof(uintptr_t);
        for (size_t i = 0; i < cnt; i++) {
            uintptr_t val = ptr[i];
            if (val < 0x10000 || val > 0x800000000000UL) continue;
            uintptr_t uc = 0;
            if (safe_read_ptr(val + 0x10, &uc) != 0) continue;
            uintptr_t fn = 0;
            if (safe_read_ptr(uc + 0x18, &fn) != 0) continue;
            if ((int)(fn & 0xFFFFFFFF) != pal_util_fname) continue;
            /* 找到PalUtility实例/CDO */
            pal_util_cdo = val;
            break;
        }
    }

    if (!pal_util_cdo) {
        json_err(fd, 404, "PalUtility object not found in memory");
        return;
    }

    uintptr_t pal_util_cls = 0;
    safe_read_ptr(pal_util_cdo + 0x10, &pal_util_cls);

    /* 找GiveExpToAroundPlayerCharacter函数 */
    void* ufunc = find_ufunction_in_class(pal_util_cls, "GiveExpToAroundPlayerCharacter");
    if (!ufunc) {
        json_err(fd, 404, "GiveExpToAroundPlayerCharacter not found");
        return;
    }

    /* 读ParmsSize */
    uintptr_t parms_raw = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &parms_raw);
    int parms_size = parms_raw & 0xFFFFFFFF;
    palhook_log("give-exp-v2: PalUtil=0x%lx func=0x%lx parms=%d exp=%d",
        pal_util_cdo, (uintptr_t)ufunc, parms_size, exp);

    /* 需要找玩家Character来获取World和Location */
    uintptr_t character = find_player_character();
    if (!character) {
        json_err(fd, 404, "player character not found");
        return;
    }

    /* 读Character的位置: 调K2_GetActorLocation太复杂
     * UE5 Actor的位置在RootComponent->RelativeLocation
     * RootComponent at Actor+0x178 (varies), RelativeLocation at SceneComponent+0x120 (varies)
     * 简单起见，参数中的Location用零向量+大半径覆盖 */

    /* 构造参数:
     * GiveExpToAroundPlayerCharacter(WorldContextObject*, FVector, float, int32, bool)
     * UE5 BlueprintFunctionLibrary的第一个参数WorldContextObject通常是隐含的
     * ProcessEvent时需要包含: WorldCtx(ptr 8) + FVector(double*3=24) + float(4) + int32(4) + bool(1) + pad
     * 但具体布局需要看FProperty链 */

    /* 先读参数链确认布局 */
    uintptr_t child_props = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_CHILDPROPS, &child_props);

    /* 遍历属性获取offset和size */
    typedef struct { char name[64]; int size; int offset; } ParamInfo;
    ParamInfo params_info[8] = {0};
    int param_count = 0;

    if (child_props && child_props > 0x10000) {
        uintptr_t prop = child_props;
        while (prop && prop > 0x10000 && param_count < 8) {
            uintptr_t pv[16] = {0};
            for (int k = 0; k < 10; k++) safe_read_ptr(prop + k*8, &pv[k]);

            uintptr_t name_raw = pv[5]; /* +0x28 */
            int name_idx = name_raw & 0xFFFFFFFF;
            resolve_fname(name_idx, params_info[param_count].name, 64);

            params_info[param_count].size = pv[7] & 0xFFFFFFFF; /* +0x38 ElementSize */
            uintptr_t off_raw = pv[9]; /* +0x48 */
            params_info[param_count].offset = (off_raw >> 32) & 0xFFFFFFFF;

            palhook_log("  param[%d] '%s' size=%d offset=0x%x",
                param_count, params_info[param_count].name,
                params_info[param_count].size, params_info[param_count].offset);

            param_count++;
            uintptr_t next = pv[4]; /* +0x20 Next */
            prop = (next && next > 0x10000) ? next : 0;
        }
    }

    if (parms_size <= 0 || parms_size > 256) {
        json_err(fd, 500, "unexpected ParmsSize");
        return;
    }

    /* 构造参数缓冲区 */
    uint8_t* params = (uint8_t*)calloc(1, parms_size);

    for (int p = 0; p < param_count; p++) {
        const char* pn = params_info[p].name;
        int off = params_info[p].offset;
        int sz = params_info[p].size;

        if (strcmp(pn, "__WorldContext") == 0 || strstr(pn, "WorldContext")) {
            /* World指针: 从Character获取 */
            /* Actor::GetWorld — 读UObject->Outer链直到找到UWorld */
            uintptr_t outer = 0;
            safe_read_ptr(character + 0x20, &outer); /* UObject+0x20 = OuterPrivate */
            /* Outer通常指向Level, Level->Outer指向World */
            if (outer > 0x10000) {
                uintptr_t world = 0;
                safe_read_ptr(outer + 0x20, &world);
                if (world > 0x10000) {
                    *(uintptr_t*)(params + off) = world;
                    palhook_log("  WorldContext = 0x%lx", world);
                }
            }
        } else if (strcmp(pn, "Location") == 0 && sz == 24) {
            /* FVector: 0,0,0 (配合大半径覆盖整个地图) */
            *(double*)(params + off + 0) = 0.0;
            *(double*)(params + off + 8) = 0.0;
            *(double*)(params + off + 16) = 0.0;
        } else if (strstr(pn, "Radius") && sz == 4) {
            *(float*)(params + off) = 999999.0f; /* 超大半径 */
        } else if (strstr(pn, "Exp") && sz == 4) {
            *(int32_t*)(params + off) = exp;
        } else if (sz == 1) {
            params[off] = 1; /* bool = true */
        }
    }

    if (enqueue_cmd((void*)pal_util_cdo, ufunc, params) != 0) {
        free(params);
        json_err(fd, 503, "queue full");
        return;
    }

    int wait_ms = 0;
    PendingCmd* last = &g_cmd_queue[(g_cmd_tail - 1 + CMD_QUEUE_SIZE) % CMD_QUEUE_SIZE];
    while (!last->done && wait_ms < 15000) { usleep(10000); wait_ms += 10; }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"exp\":%d,\"method\":\"GiveExpToAroundPlayerCharacter\","
        "\"pal_utility\":\"0x%lx\",\"parms_size\":%d,\"wait_ms\":%d}",
        (last->done && last->result == 0) ? "success" : "failed",
        exp, pal_util_cdo, parms_size, wait_ms);
    free(params);
    json_ok(fd, buf);
}

/* ========== v0.8.0: 经验/刷帕鲁 (对照 AdminCommands mod 实现) ========== */

/* FPROP_OFFSET_* 定义已上移到全局前置声明区 */

/* 在UStruct的ChildProperties链中按名字找FProperty (沿SuperStruct链向上) */
static uintptr_t find_property_in_struct(uintptr_t ustruct, const char* prop_name) {
    if (!ustruct || !prop_name) return 0;
    install_segv_handler();
    uintptr_t cls = ustruct;
    for (int depth = 0; depth < 12 && cls > 0x10000; depth++) {
        uintptr_t head = 0;
        if (safe_read_ptr(cls + UFUNC_OFFSET_CHILDPROPS, &head) != 0) return 0;
        uintptr_t prop = head;
        int count = 0;
        while (prop && prop > 0x10000 && count < 500) {
            count++;
            uintptr_t name_raw = 0;
            if (safe_read_ptr(prop + FPROP_OFFSET_NAME, &name_raw) != 0) break;
            char name[256] = {0};
            if (resolve_fname((int)(name_raw & 0xFFFFFFFF), name, sizeof(name)) > 0) {
                if (strcmp(name, prop_name) == 0) return prop;
            }
            uintptr_t next = 0;
            if (safe_read_ptr(prop + FPROP_OFFSET_NEXT, &next) != 0) break;
            if (next == prop) break;
            prop = next;
        }
        uintptr_t super = 0;
        if (safe_read_ptr(cls + UCLASS_OFFSET_SUPER, &super) != 0) return 0;
        if (super < 0x10000 || super == cls) return 0;
        cls = super;
    }
    return 0;
}

/* 读FProperty的Offset_Internal (本构建在高32位) */
static int prop_get_offset(uintptr_t prop) {
    uintptr_t raw = 0;
    if (safe_read_ptr(prop + FPROP_OFFSET_INTERNAL, &raw) != 0) return -1;
    return (int)((raw >> 32) & 0xFFFFFFFF);
}

/* 读FProperty的ElementSize */
static int prop_get_size(uintptr_t prop) {
    uintptr_t raw = 0;
    if (safe_read_ptr(prop + FPROP_OFFSET_ELEMSIZE, &raw) != 0) return -1;
    return (int)(raw & 0xFFFFFFFF);
}

/* 读FProperty的名字 */
static int prop_get_name(uintptr_t prop, char* out, int out_size) {
    uintptr_t name_raw = 0;
    if (safe_read_ptr(prop + FPROP_OFFSET_NAME, &name_raw) != 0) return -1;
    return resolve_fname((int)(name_raw & 0xFFFFFFFF), out, out_size);
}

/* 入队+等待结果, waited_out返回等待毫秒数 */
static int call_and_wait(void* obj, void* ufunc, void* params, int timeout_ms, int* waited_out) {
    if (enqueue_cmd(obj, ufunc, params) != 0) return -1;
    PendingCmd* last = &g_cmd_queue[(g_cmd_tail - 1 + CMD_QUEUE_SIZE) % CMD_QUEUE_SIZE];
    int waited = 0;
    while (!last->done && waited < timeout_ms) { usleep(10000); waited += 10; }
    if (waited_out) *waited_out = waited;
    if (!last->done) return -2;
    return last->result;
}

/* 扫描类名为指定名称的对象实例 (全部收集, 带vtable验证) */
static int collect_objects_by_class(const char* class_name, uintptr_t* out, int max_n) {
    int fi = find_fname_index(class_name);
    if (fi <= 0) return 0;
    int n = 0;
    uintptr_t segs[MAX_RW_REGIONS][2];
    int seg_count = snapshot_rw(segs, MAX_RW_REGIONS);
    for (int seg = 0; seg < seg_count; seg++) {
        uintptr_t* ptr = (uintptr_t*)segs[seg][0];
        size_t cnt = (segs[seg][1] - segs[seg][0]) / sizeof(uintptr_t);
        for (size_t i = 0; i < cnt && n < max_n; i++) {
            uintptr_t val = ptr[i];
            if (val < 0x10000 || val > 0x800000000000UL) continue;
            uintptr_t ov = 0;
            if (safe_read_ptr(val, &ov) != 0 || !is_plausible_vtable(ov)) continue;
            uintptr_t uc = 0;
            if (safe_read_ptr(val + 0x10, &uc) != 0) continue;
            uintptr_t ucv = 0;
            if (safe_read_ptr(uc, &ucv) != 0 || !is_uclass_vtable(ucv)) continue;
            uintptr_t fn = 0;
            if (safe_read_ptr(uc + 0x18, &fn) != 0) continue;
            if ((int)(fn & 0xFFFFFFFF) != fi) continue;
            int dup = 0;
            for (int k = 0; k < n; k++) if (out[k] == val) { dup = 1; break; }
            if (!dup) out[n++] = val;
        }
    }
    return n;
}

/* 扫描类名为指定名称的对象实例 (第一个匹配, 带vtable验证) */
static uintptr_t find_object_by_class_name(const char* class_name) {
    uintptr_t objs[64];
    int n = collect_objects_by_class(class_name, objs, 64);
    return n > 0 ? objs[0] : 0;
}

/* 通过Outer链找UWorld (类名为"World"的UObject) */
static uintptr_t get_object_world(uintptr_t obj) {
    uintptr_t cur = obj;
    for (int i = 0; i < 16 && cur > 0x10000; i++) {
        uintptr_t cls = 0;
        if (safe_read_ptr(cur + 0x10, &cls) != 0) return 0;
        uintptr_t fn = 0;
        if (safe_read_ptr(cls + 0x18, &fn) != 0) return 0;
        char name[64] = {0};
        resolve_fname((int)(fn & 0xFFFFFFFF), name, sizeof(name));
        palhook_log("  outer[%d] 0x%lx class='%s'", i, cur, name);
        if (strcmp(name, "World") == 0) return cur;
        uintptr_t outer = 0;
        if (safe_read_ptr(cur + 0x20, &outer) != 0) return 0;
        cur = outer;
    }
    return 0;
}

/* 探测FStructProperty里的UScriptStruct*指针 (偏移未确定, 运行时探测) */
static uintptr_t find_struct_ptr(uintptr_t fprop, const char* expect_name) {
    install_segv_handler();
    static const uintptr_t cand[] = {0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x98};
    uintptr_t fallback = 0;
    for (int k = 0; k < 10; k++) {
        uintptr_t sp = 0;
        if (safe_read_ptr(fprop + cand[k], &sp) != 0) continue;
        if (sp < 0x10000 || sp > 0x800000000000UL) continue;
        uintptr_t fn = 0;
        if (safe_read_ptr(sp + 0x18, &fn) != 0) continue;
        char name[128] = {0};
        if (resolve_fname((int)(fn & 0xFFFFFFFF), name, sizeof(name)) <= 0) continue;
        palhook_log("    struct probe +0x%lx -> 0x%lx name='%s'", cand[k], sp, name);
        if (!fallback) fallback = sp;
        if (expect_name && expect_name[0] && strstr(name, expect_name)) return sp;
    }
    return fallback;
}

/* 同find_struct_ptr但要求struct名精确匹配 (避免子串误匹配) */
static uintptr_t find_struct_ptr_exact(uintptr_t fprop, const char* expect_name) {
    static const uintptr_t cand[] = {0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x98};
    for (int k = 0; k < 10; k++) {
        uintptr_t sp = 0;
        if (safe_read_ptr(fprop + cand[k], &sp) != 0) continue;
        if (sp < 0x10000 || sp > 0x800000000000UL) continue;
        uintptr_t fn = 0;
        if (safe_read_ptr(sp + 0x18, &fn) != 0) continue;
        char name[128] = {0};
        if (resolve_fname((int)(fn & 0xFFFFFFFF), name, sizeof(name)) <= 0) continue;
        if (expect_name && expect_name[0]) {
            if (strcmp(name, expect_name) == 0) return sp;
        } else {
            /* 无名字要求时: 跳过空名/None */
            if (name[0] && strcmp(name, "None") != 0) return sp;
        }
    }
    return 0;
}

/* 取任意FProperty引用的UScriptStruct*:
 * FStructProperty直接探测; FArrayProperty经Inner(+0x50附近)间接探测 */
static uintptr_t prop_get_struct(uintptr_t prop, const char* exact_name) {
    uintptr_t st = find_struct_ptr_exact(prop, exact_name);
    if (st) return st;
    static const uintptr_t cand[] = {0x50, 0x58, 0x60, 0x68, 0x70, 0x78};
    for (int k = 0; k < 6; k++) {
        uintptr_t inner = 0;
        if (safe_read_ptr(prop + cand[k], &inner) != 0) continue;
        if (inner < 0x10000 || inner > 0x800000000000UL) continue;
        st = find_struct_ptr_exact(inner, exact_name);
        if (st) return st;
    }
    return 0;
}

/* 读UScriptStruct的PropertiesSize (与UClass同布局: +0x58) */
static int struct_get_propsize(uintptr_t ustruct) {
    uintptr_t raw = 0;
    if (safe_read_ptr(ustruct + 0x58, &raw) != 0) return -1;
    return (int)(raw & 0xFFFFFFFF);
}

/* ========== 全局对象缓存 (全内存扫描一次20秒, 必须缓存) ========== */
static uintptr_t g_cache_pal_utility = 0;
static uintptr_t g_cache_world = 0;
static uintptr_t g_cache_ksl = 0;       /* KismetStringLibrary CDO */
static uintptr_t g_cache_npc_manager = 0;
static uintptr_t g_cache_aic_class = 0;

static uintptr_t get_pal_utility(void) {
    if (!g_cache_pal_utility) g_cache_pal_utility = find_object_by_class_name("PalUtility");
    return g_cache_pal_utility;
}

static uintptr_t get_ksl(void) {
    if (!g_cache_ksl) g_cache_ksl = find_object_by_class_name("KismetStringLibrary");
    return g_cache_ksl;
}

/* ========== 公会 (PalGroupManager) ========== */
static uintptr_t g_cache_group_manager = 0;

/* TMap头部健全性检查: data指针在区域内且num为合理值 */
static int tmap_looks_live(uintptr_t map_addr) {
    uintptr_t data_ptr = 0, num_raw = 0, max_raw = 0;
    if (safe_read_ptr(map_addr + 0, &data_ptr) != 0) return 0;
    if (safe_read_ptr(map_addr + 8, &num_raw) != 0) return 0;
    if (safe_read_ptr(map_addr + 12, &max_raw) != 0) return 0;
    int num = (int)(num_raw & 0xFFFFFFFF);
    int max = (int)(max_raw & 0xFFFFFFFF);
    if (num <= 0 || num > 64) return 0;
    if (max < num || max > 4096) return 0;
    if (data_ptr < 0x10000 || data_ptr > 0x800000000000UL) return 0;
    if (!region_contains(data_ptr)) return 0;
    return 1;
}

/* Outer链是否到达World (USubsystem实例的Outer是UWorld) */
static int outer_chain_has_world(uintptr_t obj) {
    uintptr_t cur = obj;
    for (int i = 0; i < 8 && cur > 0x10000; i++) {
        uintptr_t cls = 0;
        if (safe_read_ptr(cur + 0x10, &cls) != 0 || cls < 0x10000) return 0;
        uintptr_t ucv = 0;
        if (safe_read_ptr(cls, &ucv) != 0 || !is_uclass_vtable(ucv)) return 0;
        uintptr_t fn = 0;
        if (safe_read_ptr(cls + 0x18, &fn) != 0) return 0;
        char name[64] = {0};
        resolve_fname((int)(fn & 0xFFFFFFFF), name, sizeof(name));
        if (strcmp(name, "World") == 0) return 1;
        uintptr_t outer = 0;
        if (safe_read_ptr(cur + 0x20, &outer) != 0) return 0;
        if (outer < 0x10000 || outer == cur) return 0;
        cur = outer;
    }
    return 0;
}

static uintptr_t get_group_manager(void) {
    if (g_cache_group_manager) return g_cache_group_manager;
    uintptr_t objs[64];
    int n = collect_objects_by_class("PalGroupManager", objs, 64);
    /* 1. Outer链到World 且 GuildMap/GroupMap 有内容 */
    for (int i = 0; i < n; i++) {
        if (outer_chain_has_world(objs[i]) && (tmap_looks_live(objs[i] + 0xf8) || tmap_looks_live(objs[i] + 0xa0))) {
            palhook_log("group-manager: live(World) instance 0x%lx (%d candidates)", objs[i], n);
            g_cache_group_manager = objs[i];
            return g_cache_group_manager;
        }
    }
    /* 2. Outer链到World (公会为空也选它) */
    for (int i = 0; i < n; i++) {
        if (outer_chain_has_world(objs[i])) {
            palhook_log("group-manager: World-owned instance 0x%lx (%d candidates)", objs[i], n);
            g_cache_group_manager = objs[i];
            return g_cache_group_manager;
        }
    }
    /* 3. 兜底: TMap有内容 */
    for (int i = 0; i < n; i++) {
        if (tmap_looks_live(objs[i] + 0xf8) || tmap_looks_live(objs[i] + 0xa0)) {
            palhook_log("group-manager: fallback instance 0x%lx (%d candidates)", objs[i], n);
            g_cache_group_manager = objs[i];
            return g_cache_group_manager;
        }
    }
    palhook_log("group-manager: no live instance among %d candidates (first=0x%lx)", n, n > 0 ? objs[0] : 0);
    g_cache_group_manager = n > 0 ? objs[0] : 0;
    return g_cache_group_manager;
}

/* 判断对象的类链(含超类)是否包含指定类名 */
static int class_chain_contains(uintptr_t obj, const char* class_name) {
    if (!obj || obj < 0x10000 || obj > 0x800000000000UL) return 0;
    uintptr_t uc = 0;
    if (safe_read_ptr(obj + 0x10, &uc) != 0 || uc < 0x10000) return 0;
    uintptr_t ucv = 0;
    if (safe_read_ptr(uc, &ucv) != 0 || !is_uclass_vtable(ucv)) return 0;
    uintptr_t cur = uc;
    for (int d = 0; d < 8 && cur > 0x10000; d++) {
        uintptr_t fn = 0;
        if (safe_read_ptr(cur + 0x18, &fn) != 0) return 0;
        char name[128] = {0};
        if (resolve_fname((int)(fn & 0xFFFFFFFF), name, sizeof(name)) > 0) {
            if (strcmp(name, class_name) == 0) return 1;
        }
        uintptr_t super = 0;
        if (safe_read_ptr(cur + UCLASS_OFFSET_SUPER, &super) != 0) return 0;
        if (super < 0x10000 || super == cur) return 0;
        cur = super;
    }
    return 0;
}

/* 完整校验: 对象vtable + UClass vtable + 类链含PalGroupGuildBase */
static int is_guild_object(uintptr_t ptr) {
    if (ptr < 0x10000 || ptr > 0x800000000000UL) return 0;
    uintptr_t ov = 0;
    if (safe_read_ptr(ptr, &ov) != 0 || !is_plausible_vtable(ov)) return 0;
    return class_chain_contains(ptr, "PalGroupGuildBase");
}

/* FGuid@addr 格式化成32位hex字符串 */
static void format_guid(uintptr_t addr, char* out) {
    uintptr_t t0 = 0, t1 = 0;
    safe_read_ptr(addr, &t0);
    safe_read_ptr(addr + 8, &t1);
    snprintf(out, 64, "%08X%08X%08X%08X",
        (unsigned)(t0 & 0xFFFFFFFF), (unsigned)((t0 >> 32) & 0xFFFFFFFF),
        (unsigned)(t1 & 0xFFFFFFFF), (unsigned)((t1 >> 32) & 0xFFFFFFFF));
}

/* 解析TMap<FGuid, UObject*> (UE5.1: TSet{TSparseArray Elements{Data TArray, inline...}, Hash, HashSize})
 * 返回value指针个数; 元素步长在{24,32,40,48}中探测取验证通过最多者 */
static int tmap_enum_guid_values(uintptr_t map_addr, int (*validator)(uintptr_t),
                                 uintptr_t* out, int max_n, int* out_stride) {
    if (!map_addr || map_addr < 0x10000) return 0;
    uintptr_t data_ptr = 0, num_raw = 0, max_raw = 0;
    if (safe_read_ptr(map_addr + 0, &data_ptr) != 0) return 0;
    if (safe_read_ptr(map_addr + 8, &num_raw) != 0) return 0;
    if (safe_read_ptr(map_addr + 12, &max_raw) != 0) return 0;
    int num = (int)(num_raw & 0xFFFFFFFF);
    int max = (int)(max_raw & 0xFFFFFFFF);
    if (num <= 0 || num > 1024 || max <= 0 || max > 8192) return 0;
    if (data_ptr < 0x10000 || data_ptr > 0x800000000000UL) return 0;
    if (!region_contains(data_ptr)) return 0;

    static const int strides[] = {32, 24, 40, 48};
    int best_stride = 0, best_count = 0;
    uintptr_t best_vals[64];
    for (int s = 0; s < 4; s++) {
        int stride = strides[s];
        /* 步长过大时num*stride超出实际容量, 直接跳过该步长 */
        if ((uintptr_t)num * (uintptr_t)stride > (uintptr_t)(max + 8) * 64) continue;
        uintptr_t vals[64];
        int nv = 0;
        for (int i = 0; i < num && nv < 64; i++) {
            uintptr_t e = data_ptr + (uintptr_t)i * (uintptr_t)stride;
            if (!region_contains(e + 16)) break;
            uintptr_t v = 0;
            if (safe_read_ptr(e + 16, &v) != 0) continue;
            if (v && validator(v)) {
                int dup = 0;
                for (int k = 0; k < nv; k++) if (vals[k] == v) { dup = 1; break; }
                if (!dup) vals[nv++] = v;
            }
        }
        palhook_log("tmap_enum: stride=%d num=%d valid=%d", stride, num, nv);
        if (nv > best_count) {
            best_count = nv;
            best_stride = stride;
            for (int k = 0; k < nv; k++) best_vals[k] = vals[k];
        }
    }
    if (best_count <= 0 || best_count > max_n) return 0;
    for (int k = 0; k < best_count; k++) out[k] = best_vals[k];
    if (out_stride) *out_stride = best_stride;
    return best_count;
}

/* 读公会字段: 优先反射属性名, 找不到用兜底偏移 */
static int guild_prop_offset(uintptr_t gcls, const char* propname, int fallback) {
    uintptr_t p = find_property_in_struct(gcls, propname);
    int off = p ? prop_get_offset(p) : -1;
    if (off < 0) return fallback;
    return off;
}

/* FString转FName: 调KismetStringLibrary::Conv_StringToName (任意名字都会intern进FNamePool) */
static int string_to_fname(const char* name, int* out_idx) {
    if (out_idx) *out_idx = -1;
    uintptr_t ksl = get_ksl();
    if (!ksl) { palhook_log("string_to_fname: KismetStringLibrary not found"); return -1; }
    uintptr_t cls = 0;
    if (safe_read_ptr(ksl + 0x10, &cls) != 0 || cls == 0) return -2;
    void* ufunc = find_ufunction_in_class(cls, "Conv_StringToName");
    if (!ufunc) { palhook_log("string_to_fname: Conv_StringToName not found"); return -3; }
    uintptr_t pr = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &pr);
    int parms_size = (int)(pr & 0xFFFFFFFF);
    if (parms_size <= 0 || parms_size > 256) {
        palhook_log("string_to_fname: bad ParmsSize %d", parms_size);
        return -4;
    }

    int len = (int)strlen(name);
    int char_size = 2; /* 实测: 本构建TCHAR=UTF-16 (2字节) */
    uint8_t* str_buf = (uint8_t*)calloc(1, (len + 1) * char_size);
    for (int i = 0; i < len; i++) {
        *(uint16_t*)(str_buf + i * char_size) = (uint8_t)name[i];
    }

    uint8_t* params = (uint8_t*)calloc(1, parms_size);
    int str_off = -1, rv_off = -1;
    uintptr_t head = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_CHILDPROPS, &head);
    uintptr_t prop = head;
    int count = 0;
    while (prop && prop > 0x10000 && count < 8) {
        count++;
        char pname[128] = {0};
        prop_get_name(prop, pname, sizeof(pname));
        int sz = prop_get_size(prop);
        int off = prop_get_offset(prop);
        palhook_log("  Conv_StringToName param '%s' size=%d offset=0x%x", pname, sz, off);
        if (off >= 0) {
            if (strstr(pname, "InString") || (sz == 16 && str_off < 0)) str_off = off;
            if (strcmp(pname, "ReturnValue") == 0) rv_off = off;
        }
        uintptr_t next = 0;
        if (safe_read_ptr(prop + FPROP_OFFSET_NEXT, &next) != 0) break;
        if (next == prop) break;
        prop = next;
    }
    if (str_off < 0 || rv_off < 0 || str_off + 16 > parms_size || rv_off + 8 > parms_size) {
        palhook_log("string_to_fname: params not found str=%d rv=%d", str_off, rv_off);
        free(str_buf); free(params);
        return -5;
    }
    *(uintptr_t*)(params + str_off + 0) = (uintptr_t)str_buf;
    *(int32_t*)(params + str_off + 8) = len + 1;
    *(int32_t*)(params + str_off + 12) = len + 1;

    int waited = 0;
    int r = call_and_wait((void*)ksl, ufunc, params, 15000, &waited);
    int idx = -1;
    if (r == 0) {
        uint64_t fn = *(uint64_t*)(params + rv_off);
        idx = (int)(fn & 0xFFFFFFFF);
        char verify[256] = {0};
        if (resolve_fname(idx, verify, sizeof(verify)) > 0) {
            palhook_log("string_to_fname: '%s' -> idx=%d verify='%s'", name, idx, verify);
            /* UE FName比较大小写不敏感: 池里已有其他大小写条目时会返回已有索引 */
            if (strcasecmp(verify, name) != 0) {
                palhook_log("  verify MISMATCH, discarding");
                idx = -1;
            }
        } else {
            palhook_log("string_to_fname: idx=%d resolve failed", idx);
        }
    }
    free(str_buf);
    free(params);
    if (out_idx) *out_idx = idx;
    return (idx > 0) ? 0 : -6;
}

/* 读取Actor坐标:
 * 1. 优先K2_GetActorLocation (UFunction)
 * 2. 内存直读: RootComponent属性 -> RelativeLocation属性 (本构建K2_GetActorLocation不存在) */
static int actor_get_location(uintptr_t actor, double* out) {
    uintptr_t cls = 0;
    if (safe_read_ptr(actor + 0x10, &cls) != 0 || cls == 0) return -1;

    void* ufunc = find_ufunction_in_class(cls, "K2_GetActorLocation");
    if (ufunc) {
        uintptr_t pr = 0;
        safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &pr);
        int parms_size = (int)(pr & 0xFFFFFFFF);
        if (parms_size <= 0 || parms_size > 256) parms_size = 24;
        uint8_t* params = (uint8_t*)calloc(1, parms_size);
        int waited = 0;
        int r = call_and_wait((void*)actor, ufunc, params, 15000, &waited);
        if (r == 0) {
            uintptr_t rv_prop = find_property_in_struct((uintptr_t)ufunc, "ReturnValue");
            int off = rv_prop ? prop_get_offset(rv_prop) : 0;
            if (off < 0 || off + 24 > parms_size) off = 0;
            out[0] = *(double*)(params + off + 0);
            out[1] = *(double*)(params + off + 8);
            out[2] = *(double*)(params + off + 16);
            palhook_log("actor_get_location(ufunc): (%.1f, %.1f, %.1f)", out[0], out[1], out[2]);
        }
        free(params);
        return r;
    }

    /* 内存直读: RootComponent -> RelativeLocation */
    uintptr_t rc_prop = find_property_in_struct(cls, "RootComponent");
    if (!rc_prop) { palhook_log("actor_get_location: RootComponent property not found"); return -3; }
    int rc_off = prop_get_offset(rc_prop);
    if (rc_off < 0) return -4;
    uintptr_t root = 0;
    if (safe_read_ptr(actor + rc_off, &root) != 0 || root < 0x10000) return -5;

    uintptr_t rc_cls = 0;
    if (safe_read_ptr(root + 0x10, &rc_cls) != 0) return -6;
    uintptr_t rl_prop = find_property_in_struct(rc_cls, "RelativeLocation");
    if (!rl_prop) { palhook_log("actor_get_location: RelativeLocation property not found"); return -7; }
    int rl_off = prop_get_offset(rl_prop);
    if (rl_off < 0) return -8;
    if (!region_contains(root + rl_off + 16)) return -9;

    out[0] = *(double*)(root + rl_off + 0);
    out[1] = *(double*)(root + rl_off + 8);
    out[2] = *(double*)(root + rl_off + 16);
    palhook_log("actor_get_location(mem): root=0x%lx off=0x%x -> (%.1f, %.1f, %.1f)",
        root, rl_off, out[0], out[1], out[2]);
    return 0;
}

/* 调用 PalUtility:GetNPCManager(WorldContextObject) -> UNPCManager* */
static uintptr_t get_npc_manager(uintptr_t pal_util, uintptr_t world, char* err, int err_size) {
    uintptr_t cls = 0;
    if (safe_read_ptr(pal_util + 0x10, &cls) != 0 || cls == 0) {
        snprintf(err, err_size, "cannot read PalUtility class"); return 0;
    }
    void* ufunc = find_ufunction_in_class(cls, "GetNPCManager");
    if (!ufunc) { snprintf(err, err_size, "GetNPCManager not found"); return 0; }
    uintptr_t pr = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &pr);
    int parms_size = (int)(pr & 0xFFFFFFFF);
    if (parms_size <= 0 || parms_size > 256) { snprintf(err, err_size, "bad ParmsSize"); return 0; }

    uint8_t* params = (uint8_t*)calloc(1, parms_size);
    int rv_off = -1;
    uintptr_t head = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_CHILDPROPS, &head);
    uintptr_t prop = head;
    int count = 0;
    while (prop && prop > 0x10000 && count < 16) {
        count++;
        char pname[128] = {0};
        prop_get_name(prop, pname, sizeof(pname));
        int sz = prop_get_size(prop);
        int off = prop_get_offset(prop);
        palhook_log("  GetNPCManager param '%s' size=%d offset=0x%x", pname, sz, off);
        if (off >= 0 && sz == 8) {
            if (strstr(pname, "WorldContext")) {
                *(uintptr_t*)(params + off) = world;
            } else if (strcmp(pname, "ReturnValue") == 0) {
                rv_off = off;
            }
        }
        uintptr_t next = 0;
        if (safe_read_ptr(prop + FPROP_OFFSET_NEXT, &next) != 0) break;
        if (next == prop) break;
        prop = next;
    }

    int waited = 0;
    int r = call_and_wait((void*)pal_util, ufunc, params, 15000, &waited);
    uintptr_t npc = 0;
    if (r == 0 && rv_off >= 0 && rv_off + 8 <= parms_size) {
        npc = *(uintptr_t*)(params + rv_off);
    }
    palhook_log("GetNPCManager: r=%d npc=0x%lx waited=%d", r, npc, waited);
    free(params);
    if (!npc) snprintf(err, err_size, "GetNPCManager returned null (r=%d)", r);
    return npc;
}

/*
 * /give-exp — 给经验 v3 (对照 AdminCommands items.lua)
 * POST {"exp":10000}
 * 流程: 玩家Character -> K2_GetActorLocation真实坐标 -> Outer链找UWorld
 *       PalUtility CDO -> GiveExpToAroundPlayerCharacter(WorldContextObject, Center, Radius, Exp, bCallDelegate)
 * 修复: 参数名是Center不是Location; 之前用(0,0,0)坐标+超大半径可能无效
 */
static void api_give_exp_v3(int fd, const char* req) {
    if (g_FNamePool == 0 || !g_hook_active) { json_err(fd, 503, "not ready"); return; }

    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));
    install_segv_handler();

    int exp = json_get_int(body, "exp");
    if (exp <= 0) exp = 1000;
    char pname[64] = {0};
    json_get_string(body, "name", pname, sizeof(pname));

    /* 1. 玩家Character (指定玩家则用该玩家的角色) */
    uintptr_t character = 0;
    if (pname[0]) {
        if (g_players_ts == 0 || time(NULL) - g_players_ts > 20) collect_all_players();
        fill_player_details();
        for (int i = 0; i < g_players_count && !character; i++) {
            if (strcmp(g_players[i].name, pname) == 0) character = g_players[i].character;
        }
        if (!character) {
            char e[128];
            snprintf(e, sizeof(e), "player '%s' not found", pname);
            json_err(fd, 404, e);
            return;
        }
    } else {
        character = find_player_character();
        if (!character) { json_err(fd, 404, "player character not found"); return; }
    }

    /* 2. 玩家真实坐标 */
    double loc[3] = {0, 0, 0};
    int lr = actor_get_location(character, loc);

    /* 3. UWorld (Outer链, 缓存) */
    uintptr_t world = get_object_world(character);
    if (!world) {
        if (!g_cache_world) g_cache_world = find_object_by_class_name("World");
        world = g_cache_world;
    }
    if (!world) { json_err(fd, 404, "UWorld not found"); return; }

    /* 4. PalUtility CDO (缓存) */
    uintptr_t pal_util = get_pal_utility();
    if (!pal_util) { json_err(fd, 404, "PalUtility object not found"); return; }
    uintptr_t pal_util_cls = 0;
    safe_read_ptr(pal_util + 0x10, &pal_util_cls);

    /* 5. GiveExpToAroundPlayerCharacter */
    void* ufunc = find_ufunction_in_class(pal_util_cls, "GiveExpToAroundPlayerCharacter");
    if (!ufunc) { json_err(fd, 404, "GiveExpToAroundPlayerCharacter not found"); return; }

    uintptr_t pr = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &pr);
    int parms_size = (int)(pr & 0xFFFFFFFF);
    if (parms_size <= 0 || parms_size > 256) { json_err(fd, 500, "unexpected ParmsSize"); return; }

    uint8_t* params = (uint8_t*)calloc(1, parms_size);

    /* 按属性链填充参数 */
    uintptr_t head = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_CHILDPROPS, &head);
    uintptr_t prop = head;
    int count = 0;
    while (prop && prop > 0x10000 && count < 16) {
        count++;
        char pname[128] = {0};
        prop_get_name(prop, pname, sizeof(pname));
        int sz = prop_get_size(prop);
        int off = prop_get_offset(prop);
        palhook_log("  param[%d] '%s' size=%d offset=0x%x", count - 1, pname, sz, off);
        if (off >= 0 && off + sz <= parms_size) {
            if (strstr(pname, "WorldContext")) {
                *(uintptr_t*)(params + off) = world;
            } else if (strcmp(pname, "Center") == 0 && sz == 24) {
                *(double*)(params + off + 0) = loc[0];
                *(double*)(params + off + 8) = loc[1];
                *(double*)(params + off + 16) = loc[2];
            } else if (strstr(pname, "Radius") && sz == 4) {
                *(float*)(params + off) = 1000.0f;
            } else if (strstr(pname, "Exp") && sz == 4) {
                /* SDK签名: float Exp (不是int32!) 之前写成int32导致float位模式≈0 */
                *(float*)(params + off) = (float)exp;
            } else if (sz == 1) {
                params[off] = 1; /* bool = true (bCallDelegate) */
            }
        }
        uintptr_t next = 0;
        if (safe_read_ptr(prop + FPROP_OFFSET_NEXT, &next) != 0) break;
        if (next == prop) break;
        prop = next;
    }

    palhook_log("give-exp-v3: exp=%d world=0x%lx pal_util=0x%lx loc=(%.1f,%.1f,%.1f) loc_r=%d",
        exp, world, pal_util, loc[0], loc[1], loc[2], lr);

    int waited = 0;
    int r = call_and_wait((void*)pal_util, ufunc, params, 15000, &waited);
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"exp\":%d,\"world\":\"0x%lx\",\"loc\":[%.1f,%.1f,%.1f],\"waited_ms\":%d}",
        r == 0 ? "success" : "failed", exp, world, loc[0], loc[1], loc[2], waited);
    free(params);
    json_ok(fd, buf);
}

/* 无参UFUNCTION调用 (AI激活用) */
static int call_func_no_params(uintptr_t obj, const char* name) {
    uintptr_t cls = 0;
    if (safe_read_ptr(obj + 0x10, &cls) != 0 || cls == 0) return -1;
    void* ufunc = find_ufunction_in_class(cls, name);
    if (!ufunc) { palhook_log("activate: %s not found", name); return -2; }
    uintptr_t pr = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &pr);
    int psz = (int)(pr & 0xFFFFFFFF);
    if (psz <= 0 || psz > 64) psz = 8;
    uint8_t* params = (uint8_t*)calloc(1, psz);
    int waited = 0;
    int r = call_and_wait((void*)obj, ufunc, params, 15000, &waited);
    free(params);
    return r;
}

/* 单bool参数UFUNCTION调用 (按属性链找bool参数偏移) */
static int call_func_bool(uintptr_t obj, const char* name, int b) {
    uintptr_t cls = 0;
    if (safe_read_ptr(obj + 0x10, &cls) != 0 || cls == 0) return -1;
    void* ufunc = find_ufunction_in_class(cls, name);
    if (!ufunc) { palhook_log("activate: %s not found", name); return -2; }
    uintptr_t pr = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &pr);
    int psz = (int)(pr & 0xFFFFFFFF);
    if (psz <= 0 || psz > 64) psz = 8;
    uint8_t* params = (uint8_t*)calloc(1, psz);
    uintptr_t head = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_CHILDPROPS, &head);
    uintptr_t prop = head;
    int count = 0;
    while (prop && prop > 0x10000 && count < 8) {
        count++;
        int sz = prop_get_size(prop);
        int off = prop_get_offset(prop);
        if (sz == 1 && off >= 0 && off < psz) { params[off] = (uint8_t)(b ? 1 : 0); break; }
        uintptr_t next = 0;
        if (safe_read_ptr(prop + FPROP_OFFSET_NEXT, &next) != 0) break;
        if (next == prop) break;
        prop = next;
    }
    int waited = 0;
    int r = call_and_wait((void*)obj, ufunc, params, 15000, &waited);
    free(params);
    return r;
}

/* 单对象指针参数UFUNCTION调用 (按属性链找指针参数偏移) */
static int call_func_obj(uintptr_t obj, const char* name, uintptr_t arg) {
    uintptr_t cls = 0;
    if (safe_read_ptr(obj + 0x10, &cls) != 0 || cls == 0) return -1;
    void* ufunc = find_ufunction_in_class(cls, name);
    if (!ufunc) { palhook_log("activate: %s not found", name); return -2; }
    uintptr_t pr = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &pr);
    int psz = (int)(pr & 0xFFFFFFFF);
    if (psz <= 0 || psz > 64) psz = 8;
    uint8_t* params = (uint8_t*)calloc(1, psz);
    uintptr_t head = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_CHILDPROPS, &head);
    uintptr_t prop = head;
    int count = 0;
    while (prop && prop > 0x10000 && count < 8) {
        count++;
        int sz = prop_get_size(prop);
        int off = prop_get_offset(prop);
        if (sz == 8 && off >= 0 && off + 8 <= psz) { *(uintptr_t*)(params + off) = arg; break; }
        uintptr_t next = 0;
        if (safe_read_ptr(prop + FPROP_OFFSET_NEXT, &next) != 0) break;
        if (next == prop) break;
        prop = next;
    }
    int waited = 0;
    int r = call_and_wait((void*)obj, ufunc, params, 15000, &waited);
    free(params);
    return r;
}

/* 帕鲁AI激活 (palai.activate等价: 让野生帕鲁可见且正常行动) */
static void activate_pal(uintptr_t actor) {
    if (!actor) return;
    palhook_log("activate-pal: actor=0x%lx", actor);
    call_func_no_params(actor, "SetUpDelegate");
    call_func_bool(actor, "SetActiveActor", 1);

    /* 控制器AI设置 (无控制器时等500ms重试一次, 同mod行为) */
    for (int retry = 0; retry < 2; retry++) {
        uintptr_t acls = 0;
        safe_read_ptr(actor + 0x10, &acls);
        uintptr_t cprop = find_property_in_struct(acls, "Controller");
        uintptr_t ctrl = 0;
        if (cprop) {
            int off = prop_get_offset(cprop);
            if (off >= 0) safe_read_ptr(actor + off, &ctrl);
        }
        if (ctrl > 0x10000) {
            call_func_bool(ctrl, "SetActiveAI", 1);
            call_func_obj(ctrl, "ReceivePossess", actor);
            call_func_no_params(ctrl, "SetAutoDefaultAIAction");
            call_func_no_params(ctrl, "StartDefaultAIAction");
            call_func_no_params(actor, "OnPostSpawned");
            palhook_log("activate-pal: AI activated, ctrl=0x%lx", ctrl);
            return;
        }
        if (retry == 0) usleep(500000);
    }
    palhook_log("activate-pal: no controller found");
}

/*
 * /spawn-pal — 刷帕鲁 v2 (对照 AdminCommands spawn.lua)
 * POST {"pal_id":"Anubis","level":50,"capture":1,"x":..,"y":..,"z":..}
 * 流程: PalUtility CDO -> GetNPCManager(World) -> NPCManager.NPCAIControllerBaseClass
 *       构造SpawnInfo{ControllerClass,CharacterID,Level,Location,Yaw,Squad}
 *       -> SpawnNPCForServer(SpawnInfo, nil) -> handle
 *       capture=1: TryGetIndividualActor循环 + PalCaptureSuccess
 */
static void api_spawn_pal_v2(int fd, const char* req) {
    if (g_FNamePool == 0 || !g_hook_active) { json_err(fd, 503, "not ready"); return; }

    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));
    install_segv_handler();

    char pal_id[128] = {0};
    json_get_string(body, "pal_id", pal_id, sizeof(pal_id));
    if (!pal_id[0]) { json_err(fd, 400, "pal_id is required (e.g. Anubis, SheepBall)"); return; }
    int level = json_get_int(body, "level");
    if (level <= 0) level = 1;
    if (level > 100) level = 100;
    int capture = json_get_int(body, "capture");
    capture = (capture > 0) ? 1 : 0;
    double x = json_get_double(body, "x", 0.0 / 0.0);
    double y = json_get_double(body, "y", 0.0 / 0.0);
    double z = json_get_double(body, "z", 0.0 / 0.0);
    int has_coords = !isnan(x) && !isnan(y) && !isnan(z);
    char player_name[64] = {0};
    json_get_string(body, "name", player_name, sizeof(player_name));

    /* 1. 玩家Character + 坐标 (指定玩家则刷在该玩家身边) */
    uintptr_t character = 0;
    if (player_name[0]) {
        if (g_players_ts == 0 || time(NULL) - g_players_ts > 20) {
            collect_all_players();
        }
        fill_player_details();
        for (int i = 0; i < g_players_count && !character; i++) {
            if (strcmp(g_players[i].name, player_name) == 0) {
                character = g_players[i].character;
            }
        }
        if (!character) {
            char e[128];
            snprintf(e, sizeof(e), "player '%s' not found", player_name);
            json_err(fd, 404, e);
            return;
        }
    } else {
        character = find_player_character();
    }
    if (!character) { json_err(fd, 404, "player character not found"); return; }
    double loc[3] = {0, 0, 0};
    actor_get_location(character, loc);
    if (has_coords) { loc[0] = x; loc[1] = y; loc[2] = z; }
    else { loc[0] += 300.0; loc[2] += 100.0; }

    /* 2. World */
    uintptr_t world = get_object_world(character);
    if (!world && !g_cache_world) {
        g_cache_world = find_object_by_class_name("World");
        world = g_cache_world;
    } else if (!world) {
        world = g_cache_world;
    }

    /* 3. PalUtility CDO (缓存) */
    uintptr_t pal_util = get_pal_utility();
    if (!pal_util) { json_err(fd, 404, "PalUtility object not found"); return; }

    /* 4. NPCManager (缓存) */
    uintptr_t npc = g_cache_npc_manager;
    if (!npc) {
        char err[256] = {0};
        npc = get_npc_manager(pal_util, world, err, sizeof(err));
        if (!npc) { json_err(fd, 404, err); return; }
        g_cache_npc_manager = npc;
    }

    uintptr_t npc_cls = 0;
    if (safe_read_ptr(npc + 0x10, &npc_cls) != 0 || npc_cls == 0) {
        json_err(fd, 500, "cannot read NPCManager class"); return;
    }

    /* 5. NPCAIControllerBaseClass属性 (缓存) */
    uintptr_t aic_class = g_cache_aic_class;
    if (!aic_class) {
        uintptr_t aic_prop = find_property_in_struct(npc_cls, "NPCAIControllerBaseClass");
        if (aic_prop) {
            int off = prop_get_offset(aic_prop);
            if (off >= 0) safe_read_ptr(npc + off, &aic_class);
        }
    }
    if (!aic_class) {
        palhook_log("NPCAIControllerBaseClass NOT found, dumping NPCManager props:");
        uintptr_t dhead = 0;
        safe_read_ptr(npc_cls + UFUNC_OFFSET_CHILDPROPS, &dhead);
        uintptr_t dprop = dhead;
        int dcount = 0;
        while (dprop && dprop > 0x10000 && dcount < 200) {
            dcount++;
            char pname[128] = {0};
            prop_get_name(dprop, pname, sizeof(pname));
            palhook_log("  prop '%s' size=%d off=0x%x", pname, prop_get_size(dprop), prop_get_offset(dprop));
            uintptr_t dnext = 0;
            if (safe_read_ptr(dprop + FPROP_OFFSET_NEXT, &dnext) != 0) break;
            if (dnext == dprop) break;
            dprop = dnext;
        }
        json_err(fd, 404, "NPCAIControllerBaseClass property not found");
        return;
    }
    if (!g_cache_aic_class) g_cache_aic_class = aic_class;

    /* 6. SpawnNPCForServer函数 */
    void* spawn_ufunc = find_ufunction_in_class(npc_cls, "SpawnNPCForServer");
    if (!spawn_ufunc) { json_err(fd, 404, "SpawnNPCForServer not found"); return; }
    uintptr_t pr = 0;
    safe_read_ptr((uintptr_t)spawn_ufunc + UFUNC_OFFSET_PARMSSIZE, &pr);
    int parms_size = (int)(pr & 0xFFFFFFFF);
    if (parms_size <= 0 || parms_size > 1024) { json_err(fd, 500, "bad ParmsSize"); return; }

    uint8_t* params = (uint8_t*)calloc(1, parms_size);

    /* 帕鲁CharacterID的FName索引: 池里有直接用, 没有就Conv_StringToName动态intern */
    int pal_fname = find_fname_index(pal_id);
    if (pal_fname <= 0) {
        int dyn_idx = -1;
        if (string_to_fname(pal_id, &dyn_idx) == 0 && dyn_idx > 0) {
            pal_fname = dyn_idx;
            if (g_fname_cache_count < FNAME_CACHE_SIZE) {
                strncpy(g_fname_cache[g_fname_cache_count].name, pal_id, 127);
                g_fname_cache[g_fname_cache_count].idx = pal_fname;
                g_fname_cache_count++;
            }
        }
    }
    if (pal_fname <= 0) {
        char e2[256];
        snprintf(e2, sizeof(e2), "pal '%s' not found in FNamePool and intern failed", pal_id);
        free(params); json_err(fd, 404, e2); return;
    }

    /* 7. 遍历SpawnNPCForServer属性: 填SpawnInfo结构, 记录ReturnValue */
    int rv_off = -1;
    uintptr_t head = 0;
    safe_read_ptr((uintptr_t)spawn_ufunc + UFUNC_OFFSET_CHILDPROPS, &head);
    uintptr_t prop = head;
    int count = 0;
    while (prop && prop > 0x10000 && count < 16) {
        count++;
        char pname[128] = {0};
        prop_get_name(prop, pname, sizeof(pname));
        int sz = prop_get_size(prop);
        int off = prop_get_offset(prop);
        palhook_log("  SpawnNPCForServer param '%s' size=%d offset=0x%x", pname, sz, off);

        if (off >= 0 && off + sz <= parms_size) {
            if (strcmp(pname, "ReturnValue") == 0 && sz == 8) {
                rv_off = off;
            } else if (strstr(pname, "SpawnInfo") && sz > 8) {
                /* 结构体参数(按值): 找UScriptStruct并填充子属性 */
                uintptr_t struct_def = find_struct_ptr(prop, "SpawnInfo");
                if (struct_def) {
                    uintptr_t shead = 0;
                    safe_read_ptr(struct_def + UFUNC_OFFSET_CHILDPROPS, &shead);
                    uintptr_t sprop = shead;
                    int scount = 0;
                    while (sprop && sprop > 0x10000 && scount < 64) {
                        scount++;
                        char sname[128] = {0};
                        prop_get_name(sprop, sname, sizeof(sname));
                        int ssz = prop_get_size(sprop);
                        int soff = prop_get_offset(sprop);
                        palhook_log("    SpawnInfo.%s size=%d offset=0x%x", sname, ssz, soff);
                        if (soff >= 0 && soff + ssz <= sz) {
                            uint8_t* dst = params + off + soff;
                            if (strcmp(sname, "ControllerClass") == 0 && ssz == 8) {
                                *(uintptr_t*)dst = aic_class;
                            } else if (strcmp(sname, "CharacterID") == 0 && ssz == 8) {
                                *(uint32_t*)dst = (uint32_t)pal_fname;
                            } else if (strcmp(sname, "Level") == 0 && ssz == 4) {
                                *(int32_t*)dst = level;
                            } else if (strcmp(sname, "Location") == 0 && ssz == 24) {
                                *(double*)(dst + 0) = loc[0];
                                *(double*)(dst + 8) = loc[1];
                                *(double*)(dst + 16) = loc[2];
                            } else if (strcmp(sname, "Yaw") == 0 && ssz == 8) {
                                *(double*)dst = 0.0;
                            }
                            /* Squad等指针保持NULL */
                        }
                        uintptr_t snext = 0;
                        if (safe_read_ptr(sprop + FPROP_OFFSET_NEXT, &snext) != 0) break;
                        if (snext == sprop) break;
                        sprop = snext;
                    }
                } else {
                    palhook_log("    WARNING: SpawnInfo struct not found");
                }
            }
        }
        uintptr_t next = 0;
        if (safe_read_ptr(prop + FPROP_OFFSET_NEXT, &next) != 0) break;
        if (next == prop) break;
        prop = next;
    }

    palhook_log("spawn-pal-v2: pal='%s' level=%d capture=%d aic=0x%lx npc=0x%lx loc=(%.1f,%.1f,%.1f)",
        pal_id, level, capture, aic_class, npc, loc[0], loc[1], loc[2]);

    int waited = 0;
    int r = call_and_wait((void*)npc, spawn_ufunc, params, 15000, &waited);

    uintptr_t handle = 0;
    if (r == 0 && rv_off >= 0 && rv_off + 8 <= parms_size) {
        handle = *(uintptr_t*)(params + rv_off);
    }
    palhook_log("SpawnNPCForServer: r=%d handle=0x%lx waited=%d", r, handle, waited);

    /* 8. 可选: 自动捕获 */
    int captured = 0;
    if (handle && capture) {
        uintptr_t hcls = 0;
        if (safe_read_ptr(handle + 0x10, &hcls) == 0 && hcls) {
            void* tia = find_ufunction_in_class(hcls, "TryGetIndividualActor");
            if (tia) {
                uintptr_t tpr = 0;
                safe_read_ptr((uintptr_t)tia + UFUNC_OFFSET_PARMSSIZE, &tpr);
                int tps = (int)(tpr & 0xFFFFFFFF);
                if (tps <= 0 || tps > 64) tps = 8;
                uintptr_t actor = 0;
                for (int attempt = 0; attempt < 25 && !actor; attempt++) {
                    usleep(200000);
                    uint8_t* tp = (uint8_t*)calloc(1, tps);
                    int tr = call_and_wait((void*)handle, tia, tp, 15000, &waited);
                    if (tr == 0) {
                        uintptr_t rvp = find_property_in_struct((uintptr_t)tia, "ReturnValue");
                        int toff = rvp ? prop_get_offset(rvp) : 0;
                        if (toff < 0) toff = 0;
                        actor = *(uintptr_t*)(tp + toff);
                    }
                    free(tp);
                }
                if (actor) {
                    uintptr_t pcls = 0;
                    safe_read_ptr(pal_util + 0x10, &pcls);
                    void* pcs = find_ufunction_in_class(pcls, "PalCaptureSuccess");
                    if (pcs) {
                        uintptr_t ppr = 0;
                        safe_read_ptr((uintptr_t)pcs + UFUNC_OFFSET_PARMSSIZE, &ppr);
                        int pps = (int)(ppr & 0xFFFFFFFF);
                        if (pps <= 0 || pps > 64) pps = 16;
                        uint8_t* pp = (uint8_t*)calloc(1, pps);
                        int ptr_idx = 0;
                        uintptr_t phead = 0;
                        safe_read_ptr((uintptr_t)pcs + UFUNC_OFFSET_CHILDPROPS, &phead);
                        uintptr_t pprop = phead;
                        int pcnt = 0;
                        while (pprop && pprop > 0x10000 && pcnt < 8) {
                            pcnt++;
                            char pn[64] = {0};
                            prop_get_name(pprop, pn, sizeof(pn));
                            int psz = prop_get_size(pprop);
                            int poff = prop_get_offset(pprop);
                            if (poff >= 0 && psz == 8 && poff + 8 <= pps) {
                                if (ptr_idx == 0) *(uintptr_t*)(pp + poff) = character;
                                else if (ptr_idx == 1) *(uintptr_t*)(pp + poff) = actor;
                                ptr_idx++;
                            }
                            uintptr_t pnext = 0;
                            if (safe_read_ptr(pprop + FPROP_OFFSET_NEXT, &pnext) != 0) break;
                            if (pnext == pprop) break;
                            pprop = pnext;
                        }
                        int cr = call_and_wait((void*)pal_util, pcs, pp, 15000, &waited);
                        captured = (cr == 0);
                        palhook_log("PalCaptureSuccess: r=%d actor=0x%lx", cr, actor);
                        free(pp);
                    }
                }
            }
        }
    }

    /* 9. 野生帕鲁AI激活 (capture=0时): 否则帕鲁不显示/不行动 */
    if (handle && !capture) {
        uintptr_t hcls = 0;
        if (safe_read_ptr(handle + 0x10, &hcls) == 0 && hcls) {
            void* tia = find_ufunction_in_class(hcls, "TryGetIndividualActor");
            if (tia) {
                uintptr_t tpr = 0;
                safe_read_ptr((uintptr_t)tia + UFUNC_OFFSET_PARMSSIZE, &tpr);
                int tps = (int)(tpr & 0xFFFFFFFF);
                if (tps <= 0 || tps > 64) tps = 8;
                uintptr_t actor = 0;
                for (int attempt = 0; attempt < 25 && !actor; attempt++) {
                    usleep(200000);
                    uint8_t* tp = (uint8_t*)calloc(1, tps);
                    int tr = call_and_wait((void*)handle, tia, tp, 15000, &waited);
                    if (tr == 0) {
                        uintptr_t rvp = find_property_in_struct((uintptr_t)tia, "ReturnValue");
                        int toff = rvp ? prop_get_offset(rvp) : 0;
                        if (toff < 0) toff = 0;
                        actor = *(uintptr_t*)(tp + toff);
                    }
                    free(tp);
                }
                if (actor) activate_pal(actor);
            }
        }
    }

    free(params);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"pal_id\":\"%s\",\"level\":%d,\"capture\":%d,\"captured\":%d,"
        "\"npc_manager\":\"0x%lx\",\"handle\":\"0x%lx\",\"loc\":[%.1f,%.1f,%.1f],\"waited_ms\":%d}",
        r == 0 ? "success" : "failed", pal_id, level, capture, captured,
        npc, handle, loc[0], loc[1], loc[2], waited);
    json_ok(fd, buf);
}

/* GET /get-npc-manager — 调试: 获取NPCManager和AIControllerBaseClass */
static void api_get_npc_manager(int fd, const char* req) {
    if (g_FNamePool == 0 || !g_hook_active) { json_err(fd, 503, "not ready"); return; }
    (void)req;
    install_segv_handler();

    uintptr_t character = find_player_character();
    uintptr_t world = character ? get_object_world(character) : 0;
    if (!world) {
        if (!g_cache_world) g_cache_world = find_object_by_class_name("World");
        world = g_cache_world;
    }
    uintptr_t pal_util = get_pal_utility();
    if (!pal_util) { json_err(fd, 404, "PalUtility object not found"); return; }

    char err[256] = {0};
    uintptr_t npc = g_cache_npc_manager;
    if (!npc) {
        npc = get_npc_manager(pal_util, world, err, sizeof(err));
        if (npc) g_cache_npc_manager = npc;
    }

    uintptr_t aic_class = g_cache_aic_class;
    if (npc && !aic_class) {
        uintptr_t npc_cls = 0;
        safe_read_ptr(npc + 0x10, &npc_cls);
        uintptr_t aic_prop = find_property_in_struct(npc_cls, "NPCAIControllerBaseClass");
        if (aic_prop) {
            int off = prop_get_offset(aic_prop);
            if (off >= 0) safe_read_ptr(npc + off, &aic_class);
        }
        if (aic_class) g_cache_aic_class = aic_class;
    }
    if (npc) {
        char cls_name[128] = {0};
        uintptr_t npc_cls = 0;
        safe_read_ptr(npc + 0x10, &npc_cls);
        uintptr_t fn = 0;
        safe_read_ptr(npc_cls + 0x18, &fn);
        resolve_fname((int)(fn & 0xFFFFFFFF), cls_name, sizeof(cls_name));
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"status\":\"%s\",\"character\":\"0x%lx\",\"world\":\"0x%lx\",\"pal_utility\":\"0x%lx\","
            "\"npc_manager\":\"0x%lx\",\"npc_class\":\"%s\",\"aic_class\":\"0x%lx\",\"error\":\"%s\"}",
            npc ? "success" : "failed", character, world, pal_util, npc, cls_name, aic_class, err);
        json_ok(fd, buf);
    } else {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"status\":\"failed\",\"character\":\"0x%lx\",\"world\":\"0x%lx\",\"pal_utility\":\"0x%lx\","
            "\"npc_manager\":\"0x0\",\"npc_class\":\"\",\"aic_class\":\"0x0\",\"error\":\"%s\"}",
            character, world, pal_util, err);
        json_ok(fd, buf);
    }
}

/* GET /find-prop — 调试: 沿类链查找属性并dump (obj,name) */
static void api_find_prop(int fd, const char* req) {
    (void)req;
    install_segv_handler();
    uintptr_t obj = 0;
    char name[128] = {0};
    const char* o = strstr(req, "obj=");
    if (o) obj = strtoull(o + 4, NULL, 0);
    const char* n = strstr(req, "name=");
    if (n) {
        const char* s = n + 5;
        int i = 0;
        while (s[i] && s[i] != ' ' && s[i] != '&' && i < 127) { name[i] = s[i]; i++; }
        name[i] = 0;
    }

    uintptr_t found_prop = 0;
    int found_off = -1;
    if (obj > 0x10000) {
        uintptr_t cls = 0;
        if (safe_read_ptr(obj + 0x10, &cls) == 0) {
            uintptr_t cur = cls;
            int total_logged = 0;
            for (int depth = 0; depth < 12 && cur > 0x10000 && !found_prop; depth++) {
                uintptr_t fn = 0;
                safe_read_ptr(cur + 0x18, &fn);
                char cls_name[128] = {0};
                resolve_fname((int)(fn & 0xFFFFFFFF), cls_name, sizeof(cls_name));
                uintptr_t head = 0;
                safe_read_ptr(cur + UFUNC_OFFSET_CHILDPROPS, &head);
                int cnt = 0;
                uintptr_t prop = head;
                while (prop && prop > 0x10000 && cnt < 600) {
                    cnt++;
                    char pname[128] = {0};
                    prop_get_name(prop, pname, sizeof(pname));
                    if (name[0] && strcmp(pname, name) == 0 && !found_prop) {
                        found_prop = prop;
                        found_off = prop_get_offset(prop);
                    }
                    if (total_logged < 400) {
                        total_logged++;
                        if (!name[0] || strstr(pname, "Root") || strstr(pname, "Relative") || strstr(pname, "Location") || strcmp(pname, name) == 0) {
                            palhook_log("    [%s] '%s' size=%d off=0x%x prop=0x%lx",
                                cls_name, pname, prop_get_size(prop), prop_get_offset(prop), prop);
                        }
                    }
                    uintptr_t pnext = 0;
                    if (safe_read_ptr(prop + FPROP_OFFSET_NEXT, &pnext) != 0) break;
                    if (pnext == prop) break;
                    prop = pnext;
                }
                palhook_log("  class[%d] '%s' own_props=%d head=0x%lx", depth, cls_name, cnt, head);
                uintptr_t super = 0;
                if (safe_read_ptr(cur + UCLASS_OFFSET_SUPER, &super) != 0) break;
                if (super < 0x10000 || super == cur) break;
                cur = super;
            }
        }
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"obj\":\"0x%lx\",\"name\":\"%s\",\"prop\":\"0x%lx\",\"offset\":%d}",
        found_prop ? "found" : "not_found", obj, name, found_prop, found_off);
    json_ok(fd, buf);
}

/* GET/POST /writemem — 直接写内存 (调试+等级修改用)
 * POST {"addr":"0x...","qwords":[1,2,...]} 或 {"addr":"0x...","bytes_hex":"..."} */
static void api_writemem(int fd, const char* req) {
    if (g_FNamePool == 0 || !g_hook_active) { json_err(fd, 503, "not ready"); return; }
    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));
    install_segv_handler();

    char addr_str[64] = {0};
    json_get_string(body, "addr", addr_str, sizeof(addr_str));
    if (!addr_str[0]) { json_err(fd, 400, "addr required"); return; }
    uintptr_t addr = strtoull(addr_str, NULL, 0);
    if (addr < 0x10000) { json_err(fd, 400, "invalid addr"); return; }

    /* qwords数组: {"addr":"0x..","qwords":[...]} */
    const char* qp = strstr(body, "\"qwords\"");
    if (qp) {
        qp += 8;
        while (*qp && (*qp == ' ' || *qp == ':' || *qp == '[')) qp++;
        int count = 0;
        uintptr_t cur = addr;
        while (*qp && *qp != ']' && count < 64) {
            char* end = NULL;
            uint64_t v = strtoull(qp, &end, 0);
            if (end == qp) break;
            if (region_contains(cur) && region_contains(cur + 7)) {
                *(volatile uintptr_t*)cur = (uintptr_t)v;
                count++;
            } else break;
            cur += 8;
            qp = end;
            while (*qp == ',' || *qp == ' ') qp++;
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"wrote_qwords\":%d}", count);
        json_ok(fd, buf);
        return;
    }

    /* 单值: {"addr":"0x..","value":"0x.."} 或 {"addr":"0x..","value":123} */
    char val_str[64] = {0};
    if (json_get_string(body, "value", val_str, sizeof(val_str)) == 0 && val_str[0]) {
        *(volatile uintptr_t*)addr = strtoull(val_str, NULL, 0);
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"addr\":\"%s\",\"value\":\"%s\"}", addr_str, val_str);
        json_ok(fd, buf);
        return;
    }
    const char* vp = strstr(body, "\"value\":");
    if (vp) {
        vp += 8;
        *(volatile uintptr_t*)addr = strtoull(vp, NULL, 0);
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"addr\":\"%s\"}", addr_str);
        json_ok(fd, buf);
        return;
    }
    json_err(fd, 400, "qwords or value required");
}

/*
 * /set-exp / /set-level — 直接写玩家经验值/等级
 * POST {"exp":500000} 或 {"level":30} (level同时写经验曲线近似值)
 * Exp int64 @ IndividualParameter+0x3F8 (实测: 给1经验精确+1)
 * Level byte @ IndividualParameter+0x3F0 (实测: 低字节=等级, 与REST API一致)
 */
static void api_set_exp(int fd, const char* req) {
    if (g_FNamePool == 0 || !g_hook_active) { json_err(fd, 503, "not ready"); return; }
    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));
    install_segv_handler();

    char pname[64] = {0};
    json_get_string(body, "name", pname, sizeof(pname));
    uintptr_t character = 0;
    if (pname[0]) {
        if (g_players_ts == 0 || time(NULL) - g_players_ts > 20) collect_all_players();
        fill_player_details();
        for (int i = 0; i < g_players_count && !character; i++) {
            if (strcmp(g_players[i].name, pname) == 0) character = g_players[i].character;
        }
        if (!character) {
            char e[128];
            snprintf(e, sizeof(e), "player '%s' not found", pname);
            json_err(fd, 404, e);
            return;
        }
    } else {
        character = find_player_character();
        if (!character) { json_err(fd, 404, "player character not found"); return; }
    }

    uintptr_t cls = 0;
    safe_read_ptr(character + 0x10, &cls);
    uintptr_t cpc_prop = find_property_in_struct(cls, "CharacterParameterComponent");
    if (!cpc_prop) { json_err(fd, 404, "CharacterParameterComponent property not found"); return; }
    int cpc_off = prop_get_offset(cpc_prop);
    uintptr_t comp = 0;
    if (cpc_off < 0 || safe_read_ptr(character + cpc_off, &comp) != 0 || comp < 0x10000) {
        json_err(fd, 500, "cannot read CharacterParameterComponent"); return;
    }

    uintptr_t ccls = 0;
    safe_read_ptr(comp + 0x10, &ccls);
    uintptr_t ip_prop = find_property_in_struct(ccls, "IndividualParameter");
    if (!ip_prop) { json_err(fd, 404, "IndividualParameter property not found"); return; }
    int ip_off = prop_get_offset(ip_prop);
    uintptr_t ip = 0;
    if (ip_off < 0 || safe_read_ptr(comp + ip_off, &ip) != 0 || ip < 0x10000) {
        json_err(fd, 500, "cannot read IndividualParameter"); return;
    }

    uintptr_t old_exp = 0;
    safe_read_ptr(ip + 0x3F8, &old_exp);
    uintptr_t old_level_raw = 0;
    safe_read_ptr(ip + 0x3F0, &old_level_raw);
    int old_level = (int)(old_level_raw & 0xFF);

    int64_t new_exp = (int64_t)old_exp;
    int set_exp = (strstr(body, "\"exp\"") != NULL);
    int set_level = (strstr(body, "\"level\"") != NULL);

    int level = 0;
    if (set_level) {
        level = json_get_int(body, "level");
        if (level < 1) level = 1;
        if (level > 50) level = 50;
        /* 等级字节在 +0x3F0 (实测: 低字节=等级, 高位是标志位, 只改低字节) */
        if (region_contains(ip + 0x3F0)) {
            volatile uint8_t* lv = (volatile uint8_t*)(ip + 0x3F0);
            *lv = (uint8_t)level;
        }
    }

    if (set_exp) {
        new_exp = (int64_t)json_get_int(body, "exp");
    } else if (set_level) {
        /* 经验曲线近似: T(L) ≈ 0.08 * L^4.39 (拟合实测点 47→1.83M, 48→2.14M, 35→0.5M)
         * 刻意略低, 因为等级只升不降, 低估经验保证后续AddExp不会跳级 */
        double lv_d = (double)level;
        new_exp = (int64_t)(0.08 * pow(lv_d, 4.39));
        if (new_exp < 0) new_exp = 0;
    }
    if (new_exp < 0) new_exp = 0;

    if (region_contains(ip + 0x3F8) && region_contains(ip + 0x3FF)) {
        *(volatile int64_t*)(ip + 0x3F8) = new_exp;
    }

    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"success\",\"ip\":\"0x%lx\",\"old_level\":%d,\"new_level\":%d,"
        "\"old_exp\":%lld,\"new_exp\":%lld,\"set_level\":%d}",
        ip, old_level, set_level ? level : old_level,
        (long long)old_exp, (long long)new_exp, set_level);
    json_ok(fd, buf);
}

/* ========== v0.9.0: 面板数据接口 (players/announce/kick/metrics) ========== */

static int utf8_to_utf16(const char* s, uint16_t* out, int max_out);
static int utf16_to_utf8(const uint16_t* in, int in_len, char* out, int out_size);

/* 读UE FString (Data指针+Num+Max, UTF-16) -> UTF-8 */
static int read_fstring(uintptr_t fstr_addr, char* out, int out_size) {
    uintptr_t data = 0;
    if (safe_read_ptr(fstr_addr, &data) != 0 || data < 0x10000) return -1;
    uintptr_t num_raw = 0;
    if (safe_read_ptr(fstr_addr + 8, &num_raw) != 0) return -1;
    int num = (int)(num_raw & 0xFFFFFFFF);
    if (num <= 0 || num > 4096) return -1;
    if (!region_contains(data) || !region_contains(data + (uintptr_t)num * 2)) return -1;
    /* 直接读UTF-16并转UTF-8 (数据已在区域内) */
    return utf16_to_utf8((const uint16_t*)data, num, out, out_size);
}

/* UTF-8字符串转UTF-16LE (UE FString内部编码), 返回字符数 */
static int utf8_to_utf16(const char* s, uint16_t* out, int max_out) {
    int n = 0;
    const uint8_t* p = (const uint8_t*)s;
    while (*p && n < max_out) {
        uint32_t cp;
        if (*p < 0x80) { cp = *p++; }
        else if ((*p & 0xE0) == 0xC0) { cp = ((uint32_t)(*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = ((uint32_t)(*p & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = ((uint32_t)(*p & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4; }
        else { p++; continue; }
        if (cp < 0x10000) {
            out[n++] = (uint16_t)cp;
        } else if (n + 1 < max_out) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 | (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        }
    }
    return n;
}

/* UTF-16LE转UTF-8 */
static int utf16_to_utf8(const uint16_t* in, int in_len, char* out, int out_size) {
    int o = 0;
    for (int i = 0; i < in_len && o < out_size - 4; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < in_len && in[i+1] >= 0xDC00 && in[i+1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[++i] - 0xDC00);
        }
        if (cp < 0x80) { out[o++] = (char)cp; }
        else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
    return o;
}

/* 构造FString参数 (UTF-16, 支持中文): 写入params+off, 需要外部管理str_buf生命周期 */
static void build_fstring_at(uint8_t* params, int off, const char* s, uint8_t* str_buf) {
    int n = utf8_to_utf16(s, (uint16_t*)str_buf, 2048);
    ((uint16_t*)str_buf)[n] = 0;
    *(uintptr_t*)(params + off + 0) = (uintptr_t)str_buf;
    *(int32_t*)(params + off + 8) = n + 1;
    *(int32_t*)(params + off + 12) = n + 1;
}

/* 玩家列表缓存 g_players/g_players_count/g_players_ts 已在前置声明区定义 (约第995行) */

/* 从角色拿到PlayerState (APawn::PlayerState属性, off=688实测) */
static uintptr_t get_pawn_playerstate(uintptr_t character) {
    uintptr_t ps = 0;
    if (safe_read_ptr(character + 688, &ps) != 0 || ps < 0x10000) return 0;
    uintptr_t ov = 0;
    if (safe_read_ptr(ps, &ov) != 0 || !is_plausible_vtable(ov)) return 0;
    return ps;
}

/* 从角色拿到IndividualParameter (CharacterParameterComponent@1584 -> IndividualParameter@376) */
static uintptr_t get_player_individual(uintptr_t character) {
    uintptr_t comp = 0;
    if (safe_read_ptr(character + 1584, &comp) != 0 || comp < 0x10000) return 0;
    uintptr_t ip = 0;
    if (safe_read_ptr(comp + 376, &ip) != 0 || ip < 0x10000) return 0;
    return ip;
}

/* 静默版Outer链校验: 对象是否属于某个World (真Actor才有Level->World链) */
static int obj_belongs_to_world(uintptr_t obj) {
    uintptr_t cur = obj;
    for (int i = 0; i < 8 && cur > 0x10000; i++) {
        uintptr_t cls = 0;
        if (safe_read_ptr(cur + 0x10, &cls) != 0) return 0;
        uintptr_t fn = 0;
        if (safe_read_ptr(cls + 0x18, &fn) != 0) return 0;
        char name[64] = {0};
        resolve_fname((int)(fn & 0xFFFFFFFF), name, sizeof(name));
        if (strcmp(name, "World") == 0) return 1;
        uintptr_t outer = 0;
        if (safe_read_ptr(cur + 0x20, &outer) != 0) return 0;
        cur = outer;
    }
    return 0;
}

/* 全内存扫描收集所有在线玩家角色 */
/* GameState缓存 */
static uintptr_t g_cache_gamestate = 0;

/* 读 GameState->PlayerArray (TArray<APlayerState*>), 只含已连接玩家的权威列表 */
static int get_connected_playerstates(uintptr_t* out, int max_n) {
    if (!g_cache_gamestate) g_cache_gamestate = find_gamestate();
    if (!g_cache_gamestate) return 0;
    uintptr_t gscls = 0;
    if (safe_read_ptr(g_cache_gamestate + 0x10, &gscls) != 0 || gscls < 0x10000) return 0;
    uintptr_t prop = find_property_in_struct(gscls, "PlayerArray");
    if (!prop) return 0;
    int off = prop_get_offset(prop);
    if (off < 0) return 0;
    uintptr_t dptr = 0, num_raw = 0;
    if (safe_read_ptr(g_cache_gamestate + off, &dptr) != 0) return 0;
    if (safe_read_ptr(g_cache_gamestate + off + 8, &num_raw) != 0) return 0;
    int num = (int)(num_raw & 0xFFFFFFFF);
    if (num <= 0 || num > 64 || dptr < 0x10000 || !region_contains(dptr)) return 0;
    if (!out) return num; /* 只计数 */
    if (num > max_n) num = max_n;
    for (int i = 0; i < num; i++) {
        uintptr_t p = 0;
        if (safe_read_ptr(dptr + (uintptr_t)i * 8, &p) != 0) return i;
        out[i] = p;
    }
    return num;
}

static pthread_mutex_t g_collect_lock = PTHREAD_MUTEX_INITIALIZER;

/* 零扫描路径: GameState->PlayerArray -> PlayerState -> PawnPrivate 拿角色 (毫秒级)
 * 返回-1表示PlayerArray不可用, 调用者走全扫描兜底 */
static int collect_all_players_fast(void) {
    uintptr_t conn[MAX_PLAYER_CACHE];
    int np = get_connected_playerstates(conn, MAX_PLAYER_CACHE);
    if (np <= 0) return -1;
    g_players_count = 0;
    for (int i = 0; i < np && g_players_count < MAX_PLAYER_CACHE; i++) {
        uintptr_t ps = conn[i];
        PlayerEntry* e = &g_players[g_players_count];
        memset(e, 0, sizeof(*e));
        e->playerstate = ps;
        uintptr_t pcls = 0;
        if (safe_read_ptr(ps + 0x10, &pcls) != 0 || pcls < 0x10000) continue;
        /* PawnPrivate -> Character (属性直读, 无扫描) */
        uintptr_t pp = find_property_in_struct(pcls, "PawnPrivate");
        if (pp) {
            int poff = prop_get_offset(pp);
            uintptr_t ch = 0;
            if (poff >= 0 && safe_read_ptr(ps + poff, &ch) == 0 && ch > 0x10000) {
                uintptr_t ov = 0;
                if (safe_read_ptr(ch, &ov) == 0 && is_plausible_vtable(ov)) {
                    e->character = ch;
                    /* Controller (角色属性直读) */
                    uintptr_t ccls = 0;
                    if (safe_read_ptr(ch + 0x10, &ccls) == 0 && ccls > 0x10000) {
                        uintptr_t cprop = find_property_in_struct(ccls, "Controller");
                        if (cprop) {
                            int coff = prop_get_offset(cprop);
                            uintptr_t ctrl = 0;
                            if (coff >= 0 && safe_read_ptr(ch + coff, &ctrl) == 0 && ctrl > 0x10000) {
                                uintptr_t cvt = 0;
                                if (safe_read_ptr(ctrl, &cvt) == 0 && is_plausible_vtable(cvt)) {
                                    e->controller = ctrl;
                                }
                            }
                        }
                    }
                }
            }
        }
        g_players_count++;
    }
    g_players_ts = time(NULL);
    palhook_log("collect players (fast): %d", g_players_count);
    return g_players_count;
}

/* 全内存扫描兜底 (仅当PlayerArray不可用时; 有线程在扫时其他请求复用旧缓存) */
static int collect_all_players_scan(void) {
    install_segv_handler();
    g_players_count = 0;
    const char* classes[] = {"BP_Player_Female_C", "BP_Player_Male_C", NULL};
    uintptr_t segs[MAX_RW_REGIONS][2];
    int seg_count = snapshot_rw(segs, MAX_RW_REGIONS);
    for (int c = 0; classes[c]; c++) {
        int fi = find_fname_index(classes[c]);
        if (fi <= 0) continue;
        for (int seg = 0; seg < seg_count && g_players_count < MAX_PLAYER_CACHE; seg++) {
            uintptr_t* ptr = (uintptr_t*)segs[seg][0];
            size_t cnt = (segs[seg][1] - segs[seg][0]) / sizeof(uintptr_t);
            for (size_t i = 0; i < cnt && g_players_count < MAX_PLAYER_CACHE; i++) {
                uintptr_t val = ptr[i];
                if (val < 0x10000 || val > 0x800000000000UL) continue;
                uintptr_t ov = 0;
                if (safe_read_ptr(val, &ov) != 0 || !is_plausible_vtable(ov)) continue;
                uintptr_t uc = 0;
                if (safe_read_ptr(val + 0x10, &uc) != 0) continue;
                uintptr_t ucv = 0;
                if (safe_read_ptr(uc, &ucv) != 0 || !is_uclass_vtable(ucv)) continue;
                uintptr_t fn = 0;
                if (safe_read_ptr(uc + 0x18, &fn) != 0) continue;
                if ((int)(fn & 0xFFFFFFFF) != fi) continue;
                uintptr_t on = 0;
                safe_read_ptr(val + 0x18, &on);
                char nm[64] = {0};
                resolve_fname((int)(on & 0xFFFFFFFF), nm, sizeof(nm));
                if (strncmp(nm, "Default__", 9) == 0) continue;
                /* 关键过滤: 真实世界内的Actor才有Outer->Level->World链
                 * 蓝图模板/CDO等假对象没有, 对它们调ProcessEvent会崩游戏 */
                if (!obj_belongs_to_world(val)) continue;
                /* 关键过滤2: GameState->PlayerArray权威过滤 (只含已连接玩家)
                 * 玩家下线后角色/控制器可能残留在内存直到GC, 用引擎自己的列表最可靠 */
                {
                    uintptr_t myps = get_pawn_playerstate(val);
                    uintptr_t conn[MAX_PLAYER_CACHE];
                    int nconn = get_connected_playerstates(conn, MAX_PLAYER_CACHE);
                    if (nconn > 0) {
                        int is_conn = 0;
                        for (int k = 0; k < nconn; k++) {
                            if (conn[k] == myps) { is_conn = 1; break; }
                        }
                        if (!is_conn) continue;
                    }
                }

                /* 关键过滤3: 必须有活的PlayerController才是在线玩家
                 * (玩家下线后角色残留在内存里直到GC, 但Controller会被解除/销毁,
                 *  否则空服也会显示有玩家) */
                uintptr_t ctrl = 0;
                {
                    uintptr_t cprop = find_property_in_struct(uc, "Controller");
                    if (cprop) {
                        int coff = prop_get_offset(cprop);
                        if (coff < 0 ||
                            safe_read_ptr(val + coff, &ctrl) != 0 || ctrl < 0x10000) ctrl = 0;
                    }
                    if (ctrl < 0x10000) continue;
                    {
                        uintptr_t cvt = 0;
                        if (safe_read_ptr(ctrl, &cvt) != 0 || !is_plausible_vtable(cvt)) continue;
                        uintptr_t ccls = 0;
                        if (safe_read_ptr(ctrl + 0x10, &ccls) != 0 || ccls < 0x10000) continue;
                        uintptr_t ccv = 0;
                        if (safe_read_ptr(ccls, &ccv) != 0 || !is_uclass_vtable(ccv)) continue;
                    }
                }
                /* 注意: 上面的块结束后 ctrl 仍有效, memset之后统一写入数组 */
                /* 去重 */
                int dup = 0;
                for (int d = 0; d < g_players_count; d++) {
                    if (g_players[d].character == val) { dup = 1; break; }
                }
                if (dup) continue;
                memset(&g_players[g_players_count], 0, sizeof(PlayerEntry));
                g_players[g_players_count].character = val;
                g_players[g_players_count].playerstate = get_pawn_playerstate(val);
                g_players[g_players_count].controller = ctrl;
                g_players_count++;
            }
        }
    }
    g_players_ts = time(NULL);
    return g_players_count;
}

/* 入口: TTL内直接返回; 优先零扫描路径, 失败走全扫描; 同一时间只允许一个扫描 */
static int collect_all_players(void) {
    if (g_players_ts != 0 && time(NULL) - g_players_ts < 20) return g_players_count;
    if (pthread_mutex_trylock(&g_collect_lock) != 0) return g_players_count;
    if (g_players_ts != 0 && time(NULL) - g_players_ts < 20) {
        pthread_mutex_unlock(&g_collect_lock);
        return g_players_count;
    }
    int r = collect_all_players_fast();
    if (r < 0) r = collect_all_players_scan();
    pthread_mutex_unlock(&g_collect_lock);
    return r;
}

/* 补充玩家详细信息 (名字/UID/等级/经验/坐标) */
/* 在base的[start, max_off)内尝试匹配UTF-16LE的 d.d.d.d 字符串 */
static int match_ipv4_wide(uint8_t* base, int start, int max_off, char* out, int out_size) {
    int tl = 0, dots = 0;
    for (int k = 0; k < 20 && tl < out_size - 2; k++) {
        int idx = start + k * 2;
        if (idx + 1 >= max_off) return 0;
        uint8_t b0 = base[idx];
        uint8_t b1 = base[idx + 1];
        if (b0 >= '0' && b0 <= '9' && b1 == 0) { out[tl++] = (char)b0; }
        else if (b0 == '.' && b1 == 0 && dots < 3) { out[tl++] = '.'; dots++; }
        else if ((b0 == ':' || b0 == 0) && b1 == 0) break;
        else return 0;
    }
    out[tl] = 0;
    return (dots == 3 && tl >= 7);
}

/* 从自建params缓冲读FString (缓冲是malloc的, 不做region检查) */

/* UPalPlayerAccount缓存: uid -> 平台名 (生产服历史账号多, 上限256) */
#define MAX_ACC_CACHE 256
static char g_acc_uid[MAX_ACC_CACHE][64];
static char g_acc_platform[MAX_ACC_CACHE][32];
static int g_acc_count = 0;
static time_t g_acc_ts = 0;

static pthread_mutex_t g_acc_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_acc_refresh_running = 0;

/* 账号扫描核心 (调用者保证单实例执行) */
static void collect_player_accounts_scan(void) {
    g_acc_count = 0;
    int fi = find_fname_index("PalPlayerAccount");
    if (fi <= 0) { g_acc_ts = time(NULL); return; }
    uintptr_t segs[MAX_RW_REGIONS][2];
    int seg_count = snapshot_rw(segs, MAX_RW_REGIONS);
    for (int seg = 0; seg < seg_count && g_acc_count < MAX_ACC_CACHE; seg++) {
        uintptr_t* ptr = (uintptr_t*)segs[seg][0];
        size_t cnt = (segs[seg][1] - segs[seg][0]) / sizeof(uintptr_t);
        for (size_t i = 0; i < cnt && g_acc_count < MAX_ACC_CACHE; i++) {
            uintptr_t val = ptr[i];
            if (val < 0x10000 || val > 0x800000000000UL) continue;
            uintptr_t ov = 0;
            if (safe_read_ptr(val, &ov) != 0 || !is_plausible_vtable(ov)) continue;
            uintptr_t uc2 = 0;
            if (safe_read_ptr(val + 0x10, &uc2) != 0 || uc2 < 0x10000) continue;
            uintptr_t ucv = 0;
            if (safe_read_ptr(uc2, &ucv) != 0 || !is_uclass_vtable(ucv)) continue;
            uintptr_t fn = 0;
            if (safe_read_ptr(uc2 + 0x18, &fn) != 0) continue;
            if ((int)(fn & 0xFFFFFFFF) != fi) continue;
            uintptr_t up = find_property_in_struct(uc2, "PlayerUId");
            uintptr_t pp = find_property_in_struct(uc2, "PlayerPlatform");
            if (!up || !pp) continue;
            int uoff = prop_get_offset(up);
            int poff = prop_get_offset(pp);
            if (uoff < 0 || poff < 0) continue;
            char uid[64] = "";
            format_guid(val + uoff, uid);
            if (uid[0] == '0' && uid[1] == '0') continue; /* 全零GUID跳过 */
            uintptr_t pv = 0;
            safe_read_ptr(val + poff, &pv);
            int plat = (int)(pv & 0xFF);
            const char* pname = "Unknown";
            if (plat == 1) pname = "Steam";
            else if (plat == 2) pname = "Xbox";
            else if (plat == 3) pname = "Mac";
            else if (plat == 4) pname = "PS5";
            snprintf(g_acc_uid[g_acc_count], 64, "%s", uid);
            snprintf(g_acc_platform[g_acc_count], 32, "%s", pname);
            g_acc_count++;
        }
    }
    g_acc_ts = time(NULL);
    palhook_log("collect_player_accounts: %d accounts", g_acc_count);
}

static void* acc_refresh_thread_fn(void* arg) {
    (void)arg;
    collect_player_accounts_scan();
    g_acc_refresh_running = 0;
    return NULL;
}

/* 触发账号收集: 缓存新鲜直接返回; 空缓存时同步扫一次, 否则后台刷新 (请求不阻塞) */
static void collect_player_accounts(void) {
    if (g_acc_ts != 0 && time(NULL) - g_acc_ts < 300) return;
    if (pthread_mutex_trylock(&g_acc_lock) != 0) return;
    if (g_acc_ts != 0 && time(NULL) - g_acc_ts < 300) {
        pthread_mutex_unlock(&g_acc_lock);
        return;
    }
    if (g_acc_count == 0) {
        collect_player_accounts_scan(); /* 首次同步 (缓存为空时必须拿一次) */
    } else {
        if (!g_acc_refresh_running) {
            g_acc_refresh_running = 1;
            pthread_t t;
            if (pthread_create(&t, NULL, acc_refresh_thread_fn, NULL) == 0) pthread_detach(t);
            else g_acc_refresh_running = 0;
        }
    }
    pthread_mutex_unlock(&g_acc_lock);
}

static void fill_player_details(void) {
    install_segv_handler();
    for (int i = 0; i < g_players_count; i++) {
        PlayerEntry* e = &g_players[i];
        uintptr_t ps = e->playerstate;
        if (ps) {
            uintptr_t pcls = 0;
            safe_read_ptr(ps + 0x10, &pcls);
            uintptr_t name_prop = find_property_in_struct(pcls, "PlayerNamePrivate");
            if (name_prop) {
                int off = prop_get_offset(name_prop);
                if (off >= 0) read_fstring(ps + off, e->name, sizeof(e->name));
            }
            /* Ping: UE5.1的APlayerState用CompressedPing(1字节)存储, 实际毫秒=值*4 */
            {
                uintptr_t ping_prop = find_property_in_struct(pcls, "CompressedPing");
                if (!ping_prop) ping_prop = find_property_in_struct(pcls, "Ping");
                if (ping_prop) {
                    int poff = prop_get_offset(ping_prop);
                    int psz = prop_get_size(ping_prop);
                    if (poff >= 0 && (psz == 1 || psz == 4)) {
                        uintptr_t pv = 0;
                        if (safe_read_ptr(ps + poff, &pv) == 0) {
                            if (psz == 1) e->ping = (float)((pv & 0xFF) * 4); /* CompressedPing*4 */
                            else e->ping = *(float*)&pv;
                        }
                    }
                }
            }
            uintptr_t uid_prop = find_property_in_struct(pcls, "PlayerUId");
            if (uid_prop) {
                int off = prop_get_offset(uid_prop);
                if (off >= 0) {
                    /* FGuid = {int32 A,B,C,D}: g0=A|B, g1=C|D */
                    uintptr_t g0 = 0, g1 = 0;
                    safe_read_ptr(ps + off + 0, &g0);
                    safe_read_ptr(ps + off + 8, &g1);
                    snprintf(e->uid, sizeof(e->uid), "%08X%08X%08X%08X",
                        (int)(g0 & 0xFFFFFFFF), (int)((g0 >> 32) & 0xFFFFFFFF),
                        (int)(g1 & 0xFFFFFFFF), (int)((g1 >> 32) & 0xFFFFFFFF));
                }
            }
        }

        /* IP: 纯内存读取 Controller->NetConnection (无ProcessEvent, 玩家断线时也安全)
         * URL Host 是独立 FString 缓冲(UTF-16LE), 需跟随对象内指针去找 */
        if (e->controller && !e->ip[0]) {
            uintptr_t ccls = 0;
            if (safe_read_ptr(e->controller + 0x10, &ccls) == 0 && ccls > 0x10000) {
                uintptr_t ncp = find_property_in_struct(ccls, "NetConnection");
                if (ncp) {
                    int nco = prop_get_offset(ncp);
                    uintptr_t nc = 0;
                    safe_read_ptr(e->controller + nco, &nc);
                    if (nco >= 0 && nc > 0x10000 && region_contains(nc)) {
                        int found = 0;
                        /* 1. 对象本体前2KB (先验证整段在区域内!) */
                        int can_inline = region_contains(nc) && region_contains(nc + 2047 + 40);
                        if (can_inline) {
                            for (int boff = 0; boff < 2040 && !found; boff += 2) {
                                char tmp[64] = {0};
                                if (match_ipv4_wide((uint8_t*)nc, boff, 2080, tmp, sizeof(tmp))) {
                                    snprintf(e->ip, sizeof(e->ip), "%s", tmp);
                                    found = 1;
                                }
                            }
                            for (int boff = 0; boff < 2048 && !found; boff++) {
                                uint8_t b0 = *(uint8_t*)(nc + boff);
                                if (!(b0 >= '0' && b0 <= '9')) continue;
                                char tmp[64] = {0};
                                int tl = 0, dots = 0, ok = 1;
                                for (int k = 0; k < 40 && tl < 60; k++) {
                                    uint8_t bc = *(uint8_t*)(nc + boff + k);
                                    if (bc >= '0' && bc <= '9') { tmp[tl++] = (char)bc; }
                                    else if (bc == '.' && dots < 3) { tmp[tl++] = '.'; dots++; }
                                    else if (bc == ':' || bc == 0 || bc < 0x20) { break; }
                                    else { ok = 0; break; }
                                }
                                if (ok && dots == 3 && tl >= 7) {
                                    snprintf(e->ip, sizeof(e->ip), "%s", tmp);
                                    found = 1;
                                }
                            }
                        }
                        /* 2. 跟随对象内指针找 */
                        for (int poff = 0; poff < 512 && !found; poff += 8) {
                            uintptr_t cand = 0;
                            memcpy(&cand, (void*)(nc + poff), 8);
                            if (cand < 0x10000 || cand > 0x800000000000UL) continue;
                            if (!region_contains(cand) || !region_contains(cand + 63 + 40)) continue;
                            for (int boff = 0; boff < 60 && !found; boff += 2) {
                                char tmp[64] = {0};
                                if (match_ipv4_wide((uint8_t*)cand, boff, 96, tmp, sizeof(tmp))) {
                                    snprintf(e->ip, sizeof(e->ip), "%s", tmp);
                                    found = 1;
                                }
                            }
                            for (int boff = 0; boff < 64 && !found; boff++) {
                                uint8_t b0 = *(uint8_t*)(cand + boff);
                                if (!(b0 >= '0' && b0 <= '9')) continue;
                                char tmp[64] = {0};
                                int tl = 0, dots = 0, ok = 1;
                                for (int k = 0; k < 40 && tl < 60; k++) {
                                    uint8_t bc = *(uint8_t*)(cand + boff + k);
                                    if (bc >= '0' && bc <= '9') { tmp[tl++] = (char)bc; }
                                    else if (bc == '.' && dots < 3) { tmp[tl++] = '.'; dots++; }
                                    else if (bc == ':' || bc == 0 || bc < 0x20) { break; }
                                    else { ok = 0; break; }
                                }
                                if (ok && dots == 3 && tl >= 7) {
                                    snprintf(e->ip, sizeof(e->ip), "%s", tmp);
                                    found = 1;
                                }
                            }
                        }
                    }
                }
            }
        }

        /* 平台: 从 UPalPlayerAccount 按UID匹配 (Steam/Xbox/Mac/PS5) */
        if (e->uid[0] && !e->platform[0]) {
            collect_player_accounts();
            for (int a = 0; a < g_acc_count; a++) {
                if (strcmp(g_acc_uid[a], e->uid) == 0) {
                    snprintf(e->platform, sizeof(e->platform), "%s", g_acc_platform[a]);
                    break;
                }
            }
        }

        uintptr_t ip = get_player_individual(e->character);
        if (ip) {
            uintptr_t lv = 0, ex = 0;
            safe_read_ptr(ip + 0x3F0, &lv);
            safe_read_ptr(ip + 0x3F8, &ex);
            e->level = (int)(lv & 0xFF);
            e->exp = (int64_t)ex;
        }
        /* 只有等级1-99的真实玩家才调ProcessEvent拿坐标 (防御; 等级上限可能超过50) */
        if (e->level >= 1 && e->level <= 99) {
            actor_get_location(e->character, &e->x);
        }
    }
}


/* GET /players — 在线玩家列表 (名字/UID/等级/经验/坐标/对象地址) */
static void api_players(int fd, const char* req) {
    (void)req;
    if (g_FNamePool == 0 || !g_hook_active) { json_err(fd, 503, "not ready"); return; }
    install_segv_handler();

    /* 20秒TTL缓存 */
    if (g_players_ts == 0 || time(NULL) - g_players_ts > 20) {
        collect_all_players();
    }
    fill_player_details();

    char* buf = (char*)malloc(65536);
    if (!buf) { json_err(fd, 500, "malloc failed"); return; }
    int pos = snprintf(buf, 65536, "{\"players\":[");
    for (int i = 0; i < g_players_count; i++) {
        PlayerEntry* e = &g_players[i];
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, 65536 - pos,
            "{\"name\":\"%s\",\"uid\":\"%s\",\"level\":%d,\"exp\":%lld,"
            "\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,"
            "\"ip\":\"%s\",\"platform\":\"%s\",\"ping\":%.0f,"
            "\"character\":\"0x%lx\",\"playerstate\":\"0x%lx\"}",
            e->name, e->uid, e->level, (long long)e->exp,
            e->x, e->y, e->z, e->ip, e->platform, e->ping, e->character, e->playerstate);
    }
    pos += snprintf(buf + pos, 65536 - pos, "],\"count\":%d}", g_players_count);
    json_ok(fd, buf);
    free(buf);
}

/* 找PalGameStateInGame实例 (BP_PalGameStateInGame_C或PalGameStateInGame, 必须属于World) */
static uintptr_t find_gamestate(void) {
    uintptr_t gs = 0;
    const char* gs_classes[] = {"BP_PalGameStateInGame_C", "PalGameStateInGame", NULL};
    uintptr_t segs[MAX_RW_REGIONS][2];
    int seg_count = snapshot_rw(segs, MAX_RW_REGIONS);
    for (int c = 0; gs_classes[c] && !gs; c++) {
        int fi = find_fname_index(gs_classes[c]);
        if (fi <= 0) continue;
        for (int seg = 0; seg < seg_count && !gs; seg++) {
            uintptr_t* ptr = (uintptr_t*)segs[seg][0];
            size_t cnt = (segs[seg][1] - segs[seg][0]) / sizeof(uintptr_t);
            for (size_t i = 0; i < cnt && !gs; i++) {
                uintptr_t val = ptr[i];
                if (val < 0x10000 || val > 0x800000000000UL) continue;
                uintptr_t ov = 0;
                if (safe_read_ptr(val, &ov) != 0 || !is_plausible_vtable(ov)) continue;
                uintptr_t uc = 0;
                if (safe_read_ptr(val + 0x10, &uc) != 0) continue;
                uintptr_t ucv = 0;
                if (safe_read_ptr(uc, &ucv) != 0 || !is_uclass_vtable(ucv)) continue;
                uintptr_t fn = 0;
                if (safe_read_ptr(uc + 0x18, &fn) != 0) continue;
                if ((int)(fn & 0xFFFFFFFF) == fi && obj_belongs_to_world(val)) { gs = val; }
            }
        }
    }
    return gs;
}

/* POST /announce — 全服公告: PalGameStateInGame::BroadcastServerNotice (NetMulticast RPC) */
static void api_announce(int fd, const char* req) {
    if (g_FNamePool == 0 || !g_hook_active) { json_err(fd, 503, "not ready"); return; }
    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));
    install_segv_handler();

    char msg[1024] = {0};
    json_get_string(body, "message", msg, sizeof(msg));
    if (!msg[0]) { json_err(fd, 400, "message required"); return; }

    /* 找GameState实例 */
    uintptr_t gs = find_gamestate();
    if (!gs) { json_err(fd, 404, "GameState not found"); return; }

    uintptr_t gcls = 0;
    safe_read_ptr(gs + 0x10, &gcls);
    void* ufunc = find_ufunction_in_class(gcls, "BroadcastServerNotice");
    if (!ufunc) { json_err(fd, 404, "BroadcastServerNotice not found"); return; }

    uintptr_t pr = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &pr);
    int parms_size = (int)(pr & 0xFFFFFFFF);
    if (parms_size <= 0 || parms_size > 256) { json_err(fd, 500, "bad ParmsSize"); return; }

    uint8_t* params = (uint8_t*)calloc(1, parms_size);
    /* FString结构 (16字节: Data*,Num,Max) + UTF-16数据, 由引用参数槽指向 */
    uint8_t* fstr = (uint8_t*)calloc(1, 16);
    uint8_t* str_buf = (uint8_t*)calloc(1, (strlen(msg) + 1) * 2);
    int mlen = utf8_to_utf16(msg, (uint16_t*)str_buf, 2048);
    ((uint16_t*)str_buf)[mlen] = 0;
    *(uintptr_t*)(fstr + 0) = (uintptr_t)str_buf;
    *(int32_t*)(fstr + 8) = mlen + 1;
    *(int32_t*)(fstr + 12) = mlen + 1;

    uintptr_t head = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_CHILDPROPS, &head);
    uintptr_t prop = head;
    int count = 0;
    while (prop && prop > 0x10000 && count < 8) {
        count++;
        char pname[128] = {0};
        prop_get_name(prop, pname, sizeof(pname));
        int sz = prop_get_size(prop);
        int off = prop_get_offset(prop);
        palhook_log("  BroadcastServerNotice param '%s' size=%d offset=0x%x", pname, sz, off);
        if (off >= 0 && off + sz <= parms_size) {
            if (sz == 8) {
                /* const FString& 引用参数: 槽里放FString结构地址 */
                *(uintptr_t*)(params + off) = (uintptr_t)fstr;
            } else if (sz == 16) {
                build_fstring_at(params, off, msg, str_buf);
            }
        }
        uintptr_t next = 0;
        if (safe_read_ptr(prop + FPROP_OFFSET_NEXT, &next) != 0) break;
        if (next == prop) break;
        prop = next;
    }

    palhook_log("announce: gs=0x%lx msg='%s'", gs, msg);
    int waited = 0;
    int r = call_and_wait((void*)gs, ufunc, params, 15000, &waited);
    free(params);
    free(fstr);
    free(str_buf);

    char buf[256];
    snprintf(buf, sizeof(buf), "{\"status\":\"%s\",\"message_len\":%d,\"waited_ms\":%d}",
        r == 0 ? "success" : "failed", (int)strlen(msg), waited);
    json_ok(fd, buf);
}

static void api_kick(int fd, const char* req);

/* POST /ban — 封禁玩家 (踢出+面板记录, 同kick机制) */
static void api_ban(int fd, const char* req) {
    api_kick(fd, req);
}

/* POST /kick — 踢出玩家 (按name/uid/character): ClientTravelInternal("Void") */
static void api_kick(int fd, const char* req) {
    if (g_FNamePool == 0 || !g_hook_active) { json_err(fd, 503, "not ready"); return; }
    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));
    install_segv_handler();

    char name[64] = {0}, uid[64] = {0}, char_str[64] = {0};
    json_get_string(body, "name", name, sizeof(name));
    json_get_string(body, "uid", uid, sizeof(uid));
    json_get_string(body, "character", char_str, sizeof(char_str));

    uintptr_t target_char = 0;
    if (char_str[0]) target_char = strtoull(char_str, NULL, 0);

    /* 确保玩家列表是新的 (按名字/uid找时) */
    if (!target_char && (name[0] || uid[0])) {
        collect_all_players();
        fill_player_details();
        for (int i = 0; i < g_players_count; i++) {
            if (name[0] && strcmp(g_players[i].name, name) == 0) target_char = g_players[i].character;
            if (!target_char && uid[0] && strcmp(g_players[i].uid, uid) == 0) target_char = g_players[i].character;
        }
    }
    if (!target_char) {
        json_err(fd, 404, "player not found (name/uid/character required)");
        return;
    }

    /* 找Controller: APawn::Controller属性, 再退到GetPlayerController */
    uintptr_t ctrl = 0;
    uintptr_t ccls = 0;
    safe_read_ptr(target_char + 0x10, &ccls);
    uintptr_t ctrl_prop = find_property_in_struct(ccls, "Controller");
    if (ctrl_prop) {
        int off = prop_get_offset(ctrl_prop);
        if (off >= 0) safe_read_ptr(target_char + off, &ctrl);
    }
    if (!ctrl) ctrl = resolve_ctrl(body);
    if (!ctrl) { json_err(fd, 404, "PlayerController not found"); return; }

    uintptr_t ctrl_cls = 0;
    safe_read_ptr(ctrl + 0x10, &ctrl_cls);
    void* ufunc = find_ufunction_in_class(ctrl_cls, "ClientTravelInternal");
    if (!ufunc) { json_err(fd, 404, "ClientTravelInternal not found"); return; }

    uintptr_t pr = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &pr);
    int parms_size = (int)(pr & 0xFFFFFFFF);
    if (parms_size <= 0 || parms_size > 256) { json_err(fd, 500, "bad ParmsSize"); return; }

    uint8_t* params = (uint8_t*)calloc(1, parms_size);
    uint8_t* str_buf = (uint8_t*)calloc(1, 12);
    build_fstring_at(params, 0, "Void", str_buf);

    palhook_log("kick: target=0x%lx ctrl=0x%lx", target_char, ctrl);
    int waited = 0;
    int r = call_and_wait((void*)ctrl, ufunc, params, 15000, &waited);
    free(params);
    free(str_buf);

    char buf[256];
    snprintf(buf, sizeof(buf), "{\"status\":\"%s\",\"kicked\":\"0x%lx\",\"waited_ms\":%d}",
        r == 0 ? "success" : "failed", target_char, waited);
    json_ok(fd, buf);
}

/* GET /metrics — 运行指标: 玩家数/fps估算/运行时间 */
static void api_metrics(int fd, const char* req) {
    (void)req;
    /* 速度优化: 玩家数直接读 GameState->PlayerArray (无全内存扫描, 毫秒级)
     * 兜底: PlayerArray不可用时用玩家缓存计数 */
    int pc = get_connected_playerstates(NULL, 0);
    if (pc <= 0 && g_players_count > 0) pc = g_players_count;
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"ok\",\"player_count\":%d,\"fps\":%d,\"uptime_sec\":%ld,\"version\":\"%s\"}",
        pc, g_fps, (long)(time(NULL) - g_start_ts), PALHOOK_VERSION);
    json_ok(fd, buf);
}

/* 聊天缓冲区环形池: SendSystemToPlayerChat是普通函数(非NetMulticast RPC),
 * 消息可能异步投递, ProcessEvent返回后立即free会导致异步任务读到已释放内存
 * 环形池让缓冲区存活到下次复用 */
#define CHAT_BUF_SLOTS 16
static uint8_t* g_chat_str_buf[CHAT_BUF_SLOTS] = {0};
static uint8_t* g_chat_arr_buf[CHAT_BUF_SLOTS] = {0};
static int g_chat_buf_idx = 0;

/* POST /chat — 服务器聊天: PalUtility::SendSystemToPlayerChat(world, Message, TArray<FGuid> PlayerUIdList)
 * 发到指定玩家(或所有在线玩家)的聊天窗口, 支持中文 */
static void api_chat(int fd, const char* req) {
    if (g_FNamePool == 0 || !g_hook_active) { json_err(fd, 503, "not ready"); return; }
    char body[4096] = {0};
    read_body(fd, req, body, sizeof(body));
    install_segv_handler();

    char msg[1024] = {0};
    json_get_string(body, "message", msg, sizeof(msg));
    if (!msg[0]) { json_err(fd, 400, "message required"); return; }
    char target[64] = {0};
    json_get_string(body, "name", target, sizeof(target));
    char sender[64] = {0};
    json_get_string(body, "sender", sender, sizeof(sender));
    if (!sender[0]) strcpy(sender, "服务器");

    /* 收集目标玩家UID */
    if (g_players_ts == 0 || time(NULL) - g_players_ts > 20) {
        collect_all_players();
    }
    fill_player_details();

    int n_targets = 0;
    uint8_t guid_arr[16 * MAX_PLAYER_CACHE];
    for (int i = 0; i < g_players_count; i++) {
        PlayerEntry* e = &g_players[i];
        if (target[0] && strcmp(e->name, target) != 0) continue;
        uint32_t g[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; k++) {
            uint32_t v = 0;
            for (int h = 0; h < 8; h++) {
                char c = e->uid[k * 8 + h];
                uint32_t d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else { d = 0; }
                v = (v << 4) | d;
            }
            g[k] = v;
        }
        memcpy(guid_arr + n_targets * 16, g, 16);
        n_targets++;
    }
    if (n_targets == 0) { json_err(fd, 404, "no online players to chat with"); return; }

    /* 聊天入口: GameState::BroadcastChatMessage (NetMulticast Reliable, 免内容过滤直发)
     * 与公告BroadcastServerNotice同款机制 (公告已验证显示)
     * EnterChat_Receive会走异步内容过滤管道, 过滤服务不可达时消息被丢弃 */
    uintptr_t gs = find_gamestate();
    if (!gs) { json_err(fd, 404, "GameState not found"); return; }
    uintptr_t gcls = 0;
    safe_read_ptr(gs + 0x10, &gcls);
    void* ufunc = find_ufunction_in_class(gcls, "BroadcastChatMessage");
    if (!ufunc) { json_err(fd, 404, "BroadcastChatMessage not found"); return; }

    uintptr_t pr = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_PARMSSIZE, &pr);
    int parms_size = (int)(pr & 0xFFFFFFFF);
    if (parms_size <= 0 || parms_size > 1024) { json_err(fd, 500, "bad ParmsSize"); return; }

    uint8_t* params = (uint8_t*)calloc(1, parms_size);

    /* 环形缓冲池: 3块 (消息/发送者/UID数组) */
    int slot = g_chat_buf_idx;
    g_chat_buf_idx = (g_chat_buf_idx + 1) % CHAT_BUF_SLOTS;
    if (g_chat_str_buf[slot]) free(g_chat_str_buf[slot]);
    if (g_chat_arr_buf[slot]) free(g_chat_arr_buf[slot]);
    /* 消息原文, 不加前缀: 客户端已用Sender字段自动显示"[服务器]消息" */
    uint8_t* msg_buf = (uint8_t*)calloc(1, (strlen(msg) + 1) * 2);
    g_chat_str_buf[slot] = msg_buf;
    int mlen = utf8_to_utf16(msg, (uint16_t*)msg_buf, 2048);
    ((uint16_t*)msg_buf)[mlen] = 0;

    uint8_t* sender_buf = (uint8_t*)calloc(1, (strlen(sender) + 1) * 2);
    g_chat_arr_buf[slot] = sender_buf;
    int slen = utf8_to_utf16(sender, (uint16_t*)sender_buf, 128);
    ((uint16_t*)sender_buf)[slen] = 0;

    uint8_t* uid_buf = (uint8_t*)malloc((size_t)n_targets * 16);
    memcpy(uid_buf, guid_arr, (size_t)n_targets * 16);

    /* 参数: ChatMessage结构体 (112字节, 按值内联) */
    uintptr_t head = 0;
    safe_read_ptr((uintptr_t)ufunc + UFUNC_OFFSET_CHILDPROPS, &head);
    uintptr_t prop = head;
    int count = 0;
    int chat_msg_off = -1;
    uintptr_t chat_msg_prop = 0;
    int chat_msg_size = 0;
    while (prop && prop > 0x10000 && count < 8) {
        count++;
        char pname[128] = {0};
        prop_get_name(prop, pname, sizeof(pname));
        int sz = prop_get_size(prop);
        int off = prop_get_offset(prop);
        palhook_log("  BroadcastChatMessage param '%s' size=%d offset=0x%x", pname, sz, off);
        if (strstr(pname, "ChatMessage") && sz > 16) {
            chat_msg_prop = prop;
            chat_msg_off = off;
            chat_msg_size = sz;
        }
        uintptr_t next = 0;
        if (safe_read_ptr(prop + FPROP_OFFSET_NEXT, &next) != 0) break;
        if (next == prop) break;
        prop = next;
    }
    if (chat_msg_off < 0 || chat_msg_size <= 0) {
        free(params);
        json_err(fd, 500, "ChatMessage param not found");
        return;
    }

    uintptr_t struct_def = find_struct_ptr(chat_msg_prop, "ChatMessage");
    if (!struct_def) {
        free(params);
        json_err(fd, 500, "FPalChatMessage struct not found");
        return;
    }
    uintptr_t shead = 0;
    safe_read_ptr(struct_def + UFUNC_OFFSET_CHILDPROPS, &shead);
    uintptr_t sprop = shead;
    int scount = 0;
    while (sprop && sprop > 0x10000 && scount < 32) {
        scount++;
        char sname[128] = {0};
        prop_get_name(sprop, sname, sizeof(sname));
        int ssz = prop_get_size(sprop);
        int soff = prop_get_offset(sprop);
        palhook_log("    ChatMessage.%s size=%d offset=0x%x", sname, ssz, soff);
        if (soff >= 0 && soff + ssz <= chat_msg_size) {
            uint8_t* dst = params + chat_msg_off + soff;
            if (strcmp(sname, "Category") == 0 && ssz == 1) {
                *dst = 1; /* Global */
            } else if (strcmp(sname, "Sender") == 0 && ssz == 16) {
                *(uintptr_t*)(dst + 0) = (uintptr_t)sender_buf;
                *(int32_t*)(dst + 8) = slen + 1;
                *(int32_t*)(dst + 12) = slen + 1;
            } else if (strcmp(sname, "SenderPlayerUId") == 0 && ssz == 16) {
                /* 全零GUID (实测结论):
                 * - 客户端按UID查玩家名: 查到显示玩家名, 查不到回退Sender字段
                 * - 全零 -> 回退Sender("服务器") -> 显示 [服务器]: 消息 ✓
                 * - 全0xFF -> 走占位符分支显示 [-----] ✗
                 * - 填玩家自己的GUID -> 显示玩家昵称 ✗ */
            } else if (strcmp(sname, "Message") == 0 && ssz == 16) {
                *(uintptr_t*)(dst + 0) = (uintptr_t)msg_buf;
                *(int32_t*)(dst + 8) = mlen + 1;
                *(int32_t*)(dst + 12) = mlen + 1;
            } else if (strcmp(sname, "ReceiverPlayerUIds") == 0 && ssz == 16) {
                *(uintptr_t*)(dst + 0) = (uintptr_t)uid_buf;
                *(int32_t*)(dst + 8) = n_targets;
                *(int32_t*)(dst + 12) = n_targets;
            }
        }
        uintptr_t snext = 0;
        if (safe_read_ptr(sprop + FPROP_OFFSET_NEXT, &snext) != 0) break;
        if (snext == sprop) break;
        sprop = snext;
    }

    palhook_log("chat: gs=0x%lx to=%d sender='%s' msg='%s'", gs, n_targets, sender, msg);
    int waited = 0;
    int r = call_and_wait((void*)gs, ufunc, params, 15000, &waited);
    free(params);
    /* 缓冲由环形池管理 */

    char buf[256];
    snprintf(buf, sizeof(buf), "{\"status\":\"%s\",\"sent_to\":%d,\"waited_ms\":%d}",
        r == 0 ? "success" : "failed", n_targets, waited);
    json_ok(fd, buf);
}

/* GET /dump-guilds — 调试: 探测公会管理器内存结构 */

/* GET /dump-guilds — 调试: 探测PalGroupManager的TMap内存布局 */
static void hexdump_log(uintptr_t addr, int len, const char* tag) {
    if (!region_contains(addr) || !region_contains(addr + (uintptr_t)len - 1)) {
        palhook_log("%s: 0x%lx NOT in region", tag, addr);
        return;
    }
    for (int off = 0; off < len; off += 16) {
        char line[256];
        int pos = snprintf(line, sizeof(line), "%s +0x%02x:", tag, off);
        for (int b = 0; b < 16 && off + b < len; b++) {
            uintptr_t v = 0;
            if (safe_read_ptr(addr + off + b, &v) != 0) break;
            pos += snprintf(line + pos, sizeof(line) - pos, " %02x", (int)(v & 0xFF));
        }
        palhook_log("%s", line);
    }
}

static void api_dump_guilds(int fd, const char* req) {
    (void)req;
    if (g_FNamePool == 0 || !g_hook_active) { json_err(fd, 503, "not ready"); return; }

    /* 枚举所有PalGroupManager实例 (可能有废弃实例干扰) */
    uintptr_t objs[64];
    int obj_n = collect_objects_by_class("PalGroupManager", objs, 64);
    palhook_log("dump-guilds: %d PalGroupManager candidates", obj_n);
    uintptr_t gm = get_group_manager();
    if (!gm) { json_err(fd, 404, "PalGroupManager not found"); return; }
    palhook_log("dump-guilds: manager=0x%lx (live pick)", gm);

    /* 每个实例: Outer链类名 + GuildMap/GroupMap TMap头 */
    for (int oi = 0; oi < obj_n; oi++) {
        uintptr_t cand = objs[oi];
        char chain[320] = "";
        int cp = 0;
        {
            uintptr_t cur = cand;
            for (int li = 0; li < 5 && cur > 0x10000; li++) {
                uintptr_t ccls = 0;
                if (safe_read_ptr(cur + 0x10, &ccls) != 0 || ccls < 0x10000) break;
                uintptr_t cfn = 0;
                if (safe_read_ptr(ccls + 0x18, &cfn) != 0) break;
                char cn2[64] = {0};
                resolve_fname((int)(cfn & 0xFFFFFFFF), cn2, sizeof(cn2));
                uintptr_t co = 0;
                safe_read_ptr(cur + 0x20, &co);
                cp += snprintf(chain + cp, sizeof(chain) - cp, "%s%s(0x%lx)", li ? "<-" : "", cn2, co);
                if (co < 0x10000 || co == cur) break;
                cur = co;
            }
        }
        uintptr_t gd = 0, gnr = 0, gmr = 0;
        safe_read_ptr(cand + 0xf8, &gd);
        safe_read_ptr(cand + 0x100, &gnr);
        safe_read_ptr(cand + 0x104, &gmr);
        uintptr_t rd = 0, rnr = 0, rmr = 0;
        safe_read_ptr(cand + 0xa0, &rd);
        safe_read_ptr(cand + 0xa8, &rnr);
        safe_read_ptr(cand + 0xac, &rmr);
        palhook_log("  cand[%d] 0x%lx chain=[%s]", oi, cand, chain);
        palhook_log("    GuildMap{data=0x%lx num=%d max=%d live=%d} GroupMap{data=0x%lx num=%d max=%d live=%d}%s",
            gd, (int)(gnr & 0xFFFFFFFF), (int)(gmr & 0xFFFFFFFF), tmap_looks_live(cand + 0xf8),
            rd, (int)(rnr & 0xFFFFFFFF), (int)(rmr & 0xFFFFFFFF), tmap_looks_live(cand + 0xa0),
            cand == gm ? " <-- picked" : "");
    }

    /* 管理器属性: 找GuildMap/GroupMap偏移 */
    {
        uintptr_t cls = 0;
        safe_read_ptr(gm + 0x10, &cls);
        uintptr_t cur = cls;
        for (int depth = 0; depth < 8 && cur > 0x10000; depth++) {
            uintptr_t fn = 0;
            safe_read_ptr(cur + 0x18, &fn);
            char cls_name[128] = {0};
            resolve_fname((int)(fn & 0xFFFFFFFF), cls_name, sizeof(cls_name));
            uintptr_t head = 0;
            safe_read_ptr(cur + UFUNC_OFFSET_CHILDPROPS, &head);
            uintptr_t prop = head;
            int cnt = 0;
            while (prop && prop > 0x10000 && cnt < 300) {
                cnt++;
                char pname[128] = {0};
                prop_get_name(prop, pname, sizeof(pname));
                int sz = prop_get_size(prop);
                int off = prop_get_offset(prop);
                if (strstr(pname, "Guild") || strstr(pname, "Group")) {
                    palhook_log("  mgrprop[%s] '%s' size=%d offset=0x%x", cls_name, pname, sz, off);
                }
                uintptr_t next = 0;
                if (safe_read_ptr(prop + FPROP_OFFSET_NEXT, &next) != 0) break;
                if (next == prop) break;
                prop = next;
            }
            uintptr_t super = 0;
            if (safe_read_ptr(cur + UCLASS_OFFSET_SUPER, &super) != 0) break;
            if (super < 0x10000 || super == cur) break;
            cur = super;
        }
    }

    /* TMap结构dump + 元素走查 */
    const char* map_names[] = {"GuildMap", "GroupMap", NULL};
    for (int m = 0; map_names[m]; m++) {
        uintptr_t cls = 0;
        safe_read_ptr(gm + 0x10, &cls);
        uintptr_t mp = find_property_in_struct(cls, map_names[m]);
        if (!mp) { palhook_log("prop '%s' not found", map_names[m]); continue; }
        int moff = prop_get_offset(mp);
        int msz = prop_get_size(mp);
        palhook_log("TMap '%s' offset=0x%x size=%d", map_names[m], moff, msz);
        uintptr_t maddr = gm + moff;
        hexdump_log(maddr, msz > 0 && msz <= 256 ? msz : 192, "tmaphdr");
        uintptr_t dptr = 0, num_raw = 0, max_raw = 0;
        safe_read_ptr(maddr, &dptr);
        safe_read_ptr(maddr + 8, &num_raw);
        safe_read_ptr(maddr + 12, &max_raw);
        int num = (int)(num_raw & 0xFFFFFFFF);
        palhook_log("  data=0x%lx num=%d max=%d (inline=%d)", dptr, num,
            (int)(max_raw & 0xFFFFFFFF), (dptr >= maddr && dptr < maddr + 256));
        if (dptr > 0x10000 && num > 0 && num < 128) {
            int hd_len = num * 32 + 32;
            if (hd_len > 160) hd_len = 160;
            hexdump_log(dptr, hd_len, "tmapdata");
            static const int strides[] = {32, 24, 40, 48};
            for (int s = 0; s < 4; s++) {
                int ok = 0;
                for (int i = 0; i < num && i < 16; i++) {
                    uintptr_t e = dptr + (uintptr_t)i * strides[s];
                    if (!region_contains(e + 24)) break;
                    char guid[64] = "";
                    format_guid(e, guid);
                    uintptr_t v = 0;
                    safe_read_ptr(e + 16, &v);
                    int gv = is_guild_object(v);
                    char cn[128] = "";
                    if (gv) {
                        uintptr_t vc = 0;
                        safe_read_ptr(v + 0x10, &vc);
                        uintptr_t vfn = 0;
                        safe_read_ptr(vc + 0x18, &vfn);
                        resolve_fname((int)(vfn & 0xFFFFFFFF), cn, sizeof(cn));
                    }
                    palhook_log("  stride=%d i=%d guid=%s val=0x%lx guild=%d cls='%s'",
                        strides[s], i, guid, v, gv, cn);
                    if (gv) ok++;
                }
                palhook_log("  stride=%d valid=%d/%d", strides[s], ok, num);
            }
        }
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"manager\":\"0x%lx\"}", gm);
    json_ok(fd, buf);
}


/* GET /guilds — 公会列表 (具体类扫描 + GuildMap双路径, 含成员信息) */
static void json_escape_str(const char* src, char* dst, int dst_size) {
    int d = 0;
    for (int i = 0; src[i] && d < dst_size - 2; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[d++] = '\\'; dst[d++] = (char)c; }
        else if (c >= 0x20) dst[d++] = (char)c;
    }
    dst[d] = 0;
}

/* 扫描具体公会类实例: 类链含PalGroupGuildBase + ID非零 (排除UClass对象/CDO) */
static int collect_guild_objects(uintptr_t* out, int max_n) {
    const char* names[] = {"PalGroupGuild", "PalGroupIndependentGuild", "PalGroupGuildBase", NULL};
    int fname_idx[3];
    int nf = 0;
    for (int c = 0; names[c]; c++) {
        int fi = find_fname_index(names[c]);
        if (fi > 0) fname_idx[nf++] = fi;
    }
    if (nf == 0) return 0;
    int n = 0;
    uintptr_t segs[MAX_RW_REGIONS][2];
    int seg_count = snapshot_rw(segs, MAX_RW_REGIONS);
    for (int seg = 0; seg < seg_count && n < max_n; seg++) {
        uintptr_t* ptr = (uintptr_t*)segs[seg][0];
        size_t cnt = (segs[seg][1] - segs[seg][0]) / sizeof(uintptr_t);
        for (size_t i = 0; i < cnt && n < max_n; i++) {
            uintptr_t val = ptr[i];
            if (val < 0x10000 || val > 0x800000000000UL) continue;
            uintptr_t ov = 0;
            if (safe_read_ptr(val, &ov) != 0 || !is_plausible_vtable(ov)) continue;
            uintptr_t uc = 0;
            if (safe_read_ptr(val + 0x10, &uc) != 0) continue;
            uintptr_t ucv = 0;
            if (safe_read_ptr(uc, &ucv) != 0 || !is_uclass_vtable(ucv)) continue;
            uintptr_t fn = 0;
            if (safe_read_ptr(uc + 0x18, &fn) != 0) continue;
            {
                int fnm = (int)(fn & 0xFFFFFFFF);
                int is_cls = 0;
                for (int c = 0; c < nf; c++) if (fnm == fname_idx[c]) { is_cls = 1; break; }
                if (!is_cls) continue;
            }
                /* 排除UClass对象自身: 类链必须含PalGroupGuildBase */
                if (!class_chain_contains(val, "PalGroupGuildBase")) continue;
                /* 排除CDO: ID FGuid必须非零 */
                uintptr_t t0 = 0, t1 = 0;
                safe_read_ptr(val + 0x60, &t0);
                safe_read_ptr(val + 0x68, &t1);
                if (t0 == 0 && t1 == 0) continue;
                int dup = 0;
                for (int k = 0; k < n; k++) if (out[k] == val) { dup = 1; break; }
                if (!dup) out[n++] = val;
            }
        }
    return n;
}

/* 公会结果缓存: 全内存扫描约2-5秒, 8秒TTL内直接复用 */
static char g_guilds_cache[131072];
static int g_guilds_cache_len = 0;
static time_t g_guilds_cache_ts = 0;
static int g_guilds_last_count = 0;
static pthread_mutex_t g_guilds_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t g_guilds_scan_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_guilds_refresh_running = 0;

/* 后台刷新: 单趟扫描全部公会类 -> 序列化 -> 写入缓存 (请求永不等待扫描) */
static void refresh_guilds_cache(int force) {
    if (g_FNamePool == 0 || !g_hook_active) return;
    if (pthread_mutex_trylock(&g_guilds_scan_lock) != 0) return;
    if (!force && g_guilds_cache_len > 0 && time(NULL) - g_guilds_cache_ts < 60) {
        pthread_mutex_unlock(&g_guilds_scan_lock);
        return;
    }

    static uintptr_t guilds[64];
    int guild_n = 0;
    /* 主路径: 按具体类名扫描实例 */
    guild_n = collect_guild_objects(guilds, 64);
    palhook_log("guilds: scan -> %d guild objects", guild_n);

    char* buf = (char*)malloc(131072);
    if (!buf) return;
    int pos = snprintf(buf, 131072, "{\"guilds\":[");
    int out_n = 0;
    static const char* role_names[] = {"None", "GuildMaster", "SubMaster", "Member", "Guest"};

    for (int i = 0; i < guild_n; i++) {
        uintptr_t g = guilds[i];
        uintptr_t gcls = 0;
        if (safe_read_ptr(g + 0x10, &gcls) != 0 || gcls < 0x10000) continue;

        char gid[64] = "";
        format_guid(g + 0x60, gid);
        if (!gid[0]) continue;

        /* 名字: GuildName反射 → 0x190兜底 → GroupName@0x70 */
        char gname_raw[256] = "";
        int noff = guild_prop_offset(gcls, "GuildName", 0x190);
        if (noff >= 0) read_fstring(g + noff, gname_raw, sizeof(gname_raw));
        if (!gname_raw[0]) read_fstring(g + 0x70, gname_raw, sizeof(gname_raw));

        /* 等级 */
        int loff = guild_prop_offset(gcls, "BaseCampLevel", 0x180);
        uintptr_t lv = 0;
        if (loff >= 0) safe_read_ptr(g + loff, &lv);
        int level = (int)(lv & 0xFFFFFFFF);
        if (level < 0 || level > 1000) level = 0;

        /* 营地数 */
        int boff = guild_prop_offset(gcls, "BaseCampIds", 0xa0);
        uintptr_t bnum = 0;
        if (boff >= 0) safe_read_ptr(g + boff + 8, &bnum);
        int bc_count = (int)(bnum & 0xFFFFFFFF);
        if (bc_count < 0 || bc_count > 1024) bc_count = 0;

        /* 会长 */
        char admin_uid[64] = "";
        {
            uintptr_t ap = find_property_in_struct(gcls, "AdminPlayerUId");
            if (ap) {
                int aoff = prop_get_offset(ap);
                if (aoff >= 0) format_guid(g + aoff, admin_uid);
            }
        }

        /* 调试: 前6个对象输出类名/归属/成员反射细节 */
        {
            static int dbg_n = 0;
            if (dbg_n < 6) {
                dbg_n++;
                uintptr_t cfn = 0;
                safe_read_ptr(gcls + 0x18, &cfn);
                char cn[128] = {0};
                resolve_fname((int)(cfn & 0xFFFFFFFF), cn, sizeof(cn));
                palhook_log("guilds: g=0x%lx cls='%s' inworld=%d isGuild=%d isIndep=%d name='%s'",
                    g, cn, obj_belongs_to_world(g),
                    class_chain_contains(g, "PalGroupGuild"),
                    class_chain_contains(g, "PalGroupIndependentGuild"), gname_raw);
            }
        }

        /* 成员列表 */
        char members_json[16384];
        int mpos = 0;
        int member_count = 0;
        mpos += snprintf(members_json + mpos, sizeof(members_json) - mpos, "[");

        if (class_chain_contains(g, "PalGroupGuild")) {
            /* PlayerInfoRepInfoArray (FFastArraySerializer) -> Items TArray<FPalGuildPlayerInfoRepInfo> */
            uintptr_t ra = find_property_in_struct(gcls, "PlayerInfoRepInfoArray");
            if (!ra) {
            }
            if (ra) {
                int ra_off = prop_get_offset(ra);
                uintptr_t rastruct = find_struct_ptr_exact(ra, "PalFastGuildPlayerInfoRepInfoArray");
                if (ra_off >= 0 && rastruct) {
                    uintptr_t items = find_property_in_struct(rastruct, "Items");
                    if (items) {
                        int items_off = prop_get_offset(items);
                        int elem_size = prop_get_size(items);
                        uintptr_t elemstruct = prop_get_struct(items, "PalGuildPlayerInfoRepInfo");
                        {
                            int psz = struct_get_propsize(elemstruct);
                            if (psz > elem_size && psz >= 24 && psz <= 512) elem_size = psz;
                        }
                        if (items_off >= 0 && elem_size >= 48 && elem_size <= 512 && elemstruct) {
                            uintptr_t puid_p = find_property_in_struct(elemstruct, "PlayerUId");
                            uintptr_t pinfo_p = find_property_in_struct(elemstruct, "PlayerInfo");
                            int puid_off = puid_p ? prop_get_offset(puid_p) : -1;
                            int pinfo_off = pinfo_p ? prop_get_offset(pinfo_p) : -1;
                            uintptr_t pinfostruct = pinfo_p ? prop_get_struct(pinfo_p, "PalGuildPlayerInfo") : 0;
                            int pname_off = -1, role_off = -1, status_off = -1, llt_off = -1;
                            if (pinfostruct) {
                                uintptr_t pp = find_property_in_struct(pinfostruct, "PlayerName");
                                if (pp) pname_off = prop_get_offset(pp);
                                pp = find_property_in_struct(pinfostruct, "Role");
                                if (pp) role_off = prop_get_offset(pp);
                                pp = find_property_in_struct(pinfostruct, "Status");
                                if (pp) status_off = prop_get_offset(pp);
                                pp = find_property_in_struct(pinfostruct, "LastOnlineRealTime");
                                if (pp) llt_off = prop_get_offset(pp);
                            }
                            uintptr_t arr = g + ra_off + items_off;
                            uintptr_t dptr = 0, num_raw = 0;
                            safe_read_ptr(arr, &dptr);
                            safe_read_ptr(arr + 8, &num_raw);
                            int num = (int)(num_raw & 0xFFFFFFFF);
                            if (num > 0 && num <= 128 && dptr > 0x10000 && region_contains(dptr)) {
                                for (int mi = 0; mi < num && member_count < 64; mi++) {
                                    uintptr_t e = dptr + (uintptr_t)mi * elem_size;
                                    if (!region_contains(e + 16)) break;
                                    char muid[64] = "";
                                    if (puid_off >= 0) format_guid(e + puid_off, muid);
                                    char mname_raw[256] = "";
                                    int role = -1, status = -1;
                                    int64_t ticks = 0;
                                    if (pinfo_off >= 0) {
                                        uintptr_t pi = e + pinfo_off;
                                        if (pname_off >= 0) read_fstring(pi + pname_off, mname_raw, sizeof(mname_raw));
                                        if (role_off >= 0) {
                                            uintptr_t rv = 0;
                                            safe_read_ptr(pi + role_off, &rv);
                                            role = (int)(rv & 0xFF);
                                        }
                                        if (status_off >= 0) {
                                            uintptr_t sv = 0;
                                            safe_read_ptr(pi + status_off, &sv);
                                            status = (int)(sv & 0xFF);
                                        }
                                        if (llt_off >= 0) {
                                            uintptr_t tv = 0;
                                            safe_read_ptr(pi + llt_off, &tv);
                                            ticks = (int64_t)tv;
                                        }
                                    }
                                    if (!muid[0] && !mname_raw[0]) continue;  /* 空洞 */
                                    char mname[256];
                                    json_escape_str(mname_raw, mname, sizeof(mname));
                                    long long unix_sec = (ticks - 621355968000000000LL) / 10000000LL;
                                    if (unix_sec < 0) unix_sec = 0;
                                    if (mpos > 1) members_json[mpos++] = ',';
                                    mpos += snprintf(members_json + mpos, sizeof(members_json) - mpos,
                                        "{\"uid\":\"%s\",\"name\":\"%s\",\"role\":%d,\"role_name\":\"%s\",\"status\":%d,\"last_online_unix\":%lld}",
                                        muid, mname, role, (role >= 0 && role <= 4) ? role_names[role] : "Unknown",
                                        status, unix_sec);
                                    member_count++;
                                }
                            }
                        }
                    }
                }
            }
        } else if (class_chain_contains(g, "PalGroupIndependentGuild")) {
            /* 单人公会: PlayerUId + PlayerInfo 直接属性 */
            uintptr_t puid_p = find_property_in_struct(gcls, "PlayerUId");
            uintptr_t pinfo_p = find_property_in_struct(gcls, "PlayerInfo");
            int puid_off = puid_p ? prop_get_offset(puid_p) : -1;
            int pinfo_off = pinfo_p ? prop_get_offset(pinfo_p) : -1;
            uintptr_t pinfostruct = pinfo_p ? prop_get_struct(pinfo_p, "PalGuildPlayerInfo") : 0;
            int pname_off = -1, role_off = -1, status_off = -1;
            if (pinfostruct) {
                uintptr_t pp = find_property_in_struct(pinfostruct, "PlayerName");
                if (pp) pname_off = prop_get_offset(pp);
                pp = find_property_in_struct(pinfostruct, "Role");
                if (pp) role_off = prop_get_offset(pp);
                pp = find_property_in_struct(pinfostruct, "Status");
                if (pp) status_off = prop_get_offset(pp);
            }
            char muid[64] = "";
            if (puid_off >= 0) format_guid(g + puid_off, muid);
            char mname_raw[256] = "";
            int role = -1, status = -1;
            if (pinfo_off >= 0) {
                uintptr_t pi = g + pinfo_off;
                if (pname_off >= 0) read_fstring(pi + pname_off, mname_raw, sizeof(mname_raw));
                if (role_off >= 0) {
                    uintptr_t rv = 0;
                    safe_read_ptr(pi + role_off, &rv);
                    role = (int)(rv & 0xFF);
                }
                if (status_off >= 0) {
                    uintptr_t sv = 0;
                    safe_read_ptr(pi + status_off, &sv);
                    status = (int)(sv & 0xFF);
                }
            }
            if (muid[0] || mname_raw[0]) {
                char mname[256];
                json_escape_str(mname_raw, mname, sizeof(mname));
                mpos += snprintf(members_json + mpos, sizeof(members_json) - mpos,
                    "{\"uid\":\"%s\",\"name\":\"%s\",\"role\":%d,\"role_name\":\"%s\",\"status\":%d}",
                    muid, mname, role, (role >= 0 && role <= 4) ? role_names[role] : "Unknown", status);
                member_count = 1;
            }
        }
        mpos += snprintf(members_json + mpos, sizeof(members_json) - mpos, "]");

        /* 过滤假对象: 等级/营地/成员全为0的废弃实例不输出 */
        if (level <= 0 && bc_count <= 0 && member_count <= 0) {
            continue;
        }

        char gname[256];
        json_escape_str(gname_raw, gname, sizeof(gname));
        if (out_n > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, 131072 - pos,
            "{\"id\":\"%s\",\"name\":\"%s\",\"level\":%d,\"basecamp_count\":%d,\"admin_uid\":\"%s\",\"member_count\":%d,\"members\":%s}",
            gid, gname, level, bc_count, admin_uid, member_count, members_json);
        out_n++;
    }
    pos += snprintf(buf + pos, 131072 - pos, "],\"count\":%d,\"source\":\"scan\"}", out_n);
    /* 写入缓存 (短暂加锁) */
    pthread_mutex_lock(&g_guilds_cache_lock);
    if (pos < 131072) {
        memcpy(g_guilds_cache, buf, pos + 1);
        g_guilds_cache_len = pos;
        g_guilds_cache_ts = time(NULL);
        g_guilds_last_count = out_n;
    }
    pthread_mutex_unlock(&g_guilds_cache_lock);
    pthread_mutex_unlock(&g_guilds_scan_lock);
    free(buf);
}

static void* guilds_refresh_thread_fn(void* arg) {
    (void)arg;
    refresh_guilds_cache(0);
    g_guilds_refresh_running = 0;
    return NULL;
}

/* GET /guilds — 返回缓存 (首次同步刷新, 之后60秒TTL+后台刷新, 永不阻塞) */
static void api_guilds(int fd, const char* req) {
    (void)req;
    if (g_FNamePool == 0 || !g_hook_active) { json_err(fd, 503, "not ready"); return; }
    if (g_guilds_cache_len == 0) refresh_guilds_cache(0);
    /* 启动早期(3分钟内)缓存可能是在世界加载完之前扫的空结果, 强制重扫一次 */
    if (g_guilds_cache_len > 0 && g_guilds_last_count == 0 &&
        time(NULL) - g_start_ts < 180) {
        refresh_guilds_cache(1);
    }
    /* 检查+置位必须原子: 每个HTTP请求跑在独立线程, 裸检查会让多个并发请求
     * 同时通过, 拉起多个刷新线程同时做全量对象扫描 */
    if (g_guilds_cache_len > 0 && time(NULL) - g_guilds_cache_ts >= 60) {
        int should_start = 0;
        pthread_mutex_lock(&g_guilds_cache_lock);
        if (!g_guilds_refresh_running) {
            g_guilds_refresh_running = 1;
            should_start = 1;
        }
        pthread_mutex_unlock(&g_guilds_cache_lock);
        if (should_start) {
            pthread_t t;
            if (pthread_create(&t, NULL, guilds_refresh_thread_fn, NULL) == 0) pthread_detach(t);
            else g_guilds_refresh_running = 0;
        }
    }
    char tmp[131072];
    pthread_mutex_lock(&g_guilds_cache_lock);
    int len = g_guilds_cache_len;
    if (len > 0) memcpy(tmp, g_guilds_cache, len + 1);
    pthread_mutex_unlock(&g_guilds_cache_lock);
    if (len <= 0) { json_err(fd, 500, "guilds cache not ready"); return; }
    json_ok(fd, tmp);
}


/* GET /find-value — 调试: 在玩家相关对象里搜索指定数值 (找钱包金币等字段) */
static void api_find_value(int fd, const char* req) {
    (void)req;
    if (g_FNamePool == 0 || !g_hook_active) { json_err(fd, 503, "not ready"); return; }
    install_segv_handler();

    const char* vs = strstr(req, "value=");
    if (!vs) { json_err(fd, 400, "value parameter required"); return; }
    long long target = strtoll(vs + 6, NULL, 0);

    /* 目标对象: 玩家Character + PlayerState + CharacterParameterComponent + IndividualParameter */
    uintptr_t character = find_player_character();
    if (!character) { json_err(fd, 404, "no player online"); return; }

    uintptr_t comp = 0;
    safe_read_ptr(character + 1584, &comp);
    uintptr_t ip = 0;
    if (comp > 0x10000) safe_read_ptr(comp + 376, &ip);
    uintptr_t ps = 0;
    safe_read_ptr(character + 688, &ps);

    palhook_log("find-value: target=%lld char=0x%lx comp=0x%lx ip=0x%lx ps=0x%lx",
        target, character, comp, ip, ps);

    /* 背包InventoryData对象 (金钱可能在其中) */
    uintptr_t inv = 0;
    if (ps > 0x10000) {
        uintptr_t pcls = 0;
        safe_read_ptr(ps + 0x10, &pcls);
        void* gif = find_ufunction_in_class(pcls, "GetInventoryData");
        if (gif) {
            uintptr_t pr0 = 0;
            safe_read_ptr((uintptr_t)gif + UFUNC_OFFSET_PARMSSIZE, &pr0);
            int psz = (int)(pr0 & 0xFFFFFFFF);
            if (psz <= 0 || psz > 64) psz = 8;
            uint8_t* pp = (uint8_t*)calloc(1, psz);
            int waited0 = 0;
            int rr = call_and_wait((void*)ps, gif, pp, 15000, &waited0);
            if (rr == 0) {
                uintptr_t rvp = find_property_in_struct((uintptr_t)gif, "ReturnValue");
                int roff = rvp ? prop_get_offset(rvp) : 0;
                if (roff < 0) roff = 0;
                if (roff + 8 <= psz) inv = *(uintptr_t*)(pp + roff);
            }
            free(pp);
        }
    }

    typedef struct { const char* label; uintptr_t base; size_t size; } ObjRegion;
    ObjRegion objs[5];
    int nobj = 0;
    objs[nobj++] = (ObjRegion){"character", character, 0x4000};
    if (comp > 0x10000) objs[nobj++] = (ObjRegion){"component", comp, 0x2000};
    if (ip > 0x10000) objs[nobj++] = (ObjRegion){"individual", ip, 0x2000};
    if (ps > 0x10000) objs[nobj++] = (ObjRegion){"playerstate", ps, 0x4000};
    if (inv > 0x10000) objs[nobj++] = (ObjRegion){"inventory", inv, 0x4000};

    char* buf = (char*)malloc(32768);
    if (!buf) { json_err(fd, 500, "malloc failed"); return; }
    int pos = snprintf(buf, 32768, "{\"target\":%lld,\"hits\":[", target);
    int found = 0;
    for (int o = 0; o < nobj; o++) {
        for (size_t off = 0; off + 8 <= objs[o].size; off += 4) {
            if (!region_contains(objs[o].base + off) || !region_contains(objs[o].base + off + 7)) continue;
            uintptr_t q = 0;
            if (safe_read_ptr(objs[o].base + off, &q) != 0) continue;
            /* 匹配: int64 / int32 / float (99.0f = 0x42C60000) */
            int hit = ((long long)q == target || (long long)(q & 0xFFFFFFFF) == target ||
                       (long long)(q >> 32) == target);
            if (!hit) {
                float f = (float)target;
                uint32_t fb;
                memcpy(&fb, &f, 4);
                if ((uint32_t)(q & 0xFFFFFFFF) == fb || (uint32_t)(q >> 32) == fb) hit = 1;
            }
            if (hit) {
                if (found < 32) {
                    if (found > 0) buf[pos++] = ',';
                    pos += snprintf(buf + pos, 32768 - pos,
                        "{\"obj\":\"%s\",\"off\":\"0x%lx\",\"val\":\"0x%lx\"}",
                        objs[o].label, (unsigned long)off, q);
                    palhook_log("  HIT %s+0x%lx = 0x%lx", objs[o].label, (unsigned long)off, q);
                }
                found++;
            }
        }
    }
    pos += snprintf(buf + pos, 32768 - pos, "],\"total\":%d}", found);
    json_ok(fd, buf);
    free(buf);
}

/* ========== HTTP路由 ========== */

static void handle_request(int fd) {
    /* 刷新内存映射 (新mmap的FNamePool块等)。
       /proc/self/maps 有几百行, 并发请求各解析一遍纯属浪费, 这里做短期合并。
       TTL 必须短: 快照越旧, region_contains 判断越可能与真实映射脱节。
       (safe_read_ptr 已用 process_vm_readv 兜底, 过期快照不会再导致崩溃,
        但仍会让有效地址被误判为无效, 所以取 250ms 这个量级。) */
    static struct timespec g_maps_ts = {0};
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - g_maps_ts.tv_sec) * 1000
                    + (now.tv_nsec - g_maps_ts.tv_nsec) / 1000000;
    if (elapsed_ms >= 250) {
        g_maps_ts = now;
        parse_proc_maps();
    }

    char req[MAX_REQUEST] = {0};
    int n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) { close(fd); return; }

    char method[16] = {0}, path[256] = {0};
    sscanf(req, "%15s %255s", method, path);

    if (strcmp(method, "OPTIONS") == 0) {
        http_respond(fd, 200, "text/plain", "");
        close(fd);
        return;
    }

    palhook_log("%s %s", method, path);

    /* 认证检查 */
    if (check_auth(fd, req) != 0) {
        close(fd);
        return;
    }

    if (strcmp(path, "/") == 0 || strcmp(path, "/health") == 0) {
        api_health(fd);
    } else if (strcmp(path, "/meminfo") == 0) {
        api_meminfo(fd);
    } else if (strcmp(path, "/maps") == 0) {
        api_maps(fd);
    } else if (strcmp(path, "/scan") == 0) {
        api_scan(fd);
    } else if (strcmp(path, "/help") == 0) {
        api_help(fd);
    } else if (strncmp(path, "/readmem", 8) == 0) {
        api_readmem(fd, req);
    } else if (strncmp(path, "/find-vtable", 12) == 0) {
        api_find_vtable(fd, req);
    } else if (strncmp(path, "/search-bytes", 13) == 0) {
        api_search_bytes(fd, req);
    } else if (strncmp(path, "/fname-search", 13) == 0) {
        api_fname_search(fd, req);
    } else if (strncmp(path, "/find-class", 11) == 0) {
        api_find_class(fd, req);
    } else if (strncmp(path, "/fname", 6) == 0) {
        api_fname(fd, req);
    } else if (strcmp(path, "/call-function") == 0) {
        api_call_function(fd, req);
    } else if (strcmp(path, "/give-item") == 0) {
        api_give_item(fd, req);
    } else if (strcmp(path, "/find-players") == 0) {
        api_find_players(fd, req);
    } else if (strcmp(path, "/give-exp") == 0) {
        api_give_exp_v3(fd, req);
    } else if (strcmp(path, "/give-money") == 0) {
        api_give_money(fd, req);
    } else if (strcmp(path, "/teleport") == 0) {
        api_teleport(fd, req);
    } else if (strcmp(path, "/spawn-pal") == 0) {
        api_spawn_pal_v2(fd, req);
    } else if (strcmp(path, "/get-npc-manager") == 0) {
        api_get_npc_manager(fd, req);
    } else if (strncmp(path, "/find-prop", 10) == 0) {
        api_find_prop(fd, req);
    } else if (strcmp(path, "/writemem") == 0) {
        api_writemem(fd, req);
    } else if (strcmp(path, "/set-exp") == 0 || strcmp(path, "/set-level") == 0) {
        api_set_exp(fd, req);
    } else if (strcmp(path, "/players") == 0) {
        api_players(fd, req);
    } else if (strcmp(path, "/announce") == 0) {
        api_announce(fd, req);
    } else if (strcmp(path, "/kick") == 0) {
        api_kick(fd, req);
    } else if (strcmp(path, "/metrics") == 0) {
        api_metrics(fd, req);
    } else if (strcmp(path, "/chat") == 0) {
        api_chat(fd, req);
    } else if (strcmp(path, "/ban") == 0) {
        api_ban(fd, req);
    } else if (strcmp(path, "/dump-guilds") == 0) {
        api_dump_guilds(fd, req);
    } else if (strcmp(path, "/guilds") == 0) {
        api_guilds(fd, req);
    } else if (strncmp(path, "/find-value", 11) == 0) {
        api_find_value(fd, req);
    } else if (strcmp(path, "/cheat") == 0) {
        api_cheat(fd, req);
    } else if (strcmp(path, "/set-tech-points") == 0) {
        api_set_tech_points(fd, req);
    } else {
        json_err(fd, 404, "endpoint not found, try /help");
    }

    close(fd);
}

/* ========== HTTP服务器线程 ========== */

static void* conn_thread(void* arg);

static void* http_thread(void* arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { palhook_log("ERROR: socket: %s", strerror(errno)); return NULL; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PALHOOK_PORT)
    };

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        palhook_log("ERROR: bind %d: %s", PALHOOK_PORT, strerror(errno));
        close(srv);
        return NULL;
    }
    listen(srv, 8);
    palhook_log("HTTP API on port %d", PALHOOK_PORT);
    g_running = 1;

    /* 每个连接独立线程: 避免慢请求(全内存扫描)阻塞其他请求 */
    while (g_running) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int cfd = accept(srv, (struct sockaddr*)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; break; }

        struct timeval tv = { .tv_sec = 120 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        pthread_t t;
        int* arg = (int*)malloc(sizeof(int));
        *arg = cfd;
        pthread_create(&t, NULL, conn_thread, arg);
        pthread_detach(t);
    }

    close(srv);
    return NULL;
}

/* 连接处理线程 */
static void* conn_thread(void* arg) {
    int cfd = *(int*)arg;
    free(arg);
    handle_request(cfd);
    return NULL;
}

/* ========== 延迟初始化线程 ========== */

static void* delayed_init_thread(void* arg) {
    (void)arg;
    palhook_log("waiting 60s for PalServer to fully initialize...");
    sleep(60);

    palhook_log("starting auto-discovery...");
    if (parse_proc_maps() == 0) {
        discover_ue_globals();
        /* FNamePool: BSS中的固定偏移 (ASLR安全: 基址+偏移)
         * 测试VPS: base=0x200000, FNamePool=0xc0b24b0 -> 偏移0xbe2b4b0
         * 验证: 读CurrentBlock(+8), 应该>0 */
        uintptr_t fnp_candidates[2];
        int fnp_n = 0;
        if (g_base_addr > 0) fnp_candidates[fnp_n++] = g_base_addr + OFF_FNAMEPOOL;
        fnp_candidates[fnp_n++] = 0xc0b24b0; /* 测试VPS绝对地址兜底 */
        for (int c = 0; c < fnp_n; c++) {
            uintptr_t fnp = fnp_candidates[c];
            uintptr_t cb_val = 0;
            if (safe_read_ptr(fnp + 8, &cb_val) != 0) continue;
            uint32_t cur_block = cb_val & 0xFFFFFFFF;
            if (cur_block > 10 && cur_block < 10000) {
                g_FNamePool = fnp;
                palhook_log("FNamePool at 0x%lx (CurrentBlock=%u)", fnp, cur_block);
                break;
            }
        }
        if (!g_FNamePool) {
            palhook_log("FNamePool not found at known offsets, /scan will attempt discovery");
        }
    } else {
        palhook_log("ERROR: cannot parse memory maps");
    }

    /* 重试读取AdminPassword: 此时游戏已加载配置, 内存扫描能找到缓存里的ini原文 */
    if (!g_admin_password[0]) {
        load_admin_password();
    }

    /* 预热缓存: 后台预扫描公会与账号数据 (面板首次打开时秒回, 不阻塞请求) */
    if (g_FNamePool) {
        if (!g_acc_refresh_running) {
            g_acc_refresh_running = 1;
            pthread_t ta;
            if (pthread_create(&ta, NULL, acc_refresh_thread_fn, NULL) == 0) pthread_detach(ta);
            else g_acc_refresh_running = 0;
        }
        if (!g_guilds_refresh_running) {
            g_guilds_refresh_running = 1;
            pthread_t tg;
            if (pthread_create(&tg, NULL, guilds_refresh_thread_fn, NULL) == 0) pthread_detach(tg);
            else g_guilds_refresh_running = 0;
        }
    }
    return NULL;
}

/* ========== LD_PRELOAD入口 ========== */

__attribute__((constructor))
static void palhook_init(void) {
    /* 只注入PalServer进程本体 (LD_PRELOAD会被sh/runuser/curl等子进程继承,
     * 必须严格过滤, 否则13335端口被错误进程抢占) */
    char exe[256] = {0};
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe)-1);
    if (len <= 0) return;
    exe[len] = '\0';
    if (!strstr(exe, "PalServer")) return;

    if (g_init_pid != 0) return;
    g_init_pid = getpid();
    g_start_ts = time(NULL);

    palhook_log("===========================================");
    palhook_log("PalHook v%s initializing...", PALHOOK_VERSION);
    palhook_log("PID: %d", getpid());

    install_segv_handler();
    load_admin_password();

    if (parse_proc_maps() == 0) {
        palhook_log("PalServer base: 0x%lx (%zu MB text)", g_base_addr, g_text_size/(1024*1024));
        palhook_log("Binary segments: %d, All rw regions: %d (%zu MB)",
            g_bin_segment_count, g_all_rw_count, g_total_rw_size/(1024*1024));
    }

    pthread_t ht;
    pthread_create(&ht, NULL, http_thread, NULL);
    pthread_detach(ht);

    pthread_t dt;
    pthread_create(&dt, NULL, delayed_init_thread, NULL);
    pthread_detach(dt);

    palhook_log("PalHook started! API: http://localhost:%d", PALHOOK_PORT);
    palhook_log("===========================================");
}

__attribute__((destructor))
static void palhook_fini(void) {
    if (getpid() != g_init_pid) return;
    g_running = 0;
    palhook_log("PalHook shutdown");
}
