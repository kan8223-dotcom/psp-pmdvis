/* PSP PMD Visualizer — sceGu + pmdmini + MECC + sceAudio */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
/* pspaudiolib不使用 — sceAudio直接 */
#include <pspaudio.h>
#include <psppower.h>
/* MECC (psp-media-engine-custom-core) */
#include "me-core.h"
#include "me-core-mapper.h"

#include <pspiofilemgr.h>
#include <pspsdk.h>
#include <psputility.h>
#include <intraFont.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ストレージプレフィックス — PSP-1000/2000/3000: ms0:  PSP Go: ef0: */
static const char *storage_prefix = "ms0:";
static char songs_dir[64];
static char rhythm_rom_path[128];
static char log_path[64];
static char ss_prefix[64];

static void detect_storage(void) {
    /* ef0: が開ければ Go、なければ ms0: */
    SceUID dfd = sceIoDopen("ef0:/PSP/GAME/PMDVIS");
    if (dfd >= 0) {
        sceIoDclose(dfd);
        storage_prefix = "ef0:";
    } else {
        storage_prefix = "ms0:";
    }
    snprintf(songs_dir, sizeof(songs_dir), "%s/PSP/GAME/PMDVIS/songs", storage_prefix);
    snprintf(rhythm_rom_path, sizeof(rhythm_rom_path), "%s/PSP/GAME/PMDVIS/ym2608_adpcm_rom.bin", storage_prefix);
    snprintf(log_path, sizeof(log_path), "%s/pmd_log.txt", storage_prefix);
    snprintf(ss_prefix, sizeof(ss_prefix), "%s/pmd_ss_", storage_prefix);
}

/* デバッグログ — RAMバッファ方式 (再生中メモステI/Oゼロ) */
#define LOG_BUF_SIZE (128 * 1024)
static char log_buf[LOG_BUF_SIZE];
static int log_len = 0;
static int log_initialized = 0;
static void log_open(void) {
    log_len = 0;
    log_initialized = 1;
}
static void log_write(const char *msg) {
    if (!log_initialized) return;
    int mlen = strlen(msg);
    if (log_len + mlen + 1 < LOG_BUF_SIZE) {
        memcpy(&log_buf[log_len], msg, mlen);
        log_len += mlen;
        log_buf[log_len++] = '\n';
    }
}
static void log_flush(void) {
    if (log_len == 0) return;
    SceUID fd = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) { sceIoWrite(fd, log_buf, log_len); sceIoClose(fd); }
}
static void log_int(const char *label, int val) {
    char buf[64];
    char *p = buf;
    const char *s = label;
    while (*s) *p++ = *s++;
    *p++ = '=';
    if (val < 0) { *p++ = '-'; val = -val; }
    if (val == 0) { *p++ = '0'; }
    else {
        char tmp[12]; int n = 0;
        while (val > 0) { tmp[n++] = '0' + (val % 10); val /= 10; }
        while (n > 0) *p++ = tmp[--n];
    }
    *p = '\0';
    log_write(buf);
}
static void log_hex(const char *label, unsigned int val) {
    char buf[64];
    char *p = buf;
    const char *s = label;
    while (*s) *p++ = *s++;
    *p++ = '='; *p++ = '0'; *p++ = 'x';
    for (int i = 28; i >= 0; i -= 4) {
        int nibble = (val >> i) & 0xF;
        *p++ = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    *p = '\0';
    log_write(buf);
}
static void log_close(void) {
    log_flush();
    log_initialized = 0;
}


/* pmdmini */
#include "pmdmini.h"
/* maskon/maskoff — pmdwin.hで宣言 */
extern int maskon(int ch);
extern int maskoff(int ch);

/* PSP newlib stub */
int ftruncate(int fd, off_t length) { (void)fd; (void)length; return -1; }

PSP_MODULE_INFO("PMD_VIS", 0, 1, 0);  /* ユーザーモード — ME制御はme_driver.prxに委譲 */
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-2048);  /* ME用スタック確保 */

#define PI 3.14159265358979323846f

/* パッドボタン前回状態 (ダイアログ等からの偽検出防止にグローバル) */
static unsigned int prev_buttons = 0;

/* システムの○×設定を取得 (日本=○決定, 海外=×決定) */
static int get_system_accept(void) {
    int val = PSP_UTILITY_ACCEPT_CROSS;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &val);
    return val;
}

#define SCREEN_W 480
#define SCREEN_H 272
#define BUF_WIDTH 512

/* intraFont — PSP内蔵フォント */
static intraFont *g_font = NULL;

/* ビジュアルモード */
#define MODE_KEYBOARD   0
#define MODE_FMP        1
static int vis_mode = MODE_KEYBOARD;

/* アプリ状態 */
#define APP_STATE_LIST  0
#define APP_STATE_PLAY  1
static int app_state = APP_STATE_LIST;
static int list_cursor = 0;
static int list_scroll = 0;

/* ソロモード: D-pad上下でカーソル移動、△でトラックON/OFF切替 */
#define SOLO_NUM_PARTS 11  /* FM1-6, SSG1-3, ADPCM, Rhythm */
static int solo_cursor = 0;                    /* 現在選択中のパート */
static unsigned int solo_mask = 0x7FF;         /* bit=1:ON, 0:OFF (初期:全ON) */
static unsigned int solo_mask_sent = 0x7FF;    /* ME側に送った最新値 */


#define AUDIO_BLOCK_SAMPLES 256
#define PSP_SAMPLERATE      22050
#define PSP_RENDER_SAMPLES  512   /* 22050Hzで生成 (~23.2ms分) */
#define PSP_AUDIO_SAMPLES   1024  /* 44100Hz出力 (512*2=1024, 2xアップサンプル) */
static float pmd_volume_boost = 4.0f;

static volatile int audio_paused = 0;
static volatile int audio_ack_paused = 0; /* audio threadがpause確認済みフラグ */
static volatile int ring_flush_req = 0;   /* ソロ切替時リングフラッシュ要求 */
static volatile int me_active = 0;
static volatile unsigned int *me_shared_uncached;


#define ME_CMD_NONE     0
#define ME_CMD_RENDER   1
#define ME_CMD_FLUSH    2
#define ME_CMD_DECODE_LOOP 3
#define ME_CMD_PRERENDER_DECODE 4
#define ME_CMD_STOP     0xFF
#define ME_STAT_IDLE    0
#define ME_STAT_DONE    1

/* レンダリング用テンポラリバッファ (producer threadが使用) */
static short render_buf[2][PSP_RENDER_SAMPLES * 2] __attribute__((aligned(64)));

/* リングバッファ (producer→output thread間) */
#define RING_BLOCKS_32MB 128  /* 32MB機 (PSP-1000): ~2972ms */
#define RING_BLOCKS_64MB 256  /* 64MB機 (Go/2000/3000): ~5944ms */
static int ring_blocks = RING_BLOCKS_32MB;  /* ランタイムで決定 */
static short (*ring_buf)[PSP_AUDIO_SAMPLES * 2] = NULL;
static volatile int ring_rd = 0;
static volatile int ring_wr = 0;
/* ring_rd == ring_wr: 空, (ring_wr+1)%ring_blocks == ring_rd: 満杯 */
static inline int ring_count(void) {
    int d = ring_wr - ring_rd;
    return d >= 0 ? d : d + ring_blocks;
}
static inline int ring_free(void) {
    return ring_blocks - 1 - ring_count();
}

/* visデータ同期 — リングブロックごとにスナップショット保存、出力時に更新 */
#define VIS_SIZE 23  /* rshot[6] + rdump[6] + notes[11] */
#define FMP_SIZE 44  /* voicenum[11] + volume[11] + detune[11] + fnum[11] */
#define FMP_SH_BASE 60  /* shared memory slot for FMP data */
static unsigned int (*ring_vis)[VIS_SIZE] = NULL;  /* producerが書く */
static volatile unsigned int vis_current[VIS_SIZE];    /* output threadが更新、mainが読む */
static unsigned int (*ring_fmp)[FMP_SIZE] = NULL;  /* producerが書く (FMPデータ) */
static volatile unsigned int fmp_current[FMP_SIZE];    /* output threadが更新、mainが読む */


/* ============================================================
 *  ダブルバッファ ループキャッシュ (PMD用)
 *  MEが1ループ分を事前デコード → 再生中に次ループをデコード
 * ============================================================ */
#define LC_MAX_SEC 30
#define LC_MAX_BLOCKS ((LC_MAX_SEC * PSP_SAMPLERATE / PSP_RENDER_SAMPLES) + 1)
/* 30sec * 22050Hz / 512samp = 1292 blocks, PCM ~2.5MB per buffer */

static short *lc_pcm[2];                /* each: malloc'd [n_blocks][PSP_RENDER_SAMPLES*2] */
static unsigned int *lc_vis[2];          /* each: malloc'd [n_blocks][VIS_SIZE] */
static unsigned int *lc_fmp[2];          /* each: malloc'd [n_blocks][FMP_SIZE] */
static volatile int lc_n_blocks[2];      /* decoded block count per buffer */
static volatile int lc_ready[2];         /* 1 = fully decoded, safe to play */
static volatile int lc_play;             /* 0 or 1: which buffer is playing */
static volatile int lc_play_pos;         /* current block in play buffer */
static volatile int lc_decode;           /* 0 or 1: which buffer ME is filling */
static volatile int lc_me_busy;          /* 1 = ME is running DECODE_LOOP */
static unsigned int lc_decode_start_us = 0;
static volatile int lc_decode_pending;   /* 1 = need to start next decode */
static int lc_enabled;                   /* 1 = cache allocated & active */
static int lc_overflow;                  /* 1 = loop > LC_MAX_SEC, fall back */
static int lc_actual_max_blocks;         /* 実際に確保できたブロック数 */

/* ME forward declarations */
/* ME dcache無効化はsceKernelDcacheWritebackInvalidateAllで十分 */
static void me_render_start(short *buf, int nsamples);
static void me_render_wait(short *buf, int nsamples);
static void process_upsample(short *src, short *dst, int nsamples);

static void save_screenshot(void);
/* 描画ヘルパー前方宣言 (start_trackのデコード進捗表示に使用) */
static void draw_seg_string(float x, float y, float char_w, float char_h, const char *str, unsigned int color);
static void flush_draw(void);


/* intraFontラッパー — フォントがあればintraFont、なければfallback */
static void draw_text(float x, float y, float size, const char *str, unsigned int color) {
    if (g_font) {
        flush_draw(); /* GU頂点をフラッシュしてからテクスチャモード切替 */
        intraFontSetStyle(g_font, size, color, 0x80000000, 0.0f, INTRAFONT_ALIGN_LEFT);
        intraFontPrint(g_font, x, y + size * 14.0f, str);
        sceGuDisable(GU_TEXTURE_2D); /* intraFontがテクスチャを有効にするのでリセット */
    } else {
        draw_seg_string(x, y, size * 7.0f, size * 12.0f, str, color);
    }
}
static void draw_text_center(float x, float y, float size, const char *str, unsigned int color) {
    if (g_font) {
        flush_draw();
        intraFontSetStyle(g_font, size, color, 0x80000000, 0.0f, INTRAFONT_ALIGN_CENTER);
        intraFontPrint(g_font, x, y + size * 14.0f, str);
        sceGuDisable(GU_TEXTURE_2D);
    } else {
        draw_seg_string(x, y, size * 7.0f, size * 12.0f, str, color);
    }
}

/* ============================================================
 *  GU
 * ============================================================ */
static unsigned int __attribute__((aligned(16))) gu_list[262144];

typedef struct {
    unsigned int color;   /* GU_COLOR_8888: ABGR */
    float x, y, z;
} Vertex;
#define VERTEX_FORMAT (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

#define MAX_VERTS 16384
static Vertex verts[MAX_VERTS] __attribute__((aligned(16)));
static int vtx_count = 0;

/* ---- color helpers (ABGR for PSP) ---- */
static unsigned int make_rgba(int r, int g, int b, int a) {
    return ((unsigned int)a << 24) | ((unsigned int)b << 16) |
           ((unsigned int)g << 8) | (unsigned int)r;
}

static unsigned int hsv_to_rgba(float h, float s, float v, int a) {
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r1, g1, b1;
    if (h < 60)       { r1 = c; g1 = x; b1 = 0; }
    else if (h < 120) { r1 = x; g1 = c; b1 = 0; }
    else if (h < 180) { r1 = 0; g1 = c; b1 = x; }
    else if (h < 240) { r1 = 0; g1 = x; b1 = c; }
    else if (h < 300) { r1 = x; g1 = 0; b1 = c; }
    else              { r1 = c; g1 = 0; b1 = x; }
    int r = (int)((r1 + m) * 255); if (r > 255) r = 255;
    int g = (int)((g1 + m) * 255); if (g > 255) g = 255;
    int b = (int)((b1 + m) * 255); if (b > 255) b = 255;
    return make_rgba(r, g, b, a);
}

/* ---- flush draw ---- */
static void flush_draw(void) {
    if (vtx_count == 0) return;
    sceKernelDcacheWritebackRange(verts, vtx_count * sizeof(Vertex));
    sceGuDrawArray(GU_TRIANGLES, VERTEX_FORMAT, vtx_count, NULL, verts);
    vtx_count = 0;
}

/* ---- emit quad ---- */
static void emit_quad(float x0, float y0, float x1, float y1,
                      float x2, float y2, float x3, float y3,
                      unsigned int c0, unsigned int c1, unsigned int c2, unsigned int c3) {
    if (vtx_count + 6 > MAX_VERTS) flush_draw();
    Vertex *v = &verts[vtx_count];
    v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0;
    v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0;
    v[2].color = c2; v[2].x = x2; v[2].y = y2; v[2].z = 0;
    v[3].color = c0; v[3].x = x0; v[3].y = y0; v[3].z = 0;
    v[4].color = c2; v[4].x = x2; v[4].y = y2; v[4].z = 0;
    v[5].color = c3; v[5].x = x3; v[5].y = y3; v[5].z = 0;
    vtx_count += 6;
}

static void emit_rect(float x, float y, float w, float h, unsigned int color) {
    emit_quad(x, y, x+w, y, x+w, y+h, x, y+h, color, color, color, color);
}

static void emit_fan(float cx, float cy, unsigned int cc,
                     float *ex, float *ey, unsigned int *ec, int n) {
    for (int i = 0; i < n - 1; i++) {
        if (vtx_count + 3 > MAX_VERTS) flush_draw();
        Vertex *v = &verts[vtx_count];
        v[0].color = cc; v[0].x = cx; v[0].y = cy; v[0].z = 0;
        v[1].color = ec[i]; v[1].x = ex[i]; v[1].y = ey[i]; v[1].z = 0;
        v[2].color = ec[i+1]; v[2].x = ex[i+1]; v[2].y = ey[i+1]; v[2].z = 0;
        vtx_count += 3;
    }
}

static void emit_circle(float cx, float cy, unsigned int cc, unsigned int ce, float radius, int segs) {
    float ex[33], ey[33];
    unsigned int ec[33];
    if (segs > 32) segs = 32;
    for (int s = 0; s <= segs; s++) {
        float a = 2.0f * PI * (float)s / (float)segs;
        ex[s] = cx + cosf(a) * radius;
        ey[s] = cy + sinf(a) * radius;
        ec[s] = ce;
    }
    emit_fan(cx, cy, cc, ex, ey, ec, segs + 1);
}

/* blend mode flags */
static int cur_blend = 0; /* 0=alpha, 1=additive */
static void set_alpha_blend(void) {
    if (cur_blend != 0) { flush_draw(); cur_blend = 0; }
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}
static void set_additive_blend(void) {
    if (cur_blend != 1) { flush_draw(); cur_blend = 1; }
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
}

/* ---- GU init ---- */
static void gu_init(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, gu_list);
    sceGuDrawBuffer(GU_PSM_8888, (void*)0, BUF_WIDTH);
    sceGuDispBuffer(SCREEN_W, SCREEN_H, (void*)(BUF_WIDTH * SCREEN_H * 4), BUF_WIDTH);
    sceGuDepthBuffer((void*)(BUF_WIDTH * SCREEN_H * 4 * 2), BUF_WIDTH);
    sceGuOffset(2048 - SCREEN_W/2, 2048 - SCREEN_H/2);
    sceGuViewport(2048, 2048, SCREEN_W, SCREEN_H);
    sceGuScissor(0, 0, SCREEN_W, SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

/* ============================================================
 *  埋め込み曲データ (psp-objcopy)
 * ============================================================ */

/* PMD: objcopyシンボル名 _binary_NAME_M_start/_end */
#define SONG_EXTERN(name) \
    extern const unsigned char _binary_##name##_M_start[]; \
    extern const unsigned char _binary_##name##_M_end[];

/* 埋め込み曲は廃止 — 全曲メモステ読み込み (ms0:/PSP/GAME/PMDVIS/songs/) */

#define MAX_TRACKS 64

typedef struct {
    const unsigned char *data;
    int                  size;
    const char          *name;
} TrackInfo;

static TrackInfo tracks[MAX_TRACKS];
static int       num_tracks = 0;
static int       current_track = 0;

#define ADD_TRACK(sym, label) do { \
    tracks[num_tracks].data = _binary_##sym##_M_start; \
    tracks[num_tracks].size = (int)(_binary_##sym##_M_end - _binary_##sym##_M_start); \
    tracks[num_tracks].name = label; \
    num_tracks++; \
} while(0)

/* メモステから .M ファイルをロードして tracks[] に追加 */
/* songs_dir は detect_storage() で設定 */
#define MS_NAME_MAX 64

/* メモステ読み込み用バッファ (malloc分を追跡してリロード時に解放) */
static unsigned char *ms_bufs[MAX_TRACKS];
static char           ms_names[MAX_TRACKS][MS_NAME_MAX];
static int            ms_count = 0;

static void load_memstick_songs(void) {
    char tmp[128];
    /* 前回ロード分を解放 */
    for (int i = 0; i < ms_count; i++) {
        if (ms_bufs[i]) { free(ms_bufs[i]); ms_bufs[i] = NULL; }
    }
    ms_count = 0;

    SceUID dfd = sceIoDopen(songs_dir);
    log_int("memstick: dopen ret", dfd);
    if (dfd < 0) {
        log_write("memstick: songs dir not found");
        return;
    }
    SceIoDirent entry;
    int dread_count = 0;
    while (1) {
        memset(&entry, 0, sizeof(entry));
        int dr = sceIoDread(dfd, &entry);
        if (dr <= 0) break;
        dread_count++;
        if (num_tracks >= MAX_TRACKS) break;
        if (ms_count >= MAX_TRACKS) break;
        /* .M ファイルのみ */
        int nlen = strlen(entry.d_name);
        log_write(entry.d_name);
        if (nlen < 3) continue;
        if (entry.d_name[nlen - 2] != '.' || entry.d_name[nlen - 1] != 'M') continue;

        /* ファイルオープン */
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", songs_dir, entry.d_name);
        SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
        if (fd < 0) continue;

        /* サイズ取得 */
        int fsize = (int)sceIoLseek(fd, 0, SEEK_END);
        sceIoLseek(fd, 0, SEEK_SET);
        if (fsize <= 0 || fsize > 256 * 1024) { /* 256KB上限 */
            sceIoClose(fd);
            continue;
        }

        /* メモリ確保して読み込み */
        unsigned char *buf = (unsigned char *)malloc(fsize);
        if (!buf) { sceIoClose(fd); continue; }
        int rd = sceIoRead(fd, buf, fsize);
        sceIoClose(fd);
        if (rd != fsize) { free(buf); continue; }

        /* トラック登録 */
        ms_bufs[ms_count] = buf;
        /* 拡張子を除いた名前をラベルに (先頭に * 付与でメモステ曲と区別) */
        snprintf(ms_names[ms_count], MS_NAME_MAX, "*%.*s", nlen - 2, entry.d_name);

        tracks[num_tracks].data = buf;
        tracks[num_tracks].size = fsize;
        tracks[num_tracks].name = ms_names[ms_count];
        num_tracks++;
        ms_count++;

        snprintf(tmp, sizeof(tmp), "memstick: loaded %s (%d bytes)", entry.d_name, fsize);
        log_write(tmp);
    }
    sceIoDclose(dfd);
    log_int("memstick: dread_count", dread_count);
    snprintf(tmp, sizeof(tmp), "memstick: %d songs loaded", ms_count);
    log_write(tmp);
}

static void init_tracks(void) {
    num_tracks = 0;
    /* 全曲メモステから読み込み */
    load_memstick_songs();
}

/* ============================================================
 *  SEARCH: ms0:/ 再帰走査 → songs/ にコピー
 * ============================================================ */
#define SEARCH_DIR_STACK 32

static int search_copy_file(const char *src, const char *name) {
    char dst[256];
    snprintf(dst, sizeof(dst), "%s/%s", songs_dir, name);
    /* 既に存在するならスキップ */
    SceUID check = sceIoOpen(dst, PSP_O_RDONLY, 0);
    if (check >= 0) { sceIoClose(check); return 0; }
    /* コピー */
    SceUID sf = sceIoOpen(src, PSP_O_RDONLY, 0);
    if (sf < 0) return -1;
    int fsize = (int)sceIoLseek(sf, 0, SEEK_END);
    sceIoLseek(sf, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 256 * 1024) { sceIoClose(sf); return -1; }
    unsigned char *buf = (unsigned char *)malloc(fsize);
    if (!buf) { sceIoClose(sf); return -1; }
    int rd = sceIoRead(sf, buf, fsize);
    sceIoClose(sf);
    if (rd != fsize) { free(buf); return -1; }
    SceUID df = sceIoOpen(dst, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (df < 0) { free(buf); return -1; }
    sceIoWrite(df, buf, fsize);
    sceIoClose(df);
    free(buf);
    return 1;
}

/* コピー成功したファイルをその場でtracks[]に追加 (リロード不要) */
static void search_add_track(const char *name) {
    if (num_tracks >= MAX_TRACKS || ms_count >= MAX_TRACKS) return;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", songs_dir, name);
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    int fsize = (int)sceIoLseek(fd, 0, SEEK_END);
    sceIoLseek(fd, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 256 * 1024) { sceIoClose(fd); return; }
    unsigned char *buf = (unsigned char *)malloc(fsize);
    if (!buf) { sceIoClose(fd); return; }
    int rd = sceIoRead(fd, buf, fsize);
    sceIoClose(fd);
    if (rd != fsize) { free(buf); return; }
    ms_bufs[ms_count] = buf;
    int nlen = strlen(name);
    snprintf(ms_names[ms_count], MS_NAME_MAX, "*%.*s", nlen - 2, name);
    tracks[num_tracks].data = buf;
    tracks[num_tracks].size = fsize;
    tracks[num_tracks].name = ms_names[ms_count];
    num_tracks++;
    ms_count++;
}

static int search_scan_dir(const char *dir, int depth, int *copied) {
    if (depth > 6) return 0;
    SceUID dfd = sceIoDopen(dir);
    if (dfd < 0) return 0;
    SceIoDirent entry;
    memset(&entry, 0, sizeof(entry));
    while (sceIoDread(dfd, &entry) > 0) {
        char fullpath[256];
        int dlen = strlen(dir);
        snprintf(fullpath, sizeof(fullpath), "%s%s%s",
                 dir, (dlen > 0 && dir[dlen-1] == '/') ? "" : "/",
                 entry.d_name);

        if (FIO_S_ISDIR(entry.d_stat.st_mode)) {
            if (entry.d_name[0] == '.') goto next;
            if (strcmp(fullpath, songs_dir) == 0) goto next;
            search_scan_dir(fullpath, depth + 1, copied);
        } else {
            int nlen = strlen(entry.d_name);
            if (nlen >= 3 && entry.d_name[nlen-2] == '.' && entry.d_name[nlen-1] == 'M') {
                int r = search_copy_file(fullpath, entry.d_name);
                if (r > 0) {
                    char tmp[256];
                    snprintf(tmp, sizeof(tmp), "SEARCH: copied %s", fullpath);
                    log_write(tmp);
                    search_add_track(entry.d_name);
                    (*copied)++;
                }
            }
        }
next:
        memset(&entry, 0, sizeof(entry));
    }
    sceIoDclose(dfd);
    return 0;
}

static int search_and_import(void) {
    log_write("SEARCH: called");
    sceIoMkdir(songs_dir, 0777);
    int copied = 0;
    char search_root[8];
    snprintf(search_root, sizeof(search_root), "%s/", storage_prefix);
    search_scan_dir(search_root, 0, &copied);
    log_int("SEARCH: copied", copied);
    return copied;
}

static void start_track(int idx) {
    if (idx < 0 || idx >= num_tracks) idx = 0;
    log_write("start_track: begin");
    log_int("start_track: idx", idx);
    audio_paused = 1;
    audio_ack_paused = 0; /* staleな値をリセット */
    /* audio threadが確実にpaused分岐に入るのを待つ */
    while (!audio_ack_paused) { sceKernelDelayThread(1000); }
    log_write("start_track: audio paused OK");
    /* MEが合成中なら完了を待つ (DECODE_LOOPキャンセル含む) */
    if (me_active) {
        if (lc_me_busy) {
            me_shared_uncached[42] = 1; /* cancel decode */
        }
        while (me_shared_uncached[0] != ME_CMD_NONE) {
            asm volatile("nop; nop; nop; nop;");
        }
        lc_me_busy = 0;
    }
    log_write("start_track: ME idle OK");
    current_track = idx;
    /* リングバッファ+レンダリングバッファ+vis全クリア (前曲の残響+表示除去) */
    memset(render_buf, 0, sizeof(render_buf));
    memset(ring_buf, 0, (size_t)ring_blocks * PSP_AUDIO_SAMPLES * 2 * sizeof(short));
    memset(ring_vis, 0xFF, (size_t)ring_blocks * VIS_SIZE * sizeof(unsigned int));
    if (ring_fmp) memset(ring_fmp, 0, (size_t)ring_blocks * FMP_SIZE * sizeof(unsigned int));
    memset((void *)vis_current, 0xFF, sizeof(vis_current)); /* 0xFF = note off */
    memset((void *)fmp_current, 0, sizeof(fmp_current));
    ring_rd = 0;
    ring_wr = 0;
    /* Step 1: ME_CMD_FLUSH — MEのdirty PMDWIN状態をRAMに書き戻させる
     * これでSCが後続のpmd_stop/pmd_play_memで正しい状態を読める */
    if (me_active) {
        log_write("start_track: sending ME_CMD_FLUSH");
        me_shared_uncached[1] = ME_STAT_IDLE;
        asm volatile("sync");
        me_shared_uncached[0] = ME_CMD_FLUSH;
        while (me_shared_uncached[1] != ME_STAT_DONE) {
            asm volatile("nop; nop; nop; nop;");
        }
        log_write("start_track: ME_CMD_FLUSH done");
    }
    /* Step 2: SC dcache無効化 — MEがRAMに書き戻した最新PMDWINをSCが読む */
    sceKernelDcacheWritebackInvalidateAll();
    log_write("start_track: calling pmd_stop");
    pmd_stop();
    log_write("start_track: calling pmd_play_mem");
    pmd_play_mem((unsigned char *)tracks[idx].data, tracks[idx].size);
    log_write("start_track: pmd_play_mem done");
    /* Step 3: SCの変更をRAMに反映 + MEに無効化指示 (新曲の状態を読ませる) */
    sceKernelDcacheWritebackInvalidateAll();
    if (me_active) me_shared_uncached[5] = 1; /* MEキャッシュ無効化フラグ */
    if (me_active) { me_shared_uncached[53] = solo_mask; solo_mask_sent = solo_mask; }

    /* Step 4: ダブルバッファ ループキャッシュ or 従来プリフィル */
    ring_rd = 0; ring_wr = 0;

    if (lc_enabled && me_active) {
        /* ===== ハイブリッド方式: 大容量プリフィル→ストリーミング再生 ===== */
        /* MEデコードは実時間の~86%しか出ない(th1等重い曲)ため、
         * 起動時に大量プリフィルして貯金を作り、ストリーミングで消費する。
         * 30秒キャッシュ(1292ブロック)に対し消費は43blocks/sec、
         * ME供給は~37blocks/sec → 差分6blocks/sec → 127ブロック貯金で~21秒持つ。
         * 21秒 + 30秒キャッシュ = 51秒分は確実、次ループ切替で再び貯金。 */
        lc_play = 0;
        lc_play_pos = 0;
        lc_ready[0] = 0;
        lc_ready[1] = 0;
        lc_n_blocks[0] = 0;
        lc_n_blocks[1] = 0;
        lc_overflow = 0;
        lc_decode_pending = 0;

        log_write("start_track: hybrid decode start");
        lc_decode = 0;
        me_shared_uncached[2] = (unsigned int)lc_pcm[0] & 0x1FFFFFFF;
        me_shared_uncached[3] = (unsigned int)PSP_RENDER_SAMPLES;
        me_shared_uncached[4] = 0; /* PMD mode */
        me_shared_uncached[7] = (unsigned int)lc_vis[0] & 0x1FFFFFFF;
        me_shared_uncached[57] = lc_fmp[0] ? ((unsigned int)lc_fmp[0] & 0x1FFFFFFF) : 0;
        me_shared_uncached[40] = lc_actual_max_blocks;
        me_shared_uncached[41] = 0;
        me_shared_uncached[42] = 0;
        me_shared_uncached[43] = 0;
        me_shared_uncached[1] = ME_STAT_IDLE;
        asm volatile("sync");
        me_shared_uncached[0] = ME_CMD_DECODE_LOOP;
        lc_me_busy = 1;
        lc_decode_start_us = sceKernelGetSystemTimeLow();

        /* プリフィル待機中に「Please Wait...」表示 + GPL notice */
        sceGuStart(GU_DIRECT, gu_list);
        sceGuClearColor(0xFF000000);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        draw_text_center(240, 128, 1.2f, "Please Wait...", make_rgba(220, 220, 240, 255));
        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();

        /* MEが255ブロックデコードするまで待つ — 即再生 */
        int lc_prefill_wait = ring_blocks - 1;
        while ((int)me_shared_uncached[41] < lc_prefill_wait
               && me_shared_uncached[1] != ME_STAT_DONE) {
            sceKernelDelayThread(1000); /* 1ms */
        }
        log_int("start_track: me_head_start", (int)me_shared_uncached[41]);
        log_int("opna_sample_rate_init", (int)me_shared_uncached[52]);

        /* プリフィル: MEデコード済み分をリングバッファに充填 */
        {
            int avail = (int)me_shared_uncached[41] - 2; /* 安全マージン */
            int prefill = (avail < ring_blocks - 1) ? avail : ring_blocks - 1;
            if (prefill < 0) prefill = 0;
            sceKernelDcacheInvalidateRange(
                (void *)lc_pcm[0],
                (size_t)prefill * PSP_RENDER_SAMPLES * 2 * sizeof(short));
            sceKernelDcacheInvalidateRange(
                (void *)lc_vis[0],
                (size_t)prefill * VIS_SIZE * sizeof(unsigned int));
            if (lc_fmp[0]) sceKernelDcacheInvalidateRange(
                (void *)lc_fmp[0],
                (size_t)prefill * FMP_SIZE * sizeof(unsigned int));
            for (int i = 0; i < prefill; i++) {
                memcpy(render_buf[0], &lc_pcm[0][i * PSP_RENDER_SAMPLES * 2],
                       PSP_RENDER_SAMPLES * 2 * sizeof(short));
                process_upsample(render_buf[0], ring_buf[ring_wr], PSP_RENDER_SAMPLES);
                for (int vi = 0; vi < VIS_SIZE; vi++)
                    ring_vis[ring_wr][vi] = lc_vis[0][i * VIS_SIZE + vi];
                if (lc_fmp[0]) {
                    for (int fi = 0; fi < FMP_SIZE; fi++)
                        ring_fmp[ring_wr][fi] = lc_fmp[0][i * FMP_SIZE + fi];
                }
                ring_wr = (ring_wr + 1) % ring_blocks;
            }
            lc_play_pos = prefill;
            log_int("prefill_hybrid", prefill);
        }
        lc_decode_pending = 0;
        log_write("start_track: hybrid playback started");
    } else {
        /* ===== 従来方式: リアルタイムMEプリフィル ===== */
        int rb = 0;
        int prefill = ring_blocks - 1;
        if (prefill > ring_blocks - 1) prefill = ring_blocks - 1;
        log_int("prefill_start", prefill);
        for (int i = 0; i < prefill; i++) {
            me_render_start(render_buf[rb], PSP_RENDER_SAMPLES);
            me_render_wait(render_buf[rb], PSP_RENDER_SAMPLES);
            process_upsample(render_buf[rb], ring_buf[ring_wr], PSP_RENDER_SAMPLES);
            if (me_active) {
                for (int vi = 0; vi < VIS_SIZE; vi++)
                    ring_vis[ring_wr][vi] = me_shared_uncached[8 + vi];
                if (ring_fmp) {
                    for (int fi = 0; fi < FMP_SIZE; fi++)
                        ring_fmp[ring_wr][fi] = me_shared_uncached[FMP_SH_BASE + fi];
                }
            }
            ring_wr = (ring_wr + 1) % ring_blocks;
            rb = 1 - rb;
        }
        log_int("prefill_done_ring", ring_count());
    }

    audio_paused = 0;
    log_write("start_track: done, audio resumed");
}

/* YM2608 ADPCM ROM (外部バッファ — opna.cppが参照) */
extern const unsigned char *g_ym2608_adpcm_rom;
extern int                  g_ym2608_adpcm_rom_size;
/* rhythm_rom_path は detect_storage() で設定 */

static int rhythm_rom_loaded = 0;
static void load_rhythm_rom(void) {
    SceUID fd = sceIoOpen(rhythm_rom_path, PSP_O_RDONLY, 0);
    if (fd < 0) { log_write("rhythm ROM: not found (rhythm disabled)"); return; }
    int fsize = (int)sceIoLseek(fd, 0, SEEK_END);
    sceIoLseek(fd, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 512 * 1024) { sceIoClose(fd); log_write("rhythm ROM: bad size"); return; }
    unsigned char *buf = (unsigned char *)malloc(fsize);
    if (!buf) { sceIoClose(fd); return; }
    int rd = sceIoRead(fd, buf, fsize);
    sceIoClose(fd);
    if (rd != fsize) { free(buf); return; }
    g_ym2608_adpcm_rom = buf;
    g_ym2608_adpcm_rom_size = fsize;
    rhythm_rom_loaded = 1;
    log_int("rhythm ROM: loaded, size", fsize);
}




/* ===== WAV Export: 現在の曲を ms0:/MUSIC/ にWAV書き出し ===== */
static int wav_export_loops = 1;    /* 1 or 2 */
static int wav_export_fadeout = 1;  /* 0=off, 1=on (3秒フェードアウト) */
#define WAV_FADEOUT_SEC 3
#define WAV_VOLUME_BOOST 4  /* ノイズ解決済み: 通常ブースト復帰 */

/* エクスポート設定画面 */
static int wav_export_settings(void) {
    SceCtrlData pad, prev;
    sceCtrlPeekBufferPositive(&prev, 1);
    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~prev.Buttons;
        prev = pad;

        if (pressed & PSP_CTRL_TRIANGLE) {
            wav_export_loops = (wav_export_loops == 1) ? 2 : 1;
        }
        if (pressed & PSP_CTRL_SQUARE) {
            wav_export_fadeout = !wav_export_fadeout;
        }
        if (pressed & PSP_CTRL_CIRCLE) {
            prev_buttons = pad.Buttons;
            return 1; /* 開始 */
        }
        if (pressed & PSP_CTRL_CROSS) {
            prev_buttons = pad.Buttons;
            return 0; /* キャンセル */
        }

        /* 設定画面描画 */
        sceGuStart(GU_DIRECT, gu_list);
        sceGuClearColor(0xFF080205);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        vtx_count = 0;
        emit_rect(60, 80, 360, 140, make_rgba(30, 30, 40, 200));
        flush_draw();
        draw_text_center(240, 25, 1.0f, "WAV EXPORT SETTINGS", 0xFF44FFFF);
        draw_text_center(240, 53, 0.8f, tracks[current_track].name, make_rgba(180,180,200,220));
        char loop_str[32];
        snprintf(loop_str, sizeof(loop_str), "Triangle: Loops  %d", wav_export_loops);
        draw_text(80, 88, 0.8f, loop_str, make_rgba(255, 220, 100, 255));
        draw_text(80, 116, 0.8f,
                  wav_export_fadeout ? "Square: Fadeout  ON" : "Square: Fadeout  OFF",
                  wav_export_fadeout ? make_rgba(100, 255, 150, 255) : make_rgba(180, 100, 100, 255));
        draw_text(80, 144, 0.8f, "Mode: ME (fastest)", make_rgba(200,200,200,255));
        draw_text_center(240, 180, 0.7f, "O: Start    X: Cancel", make_rgba(160,160,180,200));
        char info_str[48];
        snprintf(info_str, sizeof(info_str), "44100Hz / Volume x%d", WAV_VOLUME_BOOST);
        draw_text_center(240, 210, 0.65f, info_str, make_rgba(120,140,160,180));
        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }
}

static void wav_export_current(void) {
    if (current_track < 0 || current_track >= num_tracks) return;

    /* 設定画面 */
    if (!wav_export_settings()) return;

    /* 1. オーディオ停止 + ME idle */
    audio_paused = 1;
    audio_ack_paused = 0;
    while (!audio_ack_paused) sceKernelDelayThread(1000);
    if (me_active) {
        if (lc_me_busy) me_shared_uncached[42] = 1; /* cancel decode */
        while (me_shared_uncached[0] != ME_CMD_NONE) asm volatile("nop");
        lc_me_busy = 0;
    }
    /* ME FLUSH */
    if (me_active) {
        me_shared_uncached[1] = ME_STAT_IDLE;
        asm volatile("sync");
        me_shared_uncached[0] = ME_CMD_FLUSH;
        while (me_shared_uncached[1] != ME_STAT_DONE) asm volatile("nop");
    }
    sceKernelDcacheWritebackInvalidateAll();

    /* ME DECODE_LOOPパイプライン: pmdwin1を44100Hzでロード */
    pmd_setrate(44100);
    sceKernelDcacheWritebackInvalidateAll();
    pmd_stop();
    pmd_play_mem((unsigned char *)tracks[current_track].data, tracks[current_track].size);
    sceKernelDcacheWritebackInvalidateAll();
    if (me_active) me_shared_uncached[5] = 1;

    /* 3. ファイル作成 */
    char music_dir[32];
    snprintf(music_dir, sizeof(music_dir), "%s/MUSIC", storage_prefix);
    sceIoMkdir(music_dir, 0777);
    char path[128];
    snprintf(path, sizeof(path), "%s/MUSIC/%s.wav", storage_prefix, tracks[current_track].name);
    /* ファイル名部分のサニタイズ (prefix/MUSIC/ の後) */
    int fname_off = strlen(storage_prefix) + 7; /* "/MUSIC/" = 7 */
    for (char *p = path + fname_off; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
            *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|')
            *p = '_';
    }
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        log_write("WAV export: file open FAILED");
        log_write(path);
        log_int("WAV fd", fd);
        log_flush();
        goto wav_cleanup;
    }

    /* 4. WAVヘッダ (44バイト、サイズは後で上書き) */
    int wav_rate = 44100;
    int wav_ch = 2;
    int wav_bps = 16;
    int byte_rate = wav_rate * wav_ch * (wav_bps / 8);
    int block_align = wav_ch * (wav_bps / 8);
    unsigned char hdr[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0,  1,0,
        (unsigned char)(wav_ch), 0,
        (unsigned char)(wav_rate & 0xFF), (unsigned char)((wav_rate >> 8) & 0xFF),
        (unsigned char)((wav_rate >> 16) & 0xFF), (unsigned char)((wav_rate >> 24) & 0xFF),
        (unsigned char)(byte_rate & 0xFF), (unsigned char)((byte_rate >> 8) & 0xFF),
        (unsigned char)((byte_rate >> 16) & 0xFF), (unsigned char)((byte_rate >> 24) & 0xFF),
        (unsigned char)(block_align), 0,
        (unsigned char)(wav_bps), 0,
        'd','a','t','a', 0,0,0,0
    };
    sceIoWrite(fd, hdr, 44);

    /* 5. WAV Export: ME DECODE_LOOP → SC stream to WAV */

    int total_samples = 0;
    int cancelled = 0;
    int fadeout_blocks = WAV_FADEOUT_SEC * 44100 / PSP_RENDER_SAMPLES;
    unsigned int export_start_us = sceKernelGetSystemTimeLow();
    int est_blocks = 60 * 44100 / PSP_RENDER_SAMPLES;

    log_write("WAV pipeline: circular buf + zero-copy");
    log_int("loops", wav_export_loops);

    {
        int me_loops_done = 0;
        int me_initial_loop_count = 0;

        /* ===== ME DECODE_LOOP 循環バッファパイプライン =====
         * ME: pmdwin1デコード+音量ブースト → lc_pcm循環バッファに書き込み
         * SC: lc_pcmからゼロコピーでメモステ直書き
         * 測定なし。プリレンダなし。マージなし。tempファイルなし。 */
        {
            int buf_cap = lc_actual_max_blocks; /* 循環バッファ容量 (実確保サイズに合わせる) */
            int max_export_blocks = 600 * 44100 / PSP_RENDER_SAMPLES; /* 10分安全弁 */

            /* ME_CMD_DECODE_LOOP発行 (循環バッファモード) */
            sceKernelDcacheWritebackInvalidateAll();
            me_shared_uncached[5] = 1;
            me_shared_uncached[2] = (unsigned int)lc_pcm[0] & 0x1FFFFFFF;
            me_shared_uncached[3] = (unsigned int)PSP_RENDER_SAMPLES;
            me_shared_uncached[7] = (unsigned int)lc_vis[0] & 0x1FFFFFFF;
            me_shared_uncached[40] = (unsigned int)max_export_blocks;
            me_shared_uncached[41] = 0;  /* produced */
            me_shared_uncached[42] = 0;  /* cancel */
            me_shared_uncached[44] = 1;  /* eDRAMバイパス */
            me_shared_uncached[47] = (unsigned int)WAV_VOLUME_BOOST; /* ME側ブースト */
            me_shared_uncached[48] = (unsigned int)buf_cap; /* 循環バッファ容量 */
            me_shared_uncached[49] = 0;  /* consumed */
            me_shared_uncached[50] = wav_export_fadeout ? (unsigned int)fadeout_blocks : 0; /* フェードアウトブロック数 */
            me_shared_uncached[1] = ME_STAT_IDLE;
            asm volatile("sync");
            me_shared_uncached[0] = ME_CMD_DECODE_LOOP;

            log_write("WAV pipeline: ME decode + SC I/O");
            log_int("WAV buf_cap", buf_cap);

            int consumed = 0;
            int disp_tick = 0;
            int block_bytes = PSP_RENDER_SAMPLES * 2 * (int)sizeof(short);

            while (!cancelled) {
                int produced = (int)me_shared_uncached[41];
                int me_done = (me_shared_uncached[1] == ME_STAT_DONE);

                /* ゼロコピー直書き: lc_pcm → メモステ */
                if (produced > consumed) {
                    int avail = produced - consumed;
                    /* 循環バッファの境界を考慮してバッチ書き込み */
                    while (avail > 0) {
                        int buf_idx = consumed % buf_cap;
                        int contig = buf_cap - buf_idx; /* バッファ末尾までの連続ブロック数 */
                        int batch = avail < contig ? avail : contig;
                        short *src = &lc_pcm[0][buf_idx * PSP_RENDER_SAMPLES * 2];
                        int bytes = batch * block_bytes;
                        sceKernelDcacheInvalidateRange(src, bytes);
                        sceIoWrite(fd, src, bytes);
                        consumed += batch;
                        total_samples += batch * PSP_RENDER_SAMPLES;
                        avail -= batch;
                        /* SC消費位置をMEに通知 (MEのフロー制御用) */
                        me_shared_uncached[49] = (unsigned int)consumed;
                    }
                }

                /* ループ検出 → 完了 */
                if (me_done) {
                    /* 残りブロックをドレイン */
                    produced = (int)me_shared_uncached[41];
                    while (consumed < produced) {
                        int buf_idx = consumed % buf_cap;
                        int contig = buf_cap - buf_idx;
                        int batch = (produced - consumed) < contig ? (produced - consumed) : contig;
                        short *src = &lc_pcm[0][buf_idx * PSP_RENDER_SAMPLES * 2];
                        int bytes = batch * block_bytes;
                        sceKernelDcacheInvalidateRange(src, bytes);
                        sceIoWrite(fd, src, bytes);
                        consumed += batch;
                        total_samples += batch * PSP_RENDER_SAMPLES;
                    }
                    me_shared_uncached[49] = (unsigned int)consumed;
                    int lc = (int)me_shared_uncached[43];
                    if (lc > me_initial_loop_count) {
                        me_loops_done++;
                        me_initial_loop_count = lc;
                    }
                    log_int("WAV consumed at loop", consumed);
                    if (me_loops_done >= wav_export_loops || consumed >= max_export_blocks)
                        break;
                    /* まだループ数足りない → 再発行 */
                    me_shared_uncached[41] = 0;
                    me_shared_uncached[42] = 0;
                    me_shared_uncached[49] = 0;
                    consumed = 0;
                    me_shared_uncached[1] = ME_STAT_IDLE;
                    asm volatile("sync");
                    me_shared_uncached[0] = ME_CMD_DECODE_LOOP;
                }

                /* キャンセル + 進捗 (32ブロックごと) */
                disp_tick++;
                if ((disp_tick & 31) == 0) {
                    SceCtrlData cp; sceCtrlPeekBufferPositive(&cp, 1);
                    if (cp.Buttons & PSP_CTRL_CROSS) {
                        me_shared_uncached[42] = 1;
                        cancelled = 1;
                        while (me_shared_uncached[1] != ME_STAT_DONE) asm volatile("nop");
                        break;
                    }
                    /* 進捗表示: 推定超えたら95%固定 (99%詐欺防止) */
                    int pct;
                    if (consumed >= est_blocks) pct = 95;
                    else pct = consumed * 95 / est_blocks;

                    sceGuStart(GU_DIRECT, gu_list);
                    sceGuScissor(0,0,SCREEN_W,SCREEN_H);
                    sceGuDisable(GU_TEXTURE_2D);
                    sceGuClearColor(0xFF080205);
                    sceGuClear(GU_COLOR_BUFFER_BIT);
                    sceGuDisable(GU_DEPTH_TEST);
                    sceGuEnable(GU_BLEND);
                    sceGuBlendFunc(GU_ADD,GU_SRC_ALPHA,GU_ONE_MINUS_SRC_ALPHA,0,0);
                    vtx_count = 0;
                    draw_text_center(240,60,1.0f,"Exporting WAV...",0xFF44FFFF);
                    draw_text_center(240,85,0.7f,tracks[current_track].name,
                        make_rgba(180,180,200,220));
                    {
                        unsigned int exp_sec = total_samples / 44100;
                        char sec_str[32];
                        snprintf(sec_str,sizeof(sec_str),"%d:%02d exported",exp_sec/60,exp_sec%60);
                        draw_text_center(240,120,0.9f,sec_str,make_rgba(0,230,255,255));
                        unsigned int wb2=44+total_samples*4;
                        unsigned int eu=sceKernelGetSystemTimeLow()-export_start_us;
                        char inf[48];
                        if(eu>0){unsigned int kk=(unsigned int)((unsigned long long)wb2*1000000ULL/eu/1024);
                        snprintf(inf,sizeof(inf),"%dKB  %dKB/s",wb2/1024,kk);}
                        else snprintf(inf,sizeof(inf),"%dKB",wb2/1024);
                        draw_text_center(240,145,0.55f,inf,make_rgba(120,160,200,180));
                    }
                    draw_text_center(240,175,0.6f,"Press X to cancel",
                        make_rgba(160,100,100,180));
                    sceGuFinish(); sceGuSync(0,0);
                    sceDisplayWaitVblankStart(); sceGuSwapBuffers();
                }
            }
        }
    }

    if (cancelled) {
        sceIoClose(fd);
        sceIoRemove(path);
        goto wav_cleanup;
    }

    /* 7. WAVヘッダ更新 */
    {
        int data_size = total_samples * wav_ch * (wav_bps / 8);
        int riff_size = data_size + 36;
        unsigned char sz[4];
        sz[0] = riff_size & 0xFF; sz[1] = (riff_size >> 8) & 0xFF;
        sz[2] = (riff_size >> 16) & 0xFF; sz[3] = (riff_size >> 24) & 0xFF;
        sceIoLseek(fd, 4, PSP_SEEK_SET);
        sceIoWrite(fd, sz, 4);
        sz[0] = data_size & 0xFF; sz[1] = (data_size >> 8) & 0xFF;
        sz[2] = (data_size >> 16) & 0xFF; sz[3] = (data_size >> 24) & 0xFF;
        sceIoLseek(fd, 40, PSP_SEEK_SET);
        sceIoWrite(fd, sz, 4);
    }
    sceIoClose(fd);

    {
        unsigned int elapsed_us = sceKernelGetSystemTimeLow() - export_start_us;
        unsigned int elapsed_sec = elapsed_us / 1000000;
        unsigned int file_bytes = 44 + total_samples * 4;
        unsigned int kbps = elapsed_us > 0 ?
            (unsigned int)((unsigned long long)file_bytes * 1000000ULL / elapsed_us / 1024) : 0;
        log_int("WAV total samples", total_samples);
        log_int("WAV file KB", file_bytes / 1024);
        log_int("WAV elapsed sec", elapsed_sec);
        log_int("WAV KB/s", kbps);
        log_write("WAV ME export: done");
    }

    /* 8. 完了画面 + システムダイアログ */
    {
        sceGuStart(GU_DIRECT, gu_list);
        sceGuScissor(0, 0, SCREEN_W, SCREEN_H);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuClearColor(0xFF080205);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        vtx_count = 0;
        draw_text_center(240, 90, 1.0f, "Export Complete!", make_rgba(0,255,120,255));
        draw_text_center(240, 116, 0.7f, tracks[current_track].name,
                         make_rgba(180,180,200,220));
        {
            unsigned int exp_sec = total_samples / 44100;
            char done_str[48];
            snprintf(done_str, sizeof(done_str), "%d:%02d exported", exp_sec/60, exp_sec%60);
            draw_text_center(240, 140, 0.9f, done_str, make_rgba(0,255,120,255));
        }
        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();

        pspUtilityMsgDialogParams dialog;
        memset(&dialog, 0, sizeof(dialog));
        dialog.base.size = sizeof(dialog);
        dialog.base.language = PSP_SYSTEMPARAM_LANGUAGE_JAPANESE;
        dialog.base.buttonSwap = get_system_accept();
        dialog.base.graphicsThread = 0x11;
        dialog.base.accessThread = 0x13;
        dialog.base.fontThread = 0x12;
        dialog.base.soundThread = 0x10;
        dialog.mode = PSP_UTILITY_MSGDIALOG_MODE_TEXT;
        dialog.options = PSP_UTILITY_MSGDIALOG_OPTION_TEXT;
        {
            int lang = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
            sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &lang);
            if (lang == PSP_SYSTEMPARAM_LANGUAGE_JAPANESE) {
                snprintf(dialog.message, sizeof(dialog.message),
                         "WAV\xE3\x82\xA8\xE3\x82\xAF\xE3\x82\xB9\xE3\x83\x9D\xE3\x83\xBC\xE3\x83\x88\xE5\xAE\x8C\xE4\xBA\x86\n"
                         "%s.wav\n"
                         "%s/MUSIC/ \xE3\x81\xAB\xE4\xBF\x9D\xE5\xAD\x98",
                         tracks[current_track].name, storage_prefix);
            } else {
                snprintf(dialog.message, sizeof(dialog.message),
                         "WAV export complete!\n%s.wav\nSaved to %s/MUSIC/",
                         tracks[current_track].name, storage_prefix);
            }
        }
        if (sceUtilityMsgDialogInitStart(&dialog) >= 0) {
            int running = 1;
            while (running) {
                sceGuStart(GU_DIRECT, gu_list);
                sceGuClearColor(0xFF080205);
                sceGuClear(GU_COLOR_BUFFER_BIT);
                sceGuFinish();
                sceGuSync(0, 0);
                int status = sceUtilityMsgDialogGetStatus();
                if (status == 2) sceUtilityMsgDialogUpdate(1);
                else if (status == 3) sceUtilityMsgDialogShutdownStart();
                else if (status == 4 || status == 0) running = 0;
                sceDisplayWaitVblankStart();
                sceGuSwapBuffers();
            }
        }
    }

wav_cleanup:
    /* 9. 22050Hzに復帰 + 曲再開 */
    if (me_active) {
        me_shared_uncached[44] = 0; /* eDRAMバイパス解除 */
        me_shared_uncached[47] = 0; /* 音量ブースト解除 */
        me_shared_uncached[48] = 0; /* 循環バッファ解除 */
        me_shared_uncached[49] = 0; /* consumed位置リセット */
        me_shared_uncached[50] = 0; /* フェードアウト解除 */
        /* まずDECODE_LOOPをキャンセルして完了待ち */
        /* DECODE_LOOPキャンセル → 完了待ち → FLUSH */
        me_shared_uncached[42] = 1; /* cancel flag */
        asm volatile("sync");
        log_write("WAV cleanup: cancel sent");
        {
            int wait_count = 0;
            while (me_shared_uncached[0] != ME_CMD_NONE) {
                asm volatile("nop");
                if (++wait_count > 10000000) {
                    log_write("WAV cleanup: ME cancel TIMEOUT, forcing CMD_NONE");
                    me_shared_uncached[0] = ME_CMD_NONE;
                    break;
                }
            }
        }
        log_write("WAV cleanup: ME idle, sending FLUSH");
        me_shared_uncached[42] = 0; /* cancel flagクリア */
        me_shared_uncached[1] = ME_STAT_IDLE;
        asm volatile("sync");
        me_shared_uncached[0] = ME_CMD_FLUSH;
        {
            int wait_count = 0;
            while (me_shared_uncached[1] != ME_STAT_DONE) {
                asm volatile("nop");
                if (++wait_count > 10000000) {
                    log_write("WAV cleanup: ME flush TIMEOUT");
                    break;
                }
            }
        }
        log_write("WAV cleanup: ME done");
    }
    sceKernelDcacheWritebackInvalidateAll();
    pmd_setrate(PSP_SAMPLERATE);
    sceKernelDcacheWritebackInvalidateAll();
    if (me_active) me_shared_uncached[5] = 1;
    pmd_stop();
    audio_paused = 0;
    /* エクスポート中のボタン状態変化で偽検出防止 */
    { SceCtrlData pad_sync; sceCtrlPeekBufferPositive(&pad_sync, 1); prev_buttons = pad_sync.Buttons; }
}

/* ============================================================
 *  Audio (自前スレッド + sceAudio直接, 低優先度)
 * ============================================================ */
#define ANALYSIS_SIZE 256
static float analysis_l[ANALYSIS_SIZE];
static float analysis_r[ANALYSIS_SIZE];
static volatile int analysis_ready = 0;
static volatile int audio_alive = 1;
static int audio_ch = -1;

/* ============================================================
 *  ME (Media Engine) — MECC (mcidclan) 方式
 *  meLibOnProcess()がME上で実行される
 *  uncachedメモリでSC↔ME通信
 * ============================================================ */
/* 共有メモリ (uncachedセクション) — ME_CMD/STAT定数はファイル先頭付近で定義済み */

static volatile unsigned int _me_shared[128] __attribute__((aligned(64), section(".uncached"))) = {0};
/* [0]=cmd, [1]=status, [2]=buf_phys, [3]=nsamples, [4]=reserved, [5]=invalidate_flag, [6]=edram_addr
 * [8..13]=rshot (BD,SD,CYM,HH,TOM,RIM), [14..19]=rdump (同順)
 * [20..30]=current_notes[11パート] */

/* me_shared_uncached, me_active は上部で定義済み */

/* ME上で実行される合成ループ — eDRAM直接合成方式
 * PSPアーキテクチャ: ME eDRAM (2MB, 2.6GB/s) はME専用高速メモリ。
 * SPU Local Store → DMA put パターンと同じ:
 *   1. eDRAM上で合成 (高帯域・SCキャッシュ競合なし)
 *   2. PMDWIN状態をメインRAMに反映 (dcache writeback)
 *   3. eDRAM → メインRAM転送 (SCのaudio threadが読む)
 */
void meLibOnProcess(void) {
    volatile unsigned int *sh = (volatile unsigned int *)(0x40000000 | (unsigned int)_me_shared);

    /* eDRAM直接アドレス方式 — API不使用 (alloc APIがMEクラッシュ原因だった)
     * ME eDRAM: 0x00000000 base, 2MB (PSP Fat) / 4MB (Slim)
     * cached: 0x00000000, uncached: 0x40000000
     * 先頭4KBを合成バッファとして使用 */
    /* eDRAMの先頭は0x00000000だがC的にはNULL。1KB(0x400)オフセットで回避 */
    short *edram_buf = (short *)0x00000400; /* cached eDRAM offset 1KB */
    int edram_active = 1;
    sh[6] = (unsigned int)edram_buf; /* eDRAMアドレス報告 */

    /* ===== ME FPUテスト ===== */
    {
        /* float加算: 1.5 + 2.5 = 4.0 */
        volatile float fa = 1.5f, fb = 2.5f;
        volatile float f_add = fa + fb;

        /* float乗算: 3.0 * 2.0 = 6.0 */
        volatile float fc = 3.0f, fd = 2.0f;
        volatile float f_mul = fc * fd;

        /* float除算: 10.0 / 3.0 ≈ 3.333... */
        volatile float fe = 10.0f, ff = 3.0f;
        volatile float f_div = fe / ff;

        /* sinf相当: テイラー展開 sin(1.0) ≈ x - x³/6 + x⁵/120 - x⁷/5040 */
        volatile float x = 1.0f;
        volatile float x3 = x * x * x;
        volatile float x5 = x3 * x * x;
        volatile float x7 = x5 * x * x;
        volatile float f_sin = x - x3 / 6.0f + x5 / 120.0f - x7 / 5040.0f;
        /* sin(1.0) ≈ 0.841468 (真値 0.841471) */

        /* 結果をuncached共有メモリに書き出し (float→uintビットキャスト) */
        union { float f; unsigned int u; } conv;
        conv.f = f_add;  sh[32] = conv.u;  /* expect 0x40800000 (4.0) */
        conv.f = f_mul;  sh[33] = conv.u;  /* expect 0x40C00000 (6.0) */
        conv.f = f_div;  sh[34] = conv.u;  /* expect ~0x40555555 (3.333..) */
        conv.f = f_sin;  sh[35] = conv.u;  /* expect ~0x3F574F6C (0.84147) */
        sh[36] = 0xF9010001;  /* マーカー: FPUテスト完了 */

        /* ===== 円周率計算 ===== */
        /* ライプニッツ級数: π/4 = 1 - 1/3 + 1/5 - 1/7 + ... (1000項) */
        {
            volatile float pi_leibniz = 0.0f;
            for (int k = 0; k < 1000; k++) {
                float term = 1.0f / (2.0f * k + 1.0f);
                if (k & 1) pi_leibniz -= term;
                else       pi_leibniz += term;
            }
            pi_leibniz *= 4.0f;
            conv.f = pi_leibniz; sh[37] = conv.u;
        }

        /* マチンの公式: π/4 = 4·arctan(1/5) - arctan(1/239) */
        /* arctan(x) ≈ x - x³/3 + x⁵/5 - x⁷/7 + ... (20項で十分) */
        {
            volatile float at5 = 0.0f, at239 = 0.0f;
            float x5 = 1.0f / 5.0f, x239 = 1.0f / 239.0f;
            float p5 = x5, p239 = x239;
            for (int k = 0; k < 20; k++) {
                float denom = 2.0f * k + 1.0f;
                if (k & 1) { at5 -= p5 / denom; at239 -= p239 / denom; }
                else       { at5 += p5 / denom; at239 += p239 / denom; }
                p5 *= x5 * x5;
                p239 *= x239 * x239;
            }
            volatile float pi_machin = 4.0f * (4.0f * at5 - at239);
            conv.f = pi_machin; sh[38] = conv.u;
        }
        sh[39] = 0xF9020002; /* マーカー: 円周率テスト完了 */
    }

    /* 初回: pmdmini等のグローバル状態をMEキャッシュに取り込む */
    meCoreDcacheWritebackInvalidateAll();
    meLibIcacheInvalidateAll();

    for (;;) {
        /* コマンド待ち */
        while (sh[0] == ME_CMD_NONE) {
            asm volatile("nop; nop; nop; nop;");
        }
        if (sh[0] == ME_CMD_STOP) {
            sh[1] = ME_STAT_DONE;
            meLibHalt();
            return;
        }

        /* ME_CMD_FLUSH: トラック変更時のみ全キャッシュフラッシュ
         * PMDWIN状態をメインRAMに書き戻し+無効化してSCが正しく読めるようにする */
        if (sh[0] == ME_CMD_FLUSH) {
            meCoreDcacheWritebackInvalidateAll();
            meLibIcacheInvalidateAll();
            sh[1] = ME_STAT_DONE;
            sh[0] = ME_CMD_NONE;
            continue;
        }

        /* sh[5]=1ならキャッシュ無効化 (トラック変更後、最初のレンダリング前) */
        if (sh[5]) {
            meCoreDcacheWritebackInvalidateAll();
            meLibIcacheInvalidateAll();
            sh[5] = 0;
        }

        /* ME_CMD_DECODE_LOOP: 1ループ分を一括デコードしてキャッシュに書き込む
         * sh[2]=PCMバッファ物理addr, sh[3]=ブロックサンプル数
         * sh[7]=visバッファ物理addr, sh[40]=最大ブロック数
         * sh[41]=進捗(MEが更新), sh[42]=キャンセルフラグ(SCが設定)
         * sh[43]=デコード完了時のループカウント */
        /* ME_CMD_PRERENDER_DECODE: プリレンダ→レート切替→DECODE_LOOP一括
         * sh[45]=プリレンダブロック数(11025Hz), sh[46]=プリレンダ進捗(MEが更新)
         * その後DECODE_LOOPと同じ動作 */
        if (sh[0] == ME_CMD_PRERENDER_DECODE) {
            int pre_target = (int)sh[45];
            sh[46] = 0;
            /* Phase A: 11025Hzでpre_targetブロック早送り (出力破棄) */
            short discard_buf[PSP_RENDER_SAMPLES * 2] __attribute__((aligned(16)));
            for (int pb = 0; pb < pre_target && !sh[42]; pb++) {
                pmd_renderer(discard_buf, PSP_RENDER_SAMPLES);
                sh[46] = pb + 1;
            }
            if (sh[42]) {
                sh[1] = ME_STAT_DONE;
                sh[0] = ME_CMD_NONE;
                continue;
            }
            /* Phase B: 44100Hzに切替 */
            pmd_setrate(44100);
            meCoreDcacheWritebackInvalidateAll();
            sh[46] = pre_target; /* プリレンダ完了マーク */
            /* Phase C: DECODE_LOOP (以下と同一処理にfall through) */
            short *pcm_base = (short *)(0x80000000 | sh[2]);
            int ns = (int)sh[3];
            unsigned int *vis_base = (unsigned int *)(0x80000000 | sh[7]);
            int max_blocks = (int)sh[40];
            int byte_size = ns * 2 * sizeof(short);
            int initial_loop = pmd_getloopcount();
            int block = 0;
            sh[41] = 0;
            while (block < max_blocks && !sh[42]) {
                if (sh[5]) {
                    meCoreDcacheWritebackInvalidateAll();
                    meLibIcacheInvalidateAll();
                    sh[5] = 0;
                }
                short *dst = pcm_base + block * ns * 2;
                int use_edram2 = edram_active && !sh[44];
                short *render_dst = use_edram2 ? edram_buf : dst;
                pmd_renderer(render_dst, ns);
                if (use_edram2) {
                    meCoreDcacheWritebackRange((void*)edram_buf, byte_size);
                    meCoreMemcpy((unsigned int*)dst, (unsigned int*)edram_buf, byte_size);
                }
                meCoreDcacheWritebackRange((void*)dst, byte_size);
                pmd_fill_vis_data(&sh[8]);
                unsigned int *vdst = vis_base + block * VIS_SIZE;
                for (int vi = 0; vi < VIS_SIZE; vi++) vdst[vi] = sh[8 + vi];
                meCoreDcacheWritebackRange((void*)vdst, VIS_SIZE * sizeof(unsigned int));
                /* FMPデータ (PRERENDER_DECODE) */
                {
                    unsigned int *fmp_base_ptr = sh[57] ? (unsigned int *)(0x80000000 | sh[57]) : 0;
                    if (fmp_base_ptr) {
                        pmd_fill_fmp_data(&sh[FMP_SH_BASE]);
                        unsigned int *fdst = fmp_base_ptr + block * FMP_SIZE;
                        for (int fi = 0; fi < FMP_SIZE; fi++)
                            fdst[fi] = sh[FMP_SH_BASE + fi];
                        meCoreDcacheWritebackRange((void*)fdst, FMP_SIZE * sizeof(unsigned int));
                    }
                }
                block++;
                sh[41] = block;
                int cur_loop = pmd_getloopcount();
                if (cur_loop != initial_loop) {
                    sh[43] = cur_loop;
                    initial_loop = cur_loop;
                    /* breakしない — max_blocksまでデコード継続 */
                }
            }
            sh[41] = block;
            sh[1] = ME_STAT_DONE;
            sh[0] = ME_CMD_NONE;
            continue;
        }

        if (sh[0] == ME_CMD_DECODE_LOOP) {
            short *pcm_base = (short *)(0x80000000 | sh[2]);
            int ns = (int)sh[3];
            unsigned int *vis_base = (unsigned int *)(0x80000000 | sh[7]);
            int max_blocks = (int)sh[40];
            int byte_size = ns * 2 * sizeof(short);
            int buf_cap = (int)sh[48]; /* 循環バッファ容量 (0=従来リニア) */
            int vol_boost = (int)sh[47]; /* 音量ブースト (0=なし) */
            int fade_blocks = (int)sh[50]; /* フェードアウトブロック数 (0=なし) */

            int initial_loop = pmd_getloopcount();
            int block = 0;
            int fade_start = -1; /* フェード開始ブロック (-1=未開始) */
            unsigned int me_solo_mask = 0x7FF; /* ME側の現在マスク (全ON) */
            sh[41] = 0;

            /* FIDELITY計測: sample_rateでMIN/MAX判定 (sh[52]に書く) */
            sh[52] = pmd_get_opna_sample_rate();

            while (block < max_blocks && !sh[42]) {
                /* ソロマスク更新 (SCから通知、ブロック境界で1回だけ) */
                {
                    unsigned int new_mask = sh[53];
                    if (new_mask && new_mask != me_solo_mask) {
                        sh[54] = new_mask; /* MEが認識したマスクをSCに返す (デバッグ) */
                        sh[55] = (unsigned int)block; /* 適用ブロック位置 */
                        for (int ci = 0; ci < SOLO_NUM_PARTS; ci++) {
                            if (new_mask & (1 << ci)) maskoff(ci);
                            else maskon(ci);
                        }
                        me_solo_mask = new_mask;
                    }
                    /* ソロ中はME先行距離をキャップ (即時反映のため) */
                    if (me_solo_mask != 0x7FF) {
                        while ((block - (int)sh[56]) > 4 && !sh[42] && sh[53] != 0x7FF)
                            ; /* spin: SCが追いつくまで待機、全復帰で即解除 */
                    }
                }
                if (sh[5]) {
                    meCoreDcacheWritebackInvalidateAll();
                    meLibIcacheInvalidateAll();
                    sh[5] = 0;
                }

                /* 循環バッファ: SCの消費待ち */
                if (buf_cap > 0) {
                    while ((block - (int)sh[49]) >= buf_cap && !sh[42])
                        ; /* spin */
                    if (sh[42]) break;
                }

                int buf_idx = buf_cap > 0 ? (block % buf_cap) : block;
                short *dst = pcm_base + buf_idx * ns * 2;
                int use_edram2 = edram_active && !sh[44];
                short *render_dst = use_edram2 ? edram_buf : dst;

                pmd_renderer(render_dst, ns);

                /* ME側音量ブースト + フェードアウト (WAVエクスポート時) */
                {
                    int fv = 256; /* フェード係数 (256=100%) */
                    if (fade_start >= 0 && fade_blocks > 0) {
                        int fp = block - fade_start;
                        if (fp >= fade_blocks) fv = 0;
                        else fv = 256 * (fade_blocks - fp) / fade_blocks;
                    }
                    int b = (vol_boost ? vol_boost : 1) * fv; /* boost * fade */
                    if (b != 256 || vol_boost) { /* 変換が必要な場合のみ */
                        for (int i = 0; i < ns * 2; i++) {
                            int s = render_dst[i] * b >> 8;
                            if (s > 32767) s = 32767;
                            if (s < -32768) s = -32768;
                            render_dst[i] = (short)s;
                        }
                    }
                }

                if (use_edram2) {
                    meCoreDcacheWritebackRange((void*)edram_buf, byte_size);
                    meCoreMemcpy((unsigned int*)dst, (unsigned int*)edram_buf, byte_size);
                }
                meCoreDcacheWritebackRange((void*)dst, byte_size);

                /* vis+fmp: WAVエクスポート時はスキップ (buf_cap>0) */
                if (buf_cap == 0) {
                    pmd_fill_vis_data(&sh[8]);
                    unsigned int *vdst = vis_base + block * VIS_SIZE;
                    for (int vi = 0; vi < VIS_SIZE; vi++)
                        vdst[vi] = sh[8 + vi];
                    meCoreDcacheWritebackRange((void*)vdst, VIS_SIZE * sizeof(unsigned int));
                    /* FMPデータ */
                    unsigned int *fmp_base_ptr = sh[57] ? (unsigned int *)(0x80000000 | sh[57]) : 0;
                    if (fmp_base_ptr) {
                        pmd_fill_fmp_data(&sh[FMP_SH_BASE]);
                        unsigned int *fdst = fmp_base_ptr + block * FMP_SIZE;
                        for (int fi = 0; fi < FMP_SIZE; fi++)
                            fdst[fi] = sh[FMP_SH_BASE + fi];
                        meCoreDcacheWritebackRange((void*)fdst, FMP_SIZE * sizeof(unsigned int));
                    }
                }

                block++;
                sh[41] = block;

                /* ループ境界検出 */
                int cur_loop = pmd_getloopcount();
                if (cur_loop != initial_loop) {
                    sh[43] = cur_loop;
                    if (fade_blocks > 0 && fade_start < 0) {
                        /* フェードアウト開始 — ループ後も継続レンダリング */
                        fade_start = block;
                    }
                    initial_loop = cur_loop; /* 次ループ検出用に更新 */
                    /* breakしない — max_blocksまでデコード継続
                     * ループ区間が短い曲でも30秒分のバッファを埋める */
                }
                /* フェード完了チェック */
                if (fade_start >= 0 && block >= fade_start + fade_blocks) break;
            }

            sh[41] = block;
            sh[1] = ME_STAT_DONE;
            sh[0] = ME_CMD_NONE;
            continue;
        }

        short *main_buf = (short *)(0x80000000 | sh[2]);
        int ns = (int)sh[3];
        int byte_size = ns * 2 * sizeof(short);

        /* sh[44]=1ならeDRAMバイパス (WAVエクスポート時: eDRAM経由のノイズ回避) */
        int use_edram = edram_active && !sh[44];
        short *render_dst = use_edram ? edram_buf : main_buf;

        pmd_renderer(render_dst, ns);

        /* ビジュアライズデータをuncached共有メモリに直接書き出し
         * キャッシュ経由しないのでSCから即座に見える。dcache flush不要 */
        pmd_fill_vis_data(&sh[8]);
        pmd_fill_fmp_data(&sh[FMP_SH_BASE]);
        sh[43] = (unsigned int)pmd_getloopcount();

        if (use_edram) {
            /* eDRAM → メインRAM転送 */
            meCoreDcacheWritebackRange((void*)edram_buf, byte_size);
            meCoreMemcpy((unsigned int*)main_buf, (unsigned int*)edram_buf, byte_size);
            meCoreDcacheWritebackRange((void*)main_buf, byte_size);
        } else {
            /* メインRAM直接レンダ → dcache writeback */
            meCoreDcacheWritebackRange((void*)main_buf, byte_size);
        }

        sh[1] = ME_STAT_DONE;
        sh[0] = ME_CMD_NONE;
    }
}

/* ME合成開始 (ノンブロッキング) */
static void me_render_start(short *buf, int nsamples) {
    if (!me_active) {
        pmd_renderer(buf, nsamples);
        return;
    }
    me_shared_uncached[2] = (unsigned int)buf & 0x1FFFFFFF;
    me_shared_uncached[3] = (unsigned int)nsamples;
    me_shared_uncached[1] = ME_STAT_IDLE;
    asm volatile("sync");
    me_shared_uncached[0] = ME_CMD_RENDER;
}

/* ME合成完了待ち — リング残量に応じて動的yield
 * ring余裕あり(>=8): 長めyieldで描画にCPU渡す
 * ring少ない(<8): タイトスピンでオーディオ優先 */
static void me_render_wait(short *buf, int nsamples) {
    if (!me_active) return; /* フォールバック時は既に完了 */
    int rc = ring_count();
    if (rc >= 8) {
        /* リングに余裕あり — 描画優先: 短スピン + 長yield */
        while (me_shared_uncached[1] != ME_STAT_DONE) {
            for (int i = 0; i < 5000; i++) {
                if (me_shared_uncached[1] == ME_STAT_DONE) goto done;
                asm volatile("nop; nop; nop; nop;");
            }
            sceKernelDelayThread(2000); /* 2ms yield — mainに描画させる */
        }
    } else {
        /* リング少ない — オーディオ優先: 長スピン + 短yield */
        while (me_shared_uncached[1] != ME_STAT_DONE) {
            for (int i = 0; i < 50000; i++) {
                if (me_shared_uncached[1] == ME_STAT_DONE) goto done;
                asm volatile("nop; nop; nop; nop;");
            }
            sceKernelDelayThread(50); /* 50μs — HOMEボタン応答用 */
        }
    }
done:
    sceKernelDcacheInvalidateRange(buf, nsamples * 2 * sizeof(short));
}

/* ボリューム調整 + 2xアップサンプル 22050→44100 (各サンプルを2回出力) */
static void process_upsample(short *src, short *dst, int nsamples) {
    for (int i = 0; i < nsamples; i++) {
        int l = src[i*2];
        int r = src[i*2+1];
        l = (l * (int)(pmd_volume_boost * 256)) >> 8;
        r = (r * (int)(pmd_volume_boost * 256)) >> 8;
        if (l >  32767) l =  32767; if (l < -32768) l = -32768;
        if (r >  32767) r =  32767; if (r < -32768) r = -32768;
        short sl = (short)l, sr = (short)r;
        dst[i*4]   = sl; dst[i*4+1] = sr;
        dst[i*4+2] = sl; dst[i*4+3] = sr;
    }
}

/* ============================================================
 *  Audio Output Thread (最高優先度)
 *  リングバッファから読んでsceAudioに出力するだけ。
 *  レンダリングは一切しない → sceAudio間のギャップ = 数μs
 * ============================================================ */
static short silence_buf[PSP_AUDIO_SAMPLES * 2] __attribute__((aligned(64)));

static int audio_output_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    memset(silence_buf, 0, sizeof(silence_buf));

    while (audio_alive) {
        if (audio_paused || app_state == APP_STATE_LIST) {
            audio_ack_paused = 1;
            sceAudioOutputBlocking(audio_ch, PSP_AUDIO_VOLUME_MAX, silence_buf);
            continue;
        }
        audio_ack_paused = 0;

        /* ソロ切替時: リングが厚い場合のみフラッシュ (キャップ中は不要) */
        if (ring_flush_req) {
            int avail = ring_count();
            if (avail > 16) {
                ring_rd = (ring_wr - 2 + ring_blocks) % ring_blocks;
            }
            ring_flush_req = 0;
        }

        /* リングにデータが来るまでスピンウェイト (silence出力しない)
         * silence→実データの遷移がプチプチの原因になるため */
        while (ring_count() == 0 && audio_alive && !audio_paused) {
            sceKernelDelayThread(100); /* 0.1ms — producerにCPU時間を渡す */
        }
        if (!audio_alive || audio_paused) continue;

        /* vis_current/fmp_currentを再生中ブロックのスナップショットで更新 */
        for (int vi = 0; vi < VIS_SIZE; vi++)
            vis_current[vi] = ring_vis[ring_rd][vi];
        if (ring_fmp) {
            for (int fi = 0; fi < FMP_SIZE; fi++)
                fmp_current[fi] = ring_fmp[ring_rd][fi];
        }
        sceAudioOutputBlocking(audio_ch, PSP_AUDIO_VOLUME_MAX, ring_buf[ring_rd]);
        ring_rd = (ring_rd + 1) % ring_blocks;
    }
    sceKernelExitDeleteThread(0);
    return 0;
}

/* ============================================================
 *  Audio Producer Thread (output threadより低優先度)
 *  ME合成 → アップサンプル → リングバッファに書き込む
 * ============================================================ */
/* per-blockタイミング計測 (volatileでmainから読める、ログI/Oはしない) */
static volatile unsigned int prod_render_us = 0;  /* 直近のMEレンダリング時間(μs) */
static volatile unsigned int prod_max_us = 0;     /* 最大レンダリング時間(μs) */
static volatile int prod_underrun_cnt = 0;         /* リングアンダーラン回数 */

/* タイミングログ — RAM配列に溜めて終了時に一括書き出し */
#define TLOG_MAX 2000
static unsigned short tlog_render[TLOG_MAX];  /* render_us (上位切り捨て、65535μsまで) */
static unsigned char  tlog_ring[TLOG_MAX];    /* ring_count */
static int tlog_idx = 0;

/* ループキャッシュ: バックグラウンドMEデコード完了チェック + 次ループ開始 */
static void lc_check_decode_done(void) {
    if (!lc_me_busy) return;
    if (me_shared_uncached[1] != ME_STAT_DONE) return;
    if (me_shared_uncached[0] != ME_CMD_NONE) return;

    int di = lc_decode;
    lc_n_blocks[di] = (int)me_shared_uncached[41];
    /* SC dcache無効化 — MEが書いたPCM/visを読めるようにする */
    sceKernelDcacheWritebackInvalidateAll();
    lc_ready[di] = 1;
    lc_me_busy = 0;
    unsigned int elapsed_ms = (sceKernelGetSystemTimeLow() - lc_decode_start_us) / 1000;
    log_int("lc_decode_done_ms", (int)elapsed_ms);
    log_int("lc_decode_blocks", lc_n_blocks[di]);
    log_int("opna_sample_rate", (int)me_shared_uncached[52]);

    /* 次バッファのデコードを自動開始 (ダブルバッファ ストリーミング) */
    int next_di = 1 - di;
    if (!lc_ready[next_di]) {
        lc_decode = next_di;
        lc_ready[next_di] = 0;
        lc_n_blocks[next_di] = 0;
        lc_decode_pending = 0;
        me_shared_uncached[2] = (unsigned int)lc_pcm[next_di] & 0x1FFFFFFF;
        me_shared_uncached[3] = (unsigned int)PSP_RENDER_SAMPLES;
        me_shared_uncached[4] = 0;
        me_shared_uncached[7] = (unsigned int)lc_vis[next_di] & 0x1FFFFFFF;
        me_shared_uncached[57] = lc_fmp[next_di] ? ((unsigned int)lc_fmp[next_di] & 0x1FFFFFFF) : 0;
        me_shared_uncached[40] = lc_actual_max_blocks;
        me_shared_uncached[41] = 0;
        me_shared_uncached[42] = 0;
        me_shared_uncached[43] = 0;
        me_shared_uncached[1] = ME_STAT_IDLE;
        asm volatile("sync");
        me_shared_uncached[0] = ME_CMD_DECODE_LOOP;
        lc_me_busy = 1;
        lc_decode_start_us = sceKernelGetSystemTimeLow();
    }
}

static void lc_start_next_decode(void) {
    if (lc_me_busy || !lc_decode_pending) return;
    int di = 1 - lc_play; /* 再生中でない方に書く */
    lc_decode = di;
    lc_ready[di] = 0;
    lc_n_blocks[di] = 0;
    lc_decode_pending = 0;

    me_shared_uncached[2] = (unsigned int)lc_pcm[di] & 0x1FFFFFFF;
    me_shared_uncached[3] = (unsigned int)PSP_RENDER_SAMPLES;
    me_shared_uncached[4] = 0;
    me_shared_uncached[7] = (unsigned int)lc_vis[di] & 0x1FFFFFFF;
    me_shared_uncached[57] = lc_fmp[di] ? ((unsigned int)lc_fmp[di] & 0x1FFFFFFF) : 0;
    me_shared_uncached[40] = lc_actual_max_blocks;
    me_shared_uncached[41] = 0;
    me_shared_uncached[42] = 0;
    me_shared_uncached[43] = 0;
    me_shared_uncached[1] = ME_STAT_IDLE;
    asm volatile("sync");
    me_shared_uncached[0] = ME_CMD_DECODE_LOOP;
    lc_me_busy = 1;
    lc_decode_start_us = sceKernelGetSystemTimeLow();
}

static int audio_producer_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int rb = 0; /* render_buf ダブルバッファインデックス */

    while (audio_alive) {
        if (audio_paused || app_state == APP_STATE_LIST) {
            sceKernelDelayThread(5000);
            continue;
        }

        /* ループキャッシュ: バックグラウンドデコード管理 */
        if (lc_enabled && me_active) {
            lc_check_decode_done();
            lc_start_next_decode();
        }

        /* 定期ステータスログ (毎秒) */
        {
            static unsigned int last_status_us = 0;
            unsigned int now_us = sceKernelGetSystemTimeLow();
            if (now_us - last_status_us > 1000000) {
                last_status_us = now_us;
                char sb[128];
                int me_prog = lc_me_busy ? (int)me_shared_uncached[41] : -1;
                static int prev_me_prog = 0;
                int me_spd = me_prog - prev_me_prog;
                prev_me_prog = me_prog;
                snprintf(sb, sizeof(sb),
                    "STAT r=%d p=%d me=%d spd=%d bi=%d rd=%d/%d sm=0x%03X me_sm=0x%03X me_sb=%u",
                    ring_count(), lc_play_pos, me_prog, me_spd,
                    lc_play, lc_ready[0], lc_ready[1],
                    solo_mask_sent, (unsigned int)me_shared_uncached[54],
                    (unsigned int)me_shared_uncached[55]);
                log_write(sb);
            }
        }

        /* ソロ中はリング充填を制限 (マスク変更の即時反映のため) */
        int ring_cap_active = (solo_mask_sent != 0x7FF);
        int ring_ok = ring_cap_active ? (ring_count() < 8) : (ring_free() > 0);

        if (ring_ok) {
            unsigned int t0 = sceKernelGetSystemTimeLow();

            if (lc_enabled && me_active) {
                /* ===== ループキャッシュ ストリーミング読み出し ===== */
                int bi = lc_play;
                int pos = lc_play_pos;

                /* 読み出し可能な上限を決定:
                 * - 完了済みバッファ(lc_ready): n_blocksまで全部読める
                 * - デコード中バッファ(lc_decode==bi): ME進捗まで読める */
                int readable;
                if (lc_ready[bi]) {
                    readable = lc_n_blocks[bi];
                } else if (lc_decode == bi && lc_me_busy) {
                    /* MEデコード中 — 進捗から安全マージン分手前まで読む
                     * マージン1だとMEに張り付いてdcache invalidate高頻度
                     * → cache thrashingでME速度低下の可能性 */
                    #define LC_SAFETY_MARGIN 1
                    int progress = (int)me_shared_uncached[41];
                    readable = (progress > LC_SAFETY_MARGIN) ? progress - LC_SAFETY_MARGIN : 0;
                    /* SC dcache無効化 — MEが書いたデータを読むため */
                    if (readable > pos) {
                        sceKernelDcacheInvalidateRange(
                            (void *)&lc_pcm[bi][pos * PSP_RENDER_SAMPLES * 2],
                            (readable - pos) * PSP_RENDER_SAMPLES * 2 * sizeof(short));
                        sceKernelDcacheInvalidateRange(
                            (void *)&lc_vis[bi][pos * VIS_SIZE],
                            (readable - pos) * VIS_SIZE * sizeof(unsigned int));
                        if (lc_fmp[bi]) sceKernelDcacheInvalidateRange(
                            (void *)&lc_fmp[bi][pos * FMP_SIZE],
                            (readable - pos) * FMP_SIZE * sizeof(unsigned int));
                    }
                } else {
                    readable = lc_n_blocks[bi];
                }

                /* デコード早期開始: 次バッファが未デコードなら先行してMEに投げる。
                 * 消費位置は変えない (スキップ防止) */
                if (!lc_me_busy && !lc_decode_pending) {
                    int next = 1 - bi;
                    if (!lc_ready[next]) {
                        lc_decode_pending = 1;
                    }
                }

                if (pos < readable) {
                    memcpy(render_buf[rb],
                           &lc_pcm[bi][pos * PSP_RENDER_SAMPLES * 2],
                           PSP_RENDER_SAMPLES * 2 * sizeof(short));
                    process_upsample(render_buf[rb], ring_buf[ring_wr],
                                     PSP_RENDER_SAMPLES);
                    for (int vi = 0; vi < VIS_SIZE; vi++)
                        ring_vis[ring_wr][vi] = lc_vis[bi][pos * VIS_SIZE + vi];
                    if (ring_fmp && lc_fmp[bi]) {
                        for (int fi = 0; fi < FMP_SIZE; fi++)
                            ring_fmp[ring_wr][fi] = lc_fmp[bi][pos * FMP_SIZE + fi];
                    }

                    ring_wr = (ring_wr + 1) % ring_blocks;
                    rb = 1 - rb;
                    lc_play_pos = pos + 1;
                    /* SC再生位置をMEに通知 (ソロgapスロットル用) */
                    me_shared_uncached[56] = (unsigned int)lc_play_pos;

                    /* キャッシュ末端到達 → 次バッファ or ループ再生 */
                    if (lc_play_pos >= lc_n_blocks[bi] && lc_ready[bi]) {
                        int next = 1 - bi;
                        if (lc_ready[next] || (lc_decode == next && lc_me_busy)) {
                            /* 次バッファready or デコード中 → 切り替え (ストリーミング読み出し) */
                            log_int("RW_switch_bi", bi);
                            log_int("RW_switch_ring", ring_count());
                            lc_play = next;
                            lc_play_pos = 0;
                            lc_ready[bi] = 0;
                            lc_decode_pending = 1;
                        } else {
                            /* 次バッファ未開始 → 現バッファをループ再生 */
                            log_int("RW_loop_pos", pos);
                            log_int("RW_loop_nblk", lc_n_blocks[bi]);
                            log_int("RW_loop_ring", ring_count());
                            lc_play_pos = 0;
                        }
                    }
                } else {
                    /* 読み出し待ち or バッファ切替 or ループ再生 */
                    if (pos >= lc_n_blocks[bi] && lc_ready[bi]) {
                        int next = 1 - bi;
                        if (lc_ready[next] || (lc_decode == next && lc_me_busy)) {
                            log_int("RW_switch2_bi", bi);
                            log_int("RW_switch2_ring", ring_count());
                            lc_play = next;
                            lc_play_pos = 0;
                            lc_ready[bi] = 0;
                            lc_decode_pending = 1;
                        } else {
                            /* 次バッファ未開始 → 現バッファをループ再生 */
                            log_int("RW_loop2_pos", pos);
                            log_int("RW_loop2_nblk", lc_n_blocks[bi]);
                            log_int("RW_loop2_ring", ring_count());
                            lc_play_pos = 0;
                        }
                    } else {
                        /* MEの進捗待ち */
                        sceKernelDelayThread(500);
                    }
                }
            } else {
                /* ===== 従来方式: リアルタイムMEレンダリング ===== */
                me_render_start(render_buf[rb], PSP_RENDER_SAMPLES);
                me_render_wait(render_buf[rb], PSP_RENDER_SAMPLES);
                process_upsample(render_buf[rb], ring_buf[ring_wr],
                                 PSP_RENDER_SAMPLES);
                if (me_active) {
                    for (int vi = 0; vi < VIS_SIZE; vi++)
                        ring_vis[ring_wr][vi] = me_shared_uncached[8 + vi];
                    if (ring_fmp) {
                        for (int fi = 0; fi < FMP_SIZE; fi++)
                            ring_fmp[ring_wr][fi] = me_shared_uncached[FMP_SH_BASE + fi];
                    }
                }
                ring_wr = (ring_wr + 1) % ring_blocks;
                rb = 1 - rb;
            }

            unsigned int t1 = sceKernelGetSystemTimeLow();
            unsigned int elapsed = t1 - t0;
            prod_render_us = elapsed;
            if (elapsed > prod_max_us) prod_max_us = elapsed;

            /* RAMログ (I/Oなし) */
            if (tlog_idx < TLOG_MAX) {
                tlog_render[tlog_idx] = (elapsed > 65535) ? 65535 : (unsigned short)elapsed;
                tlog_ring[tlog_idx] = (unsigned char)ring_count();
                tlog_idx++;
            }
        } else {
            sceKernelDelayThread(3000);
        }

        /* underrun検出 */
        if (ring_count() == 0 && !audio_paused && app_state == APP_STATE_PLAY) {
            prod_underrun_cnt++;
        }
    }
    sceKernelExitDeleteThread(0);
    return 0;
}

/* ============================================================
 *  FFT (256点)
 * ============================================================ */
#define FFT_N 256
static int   bit_rev[FFT_N];
static float fft_cos_tbl[FFT_N / 2];
static float fft_sin_tbl[FFT_N / 2];

static void fft_init(void) {
    for (int i = 0; i < FFT_N; i++) {
        int j = 0, x = i;
        for (int b = 0; b < 8; b++) { j = (j << 1) | (x & 1); x >>= 1; }
        bit_rev[i] = j;
    }
    for (int i = 0; i < FFT_N / 2; i++) {
        float angle = -2.0f * PI * (float)i / (float)FFT_N;
        fft_cos_tbl[i] = cosf(angle);
        fft_sin_tbl[i] = sinf(angle);
    }
}

static void fft_compute(float *re, float *im) {
    float tr[FFT_N], ti[FFT_N];
    for (int i = 0; i < FFT_N; i++) { tr[i] = re[bit_rev[i]]; ti[i] = im[bit_rev[i]]; }
    memcpy(re, tr, sizeof(float) * FFT_N);
    memcpy(im, ti, sizeof(float) * FFT_N);
    for (int s = 1; s <= 8; s++) {
        int m = 1 << s, half = m >> 1, step = FFT_N / m;
        for (int k = 0; k < FFT_N; k += m) {
            for (int j = 0; j < half; j++) {
                int tw = j * step;
                float wr = fft_cos_tbl[tw], wi = fft_sin_tbl[tw];
                float xr = wr * re[k+j+half] - wi * im[k+j+half];
                float xi = wr * im[k+j+half] + wi * re[k+j+half];
                float ur = re[k+j], ui = im[k+j];
                re[k+j] = ur + xr; im[k+j] = ui + xi;
                re[k+j+half] = ur - xr; im[k+j+half] = ui - xi;
            }
        }
    }
}

#define NUM_BANDS 32
static float spectrum[NUM_BANDS];
static float spectrum_smooth[NUM_BANDS];
static float rms_l, rms_r;

#define BEAT_HISTORY 16
static float energy_history[BEAT_HISTORY];
static int   energy_idx = 0;
static float beat_value = 0.0f;

static float hann_window[FFT_N];
static void init_hann(void) {
    for (int i = 0; i < FFT_N; i++)
        hann_window[i] = 0.5f * (1.0f - cosf(2.0f * PI * (float)i / (float)(FFT_N - 1)));
}

static void do_analysis(void) {
    if (!analysis_ready) return;
    float re[FFT_N], im[FFT_N];
    float sum_l = 0, sum_r = 0, energy = 0;
    for (int i = 0; i < FFT_N; i++) {
        float mono = (analysis_l[i] + analysis_r[i]) * 0.5f;
        re[i] = mono * hann_window[i]; im[i] = 0.0f;
        sum_l += analysis_l[i] * analysis_l[i];
        sum_r += analysis_r[i] * analysis_r[i];
        energy += mono * mono;
    }
    rms_l = sqrtf(sum_l / FFT_N); rms_r = sqrtf(sum_r / FFT_N);
    fft_compute(re, im);
    float mag[FFT_N / 2];
    for (int i = 0; i < FFT_N / 2; i++)
        mag[i] = sqrtf(re[i]*re[i] + im[i]*im[i]) / (float)FFT_N;
    for (int b = 0; b < NUM_BANDS; b++) {
        float log_min = logf(1.0f), log_max = logf((float)(FFT_N / 2));
        float log0 = log_min + (log_max - log_min) * (float)b / NUM_BANDS;
        float log1 = log_min + (log_max - log_min) * (float)(b + 1) / NUM_BANDS;
        int bin0 = (int)expf(log0), bin1 = (int)expf(log1);
        if (bin0 < 1) bin0 = 1; if (bin1 <= bin0) bin1 = bin0 + 1;
        if (bin1 > FFT_N / 2) bin1 = FFT_N / 2;
        float sum = 0; int cnt = 0;
        for (int i = bin0; i < bin1; i++) { sum += mag[i]; cnt++; }
        spectrum[b] = (cnt > 0) ? sum / cnt : 0;
    }
    for (int b = 0; b < NUM_BANDS; b++) {
        float t = spectrum[b];
        spectrum_smooth[b] = (t > spectrum_smooth[b]) ? t : spectrum_smooth[b] * 0.85f + t * 0.15f;
    }
    float cur_energy = energy / FFT_N;
    energy_history[energy_idx] = cur_energy;
    energy_idx = (energy_idx + 1) % BEAT_HISTORY;
    float avg_e = 0;
    for (int i = 0; i < BEAT_HISTORY; i++) avg_e += energy_history[i];
    avg_e /= BEAT_HISTORY;
    beat_value = (avg_e > 0.00001f) ? (cur_energy / avg_e) - 1.0f : 0.0f;
    if (beat_value < 0) beat_value = 0;
    analysis_ready = 0;
}

/* ============================================================
 *  MODE 0: WAVEFORM
 * ============================================================ */
static void draw_waveform(void) {
    float cx = SCREEN_W / 2.0f;
    float cy_l = SCREEN_H * 0.35f, cy_r = SCREEN_H * 0.65f;
    float w = SCREEN_W * 0.88f;
    float amp = 80.0f;
    set_additive_blend();
    for (int i = 0; i < ANALYSIS_SIZE - 1; i++) {
        float x0 = cx - w/2 + w * (float)i / (ANALYSIS_SIZE-1);
        float x1 = cx - w/2 + w * (float)(i+1) / (ANALYSIS_SIZE-1);
        float y0 = cy_l - analysis_l[i] * amp;
        float y1 = cy_l - analysis_l[i+1] * amp;
        unsigned int c = make_rgba(0, 200, 255, 200);
        emit_quad(x0, y0-1, x1, y1-1, x1, y1+1, x0, y0+1, c, c, c, c);
    }
    for (int i = 0; i < ANALYSIS_SIZE - 1; i++) {
        float x0 = cx - w/2 + w * (float)i / (ANALYSIS_SIZE-1);
        float x1 = cx - w/2 + w * (float)(i+1) / (ANALYSIS_SIZE-1);
        float y0 = cy_r - analysis_r[i] * amp;
        float y1 = cy_r - analysis_r[i+1] * amp;
        unsigned int c = make_rgba(255, 50, 200, 200);
        emit_quad(x0, y0-1, x1, y1-1, x1, y1+1, x0, y0+1, c, c, c, c);
    }
    set_alpha_blend();
    float bh_l = rms_l * 160; if (bh_l > 100) bh_l = 100;
    float bh_r = rms_r * 160; if (bh_r > 100) bh_r = 100;
    emit_rect(6, SCREEN_H/2 - bh_l/2, 4, bh_l, make_rgba(0, 200, 255, 180));
    emit_rect(SCREEN_W-10, SCREEN_H/2 - bh_r/2, 4, bh_r, make_rgba(255, 50, 200, 180));
}

/* ============================================================
 *  MODE 1: SPECTRUM
 * ============================================================ */
static void draw_spectrum(void) {
    float margin = 10.0f;
    float total_w = SCREEN_W - margin * 2;
    float bar_w = total_w / NUM_BANDS - 1.0f;
    float gap = 1.0f;
    float base_y = SCREEN_H * 0.85f;
    float max_h = SCREEN_H * 0.7f;
    set_alpha_blend();
    for (int b = 0; b < NUM_BANDS; b++) {
        float val = spectrum_smooth[b];
        float db = 20.0f * log10f(val + 0.00001f);
        float norm = (db + 60.0f) / 60.0f;
        if (norm < 0) norm = 0; if (norm > 1) norm = 1;
        float h = norm * max_h;
        float x = margin + b * (bar_w + gap);
        float hue = 200.0f + (float)b / NUM_BANDS * 160.0f;
        if (hue >= 360.0f) hue -= 360.0f;
        unsigned int c_top = hsv_to_rgba(hue, 0.8f, 1.0f, 230);
        unsigned int c_bot = hsv_to_rgba(hue, 0.9f, 0.4f, 200);
        emit_quad(x, base_y-h, x+bar_w, base_y-h, x+bar_w, base_y, x, base_y,
                  c_top, c_top, c_bot, c_bot);
        unsigned int c_ref = hsv_to_rgba(hue, 0.6f, 0.3f, 60);
        unsigned int c_zero = hsv_to_rgba(hue, 0.6f, 0.3f, 0);
        float ref_h = h * 0.3f;
        emit_quad(x, base_y, x+bar_w, base_y, x+bar_w, base_y+ref_h, x, base_y+ref_h,
                  c_ref, c_ref, c_zero, c_zero);
    }
}

/* ============================================================
 *  MODE 2: CIRCLE
 * ============================================================ */
static void draw_circle_vis(void) {
    float cx = SCREEN_W / 2.0f, cy = SCREEN_H / 2.0f;
    float base_r = 50.0f;
    set_additive_blend();
    int n = NUM_BANDS * 2;
    for (int i = 0; i < n; i++) {
        float a0 = 2.0f * PI * (float)i / n - PI / 2.0f;
        float a1 = 2.0f * PI * (float)(i+1) / n - PI / 2.0f;
        int bi = (i < NUM_BANDS) ? i : (n - 1 - i);
        float val = spectrum_smooth[bi];
        float db = 20.0f * log10f(val + 0.00001f);
        float norm = (db + 60.0f) / 60.0f;
        if (norm < 0) norm = 0; if (norm > 1) norm = 1;
        float ext = norm * 70.0f;
        float r0 = base_r, r1 = base_r + ext;
        float hue = (float)bi / NUM_BANDS * 270.0f;
        unsigned int ci = hsv_to_rgba(hue, 0.7f, 0.5f, 150);
        unsigned int co = hsv_to_rgba(hue, 0.9f, 1.0f, (int)(200*norm));
        emit_quad(cx+cosf(a0)*r0, cy+sinf(a0)*r0,
                  cx+cosf(a1)*r0, cy+sinf(a1)*r0,
                  cx+cosf(a1)*r1, cy+sinf(a1)*r1,
                  cx+cosf(a0)*r1, cy+sinf(a0)*r1,
                  ci, ci, co, co);
    }
    float rms_avg = (rms_l + rms_r) * 0.5f;
    float gr = base_r * 0.8f + rms_avg * 80.0f;
    if (gr > base_r) gr = base_r;
    int ga = (int)(rms_avg * 400); if (ga > 150) ga = 150;
    emit_circle(cx, cy, make_rgba(100, 150, 255, ga), make_rgba(100, 150, 255, 0), gr, 16);
}

/* ============================================================
 *  MODE 3: PARTICLE
 * ============================================================ */
#define MAX_PARTICLES 100
typedef struct { float x, y, vx, vy, life, max_life, size, hue; } Particle;
static Particle particles[MAX_PARTICLES];
static int particles_inited = 0;
static unsigned int rng_state = 12345;
static float randf(void) {
    rng_state = rng_state * 1103515245 + 12345;
    return (float)(rng_state >> 16 & 0x7FFF) / 32767.0f;
}
static void emit_particle(int idx) {
    Particle *p = &particles[idx];
    float angle = randf() * 2.0f * PI;
    float speed = 1.0f + randf() * 3.0f;
    p->x = SCREEN_W / 2.0f; p->y = SCREEN_H / 2.0f;
    p->vx = cosf(angle) * speed; p->vy = sinf(angle) * speed;
    p->life = 0; p->max_life = 40.0f + randf() * 80.0f;
    p->size = 2.0f + randf() * 3.0f; p->hue = randf() * 360.0f;
}
static void draw_particles(void) {
    if (!particles_inited) {
        for (int i = 0; i < MAX_PARTICLES; i++) {
            emit_particle(i);
            particles[i].life = randf() * particles[i].max_life;
        }
        particles_inited = 1;
    }
    int emit_count = (int)(beat_value * 10.0f);
    if (emit_count > 15) emit_count = 15;
    set_additive_blend();
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &particles[i];
        if (emit_count > 0 && p->life >= p->max_life) {
            emit_particle(i);
            p->vx *= 1.0f + beat_value * 3.0f;
            p->vy *= 1.0f + beat_value * 3.0f;
            emit_count--;
        }
        p->x += p->vx; p->y += p->vy;
        p->vx *= 0.98f; p->vy *= 0.98f;
        p->life += 1.0f;
        if (p->life >= p->max_life) continue;
        float t = p->life / p->max_life;
        float alpha = (1.0f - t) * 200.0f; if (alpha < 0) alpha = 0;
        float sz = p->size * (1.0f + t * 2.0f);
        float bright = 0.5f + (rms_l + rms_r) * 2.0f; if (bright > 1.0f) bright = 1.0f;
        emit_rect(p->x - sz, p->y - sz, sz*2, sz*2,
                  hsv_to_rgba(p->hue + t * 60.0f, 0.8f, bright, (int)alpha));
    }
    float rms_avg = (rms_l + rms_r) * 0.5f;
    float gr = 20.0f + rms_avg * 60.0f + beat_value * 30.0f; if (gr > 80) gr = 80;
    int ga = (int)(rms_avg * 300 + beat_value * 100); if (ga > 200) ga = 200;
    emit_circle(SCREEN_W/2, SCREEN_H/2,
                hsv_to_rgba(180+beat_value*60, 0.6f, 1.0f, ga),
                hsv_to_rgba(180+beat_value*60, 0.6f, 1.0f, 0), gr, 12);
}

/* ============================================================
 *  簡易線描画フォント
 * ============================================================ */
typedef struct { signed char x0,y0,x1,y1; } FontSeg;
#define FS_END {-1,-1,-1,-1}
static const FontSeg seg_font[][7] = {
    /* A-Z */
    {{0,4,0,1},{0,1,2,0},{2,0,4,1},{4,1,4,4},{0,2,4,2},FS_END,FS_END},
    {{0,0,0,4},{0,0,3,0},{3,0,4,1},{4,1,3,2},{0,2,3,2},{3,2,4,3},{4,3,0,4}},
    {{4,0,0,0},{0,0,0,4},{0,4,4,4},FS_END,FS_END,FS_END,FS_END},
    {{0,0,0,4},{0,0,3,0},{3,0,4,2},{4,2,3,4},{3,4,0,4},FS_END,FS_END},
    {{4,0,0,0},{0,0,0,4},{0,4,4,4},{0,2,3,2},FS_END,FS_END,FS_END},
    {{4,0,0,0},{0,0,0,4},{0,2,3,2},FS_END,FS_END,FS_END,FS_END},
    {{4,0,0,0},{0,0,0,4},{0,4,4,4},{4,4,4,2},{4,2,2,2},FS_END,FS_END},
    {{0,0,0,4},{4,0,4,4},{0,2,4,2},FS_END,FS_END,FS_END,FS_END},
    {{1,0,3,0},{2,0,2,4},{1,4,3,4},FS_END,FS_END,FS_END,FS_END},
    {{1,0,4,0},{3,0,3,4},{3,4,0,4},{0,4,0,3},FS_END,FS_END,FS_END},
    {{0,0,0,4},{4,0,0,2},{0,2,4,4},FS_END,FS_END,FS_END,FS_END},
    {{0,0,0,4},{0,4,4,4},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{0,4,0,0},{0,0,2,2},{2,2,4,0},{4,0,4,4},FS_END,FS_END,FS_END},
    {{0,4,0,0},{0,0,4,4},{4,4,4,0},FS_END,FS_END,FS_END,FS_END},
    {{0,0,4,0},{4,0,4,4},{4,4,0,4},{0,4,0,0},FS_END,FS_END,FS_END},
    {{0,4,0,0},{0,0,4,0},{4,0,4,2},{4,2,0,2},FS_END,FS_END,FS_END},
    {{0,0,4,0},{4,0,4,4},{4,4,0,4},{0,4,0,0},{3,3,4,4},FS_END,FS_END},
    {{0,4,0,0},{0,0,4,0},{4,0,4,2},{4,2,0,2},{0,2,4,4},FS_END,FS_END},
    {{4,0,0,0},{0,0,0,2},{0,2,4,2},{4,2,4,4},{4,4,0,4},FS_END,FS_END},
    {{0,0,4,0},{2,0,2,4},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{0,0,0,4},{0,4,4,4},{4,4,4,0},FS_END,FS_END,FS_END,FS_END},
    {{0,0,2,4},{2,4,4,0},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{0,0,0,4},{0,4,2,2},{2,2,4,4},{4,4,4,0},FS_END,FS_END,FS_END},
    {{0,0,4,4},{4,0,0,4},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{0,0,2,2},{4,0,2,2},{2,2,2,4},FS_END,FS_END,FS_END,FS_END},
    {{0,0,4,0},{4,0,0,4},{0,4,4,4},FS_END,FS_END,FS_END,FS_END},
    /* 0-9 */
    {{0,0,4,0},{4,0,4,4},{4,4,0,4},{0,4,0,0},{0,4,4,0},FS_END,FS_END},
    {{1,1,2,0},{2,0,2,4},{1,4,3,4},FS_END,FS_END,FS_END,FS_END},
    {{0,0,4,0},{4,0,4,2},{4,2,0,2},{0,2,0,4},{0,4,4,4},FS_END,FS_END},
    {{0,0,4,0},{4,0,4,4},{4,4,0,4},{0,2,4,2},FS_END,FS_END,FS_END},
    {{0,0,0,2},{0,2,4,2},{4,0,4,4},FS_END,FS_END,FS_END,FS_END},
    {{4,0,0,0},{0,0,0,2},{0,2,4,2},{4,2,4,4},{4,4,0,4},FS_END,FS_END},
    {{4,0,0,0},{0,0,0,4},{0,4,4,4},{4,4,4,2},{4,2,0,2},FS_END,FS_END},
    {{0,0,4,0},{4,0,4,4},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{0,0,4,0},{4,0,4,4},{4,4,0,4},{0,4,0,0},{0,2,4,2},FS_END,FS_END},
    {{0,2,4,2},{4,2,4,4},{0,0,4,0},{4,0,0,0},{0,0,0,2},FS_END,FS_END},
    /* space : + - . / < > */
    {FS_END,FS_END,FS_END,FS_END,FS_END,FS_END,FS_END},
    {{2,1,2,1},{2,3,2,3},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{2,1,2,3},{1,2,3,2},FS_END,FS_END,FS_END,FS_END,FS_END},
    {{1,2,3,2},FS_END,FS_END,FS_END,FS_END,FS_END,FS_END},
    {{2,4,2,4},FS_END,FS_END,FS_END,FS_END,FS_END,FS_END}, /* . */
    {{1,1,3,3},FS_END,FS_END,FS_END,FS_END,FS_END,FS_END}, /* / */
    {{3,0,1,2},{1,2,3,4},FS_END,FS_END,FS_END,FS_END,FS_END}, /* < */
    {{1,0,3,2},{3,2,1,4},FS_END,FS_END,FS_END,FS_END,FS_END}, /* > */
};
static int seg_char_index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    if (c == ' ') return 36;
    if (c == ':') return 37;
    if (c == '+') return 38;
    if (c == '-') return 39;
    if (c == '.') return 40;
    if (c == '/') return 41;
    if (c == '<') return 42;
    if (c == '>') return 43;
    return 36;
}
static void draw_seg_string(float x, float y, float char_w, float char_h, const char *str, unsigned int color) {
    float th = 0.8f;
    while (*str) {
        int idx = seg_char_index(*str);
        const FontSeg *segs = seg_font[idx];
        for (int s = 0; s < 7; s++) {
            if (segs[s].x0 < 0) break;
            float x0 = x + segs[s].x0 * char_w / 4.0f;
            float y0 = y + segs[s].y0 * char_h / 4.0f;
            float x1 = x + segs[s].x1 * char_w / 4.0f;
            float y1 = y + segs[s].y1 * char_h / 4.0f;
            float dx = x1 - x0, dy = y1 - y0;
            float len = sqrtf(dx*dx + dy*dy);
            if (len < 0.001f) {
                emit_quad(x0-th, y0-th, x0+th, y0-th, x0+th, y0+th, x0-th, y0+th,
                          color, color, color, color);
            } else {
                float nx = -dy/len*th, ny = dx/len*th;
                emit_quad(x0+nx, y0+ny, x0-nx, y0-ny, x1-nx, y1-ny, x1+nx, y1+ny,
                          color, color, color, color);
            }
        }
        x += char_w + 1.0f;
        str++;
    }
}

/* ============================================================
 *  共通定義 (ピアノロール / FMP共用)
 * ============================================================ */
#define KB_NUM_PARTS 11
static const char *part_names[KB_NUM_PARTS] = {
    "FM1","FM2","FM3","FM4","FM5","FM6","SSG1","SSG2","SSG3","ADPC","RHY"
};
static const float part_hues[KB_NUM_PARTS] = {
    200,210,220,230,240,250, 120,130,140, 30, 0
};
static const char *rhythm_names[6] = {"BD","SD","CYM","HH","TOM","RIM"};
static int prev_rdump[6] = {0,0,0,0,0,0};
static float rhy_bright[6] = {0,0,0,0,0,0};

/* ============================================================
 *  MODE 5: FMP-STYLE CHANNEL STATUS
 * ============================================================ */
static const char *note_names[12] = {"C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"};

static void draw_fmp_view(void) {
    set_alpha_blend();

    /* visデータ取得 */
    int notes[KB_NUM_PARTS];
    int voicenum[KB_NUM_PARTS], vol[KB_NUM_PARTS], det[KB_NUM_PARTS];
    unsigned int freq[KB_NUM_PARTS];
    int rdump[6];
    if (me_active) {
        for (int i = 0; i < 6; i++)
            rdump[i] = (int)(vis_current[i] + vis_current[6+i]);
        for (int i = 0; i < KB_NUM_PARTS; i++) {
            notes[i] = (vis_current[12+i] == 0xFFFFFFFF) ? -1 : (int)vis_current[12+i];
            voicenum[i] = (int)fmp_current[i];       /* voicenum[0..10] */
            vol[i] = (int)fmp_current[11+i];          /* volume[11..21] */
            det[i] = (int)(short)fmp_current[22+i];   /* detune[22..32] signed */
            freq[i] = fmp_current[33+i];               /* fnum[33..43] */
        }
    } else {
        pmd_get_current_notes(notes, KB_NUM_PARTS);
        pmd_get_rhythm_dumps(rdump);
        for (int i = 0; i < KB_NUM_PARTS; i++) {
            voicenum[i] = 0; vol[i] = 0; det[i] = 0; freq[i] = 0;
        }
    }
    /* リズムブライトネス更新 */
    for (int i = 0; i < 6; i++) {
        if (rdump[i] != prev_rdump[i]) { rhy_bright[i] = 1.0f; prev_rdump[i] = rdump[i]; }
        else rhy_bright[i] *= 0.90f;
    }

    float row_h = 20.0f;
    float start_y = 6.0f;
    float col_x[] = { 4.0f, 42.0f, 88.0f, 138.0f, 195.0f, 272.0f, 380.0f };
    /* col: PART  TONE  VOL  KEY  DET  FREQ  STATUS */

    /* ヘッダ行 */
    unsigned int hdr_col = make_rgba(120, 140, 180, 200);
    emit_rect(0, 0, SCREEN_W, start_y + row_h - 2, make_rgba(10, 10, 30, 200));
    flush_draw();
    draw_text(col_x[0], 2, 0.45f, "PART", hdr_col);
    draw_text(col_x[1], 2, 0.45f, "TONE", hdr_col);
    draw_text(col_x[2], 2, 0.45f, "VOL", hdr_col);
    draw_text(col_x[3], 2, 0.45f, "KEY", hdr_col);
    draw_text(col_x[4], 2, 0.45f, "DET", hdr_col);
    draw_text(col_x[5], 2, 0.45f, "FREQ", hdr_col);
    draw_text(col_x[6], 2, 0.45f, "STATUS", hdr_col);

    /* 各パート行 */
    for (int p = 0; p < KB_NUM_PARTS; p++) {
        float y = start_y + row_h + p * row_h;
        int part_on = (solo_mask >> p) & 1;

        /* 背景 */
        float bg_v = part_on ? 0.08f : 0.03f;
        int bg_a = part_on ? 140 : 60;
        emit_rect(0, y, SCREEN_W, row_h - 1, hsv_to_rgba(part_hues[p], 0.3f, bg_v, bg_a));

        /* ソロカーソル枠 */
        if (p == solo_cursor) {
            unsigned int cc = make_rgba(255, 255, 80, 220);
            emit_rect(0, y - 1, SCREEN_W, 2, cc);
            emit_rect(0, y + row_h - 2, SCREEN_W, 2, cc);
            emit_rect(0, y, 3, row_h - 1, cc);
        }
    }
    flush_draw();

    /* テキスト描画 */
    for (int p = 0; p < KB_NUM_PARTS; p++) {
        float y = start_y + row_h + p * row_h;
        int part_on = (solo_mask >> p) & 1;
        unsigned int txt_col = part_on ? hsv_to_rgba(part_hues[p], 0.3f, 1.0f, 255)
                                       : make_rgba(60, 60, 60, 120);
        float ty = y + 3.0f;
        float sz = 0.45f;

        /* PART名 — カーソル行は ">" 付き */
        if (p == solo_cursor)
            draw_text(col_x[0] - 1, ty, sz, ">", make_rgba(255, 255, 80, 255));
        draw_text(col_x[0] + 8, ty, sz, part_names[p], txt_col);

        if (p == 10) {
            /* リズムパート — BD/SD/CYM/HH/TOM/RIM のアクティビティ表示 */
            char rbuf[32];
            snprintf(rbuf, sizeof(rbuf), "%s%s%s%s%s%s",
                rhy_bright[0]>0.3f?"BD ":".. ",
                rhy_bright[1]>0.3f?"SD ":".. ",
                rhy_bright[2]>0.3f?"CY ":".. ",
                rhy_bright[3]>0.3f?"HH ":".. ",
                rhy_bright[4]>0.3f?"TM ":".. ",
                rhy_bright[5]>0.3f?"RM":"  ");
            draw_text(col_x[1], ty, sz, rbuf, txt_col);
            continue;
        }

        /* TONE (@番号) */
        char tbuf[8];
        snprintf(tbuf, sizeof(tbuf), "@%d", voicenum[p]);
        draw_text(col_x[1], ty, sz, tbuf, txt_col);

        /* VOL */
        char vbuf[8];
        snprintf(vbuf, sizeof(vbuf), "%3d", vol[p]);
        draw_text(col_x[2], ty, sz, vbuf, txt_col);

        /* KEY (音階) */
        int note = notes[p];
        if (note >= 0 && note != 255) {
            int key = note & 0x0F;
            int oct = (note >> 4) & 0x0F;
            char kbuf[8];
            if (key < 12) {
                snprintf(kbuf, sizeof(kbuf), "%s%d", note_names[key], oct);
            } else {
                snprintf(kbuf, sizeof(kbuf), "?%d", oct);
            }
            unsigned int key_col = part_on ? make_rgba(255, 255, 255, 255) : txt_col;
            draw_text(col_x[3], ty, sz, kbuf, key_col);
        } else {
            draw_text(col_x[3], ty, sz, "---", make_rgba(80, 80, 80, 150));
        }

        /* DET (デチューン) */
        char dbuf[8];
        if (det[p] != 0)
            snprintf(dbuf, sizeof(dbuf), "%+d", det[p]);
        else
            snprintf(dbuf, sizeof(dbuf), "0");
        draw_text(col_x[4], ty, sz, dbuf, txt_col);

        /* FREQ (block/fnum) */
        char fbuf[12];
        {
            int blk = (freq[p] >> 11) & 7;
            int fn = freq[p] & 0x7FF;
            snprintf(fbuf, sizeof(fbuf), "%d/%04X", blk, fn);
        }
        draw_text(col_x[5], ty, sz, fbuf, txt_col);

        /* STATUS — keyon indicator */
        if (note >= 0 && note != 255 && part_on) {
            /* ボリュームバー */
            float bar_w = 80.0f * vol[p] / 128.0f;
            if (bar_w > 80.0f) bar_w = 80.0f;
            if (bar_w < 0) bar_w = 0;
            emit_rect(col_x[6], y + 3, bar_w, row_h - 7,
                      hsv_to_rgba(part_hues[p], 0.7f, 0.8f, 180));
            flush_draw();
            draw_text(col_x[6] + 2, ty, 0.4f, "ON", make_rgba(255, 255, 255, 200));
        } else {
            draw_text(col_x[6], ty, 0.4f, "---", make_rgba(60, 60, 60, 100));
        }
    }

    /* 曲名ガイド枠 */
    {
        float gy = SCREEN_H - 30;
        emit_rect(0, gy, SCREEN_W, 16, make_rgba(0,0,0,180));
        flush_draw();
        char title[80];
        snprintf(title, sizeof(title), "%d. %s", current_track+1, tracks[current_track].name);
        draw_text(6, gy, 0.5f, title, make_rgba(255,255,255,220));
    }
}

/* ============================================================
 *  MODE 4: KEYBOARD
 * ============================================================ */
static int is_black_key(int note) {
    return (note==1||note==3||note==6||note==8||note==10);
}
static int white_key_index(int note) {
    static const int wk[]={0,-1,1,-1,2,3,-1,4,-1,5,-1,6};
    if (note<0||note>11) return -1; return wk[note];
}

static void draw_keyboard(void) {
    float label_w = 36.0f;
    float kb_total_w = SCREEN_W - label_w - 10.0f;
    float kb_x = label_w + 4.0f;
    float start_y = 2.0f;
    float bottom_margin = 16.0f;
    int num_octaves = 8;
    int whites_per_oct = 7;
    int total_whites = num_octaves * whites_per_oct;
    static const int w2note[] = {0,2,4,5,7,9,11};

    set_alpha_blend();

    {
        float total_kb_h = SCREEN_H - start_y - bottom_margin;
        float row_h = total_kb_h / KB_NUM_PARTS;
        float wk_w = kb_total_w / total_whites;
        float wk_h = row_h - 2.0f;
        float bk_w = wk_w * 0.6f;
        float bk_h = wk_h * 0.55f;

        /* visデータ: output threadが再生時にvis_currentを更新 → 音と同期 */
        int notes[KB_NUM_PARTS];
        int rdump[6];
        if (me_active) {
            for (int i = 0; i < 6; i++)
                rdump[i] = (int)(vis_current[i] + vis_current[6+i]);
            for (int i = 0; i < KB_NUM_PARTS; i++)
                notes[i] = (vis_current[12+i] == 0xFFFFFFFF) ? -1 : (int)vis_current[12+i];
        } else {
            pmd_get_current_notes(notes, KB_NUM_PARTS);
            pmd_get_rhythm_dumps(rdump);
        }
        for (int i = 0; i < 6; i++) {
            if (rdump[i] != prev_rdump[i]) { rhy_bright[i] = 1.0f; prev_rdump[i] = rdump[i]; }
            else rhy_bright[i] *= 0.90f;
        }

        for (int p = 0; p < KB_NUM_PARTS; p++) {
            float y = start_y + p * row_h;
            int part_on = (solo_mask >> p) & 1;
            float lbl_sat = part_on ? 0.6f : 0.1f;
            float lbl_val = part_on ? 0.3f : 0.08f;
            int lbl_alpha = part_on ? 180 : 60;
            emit_rect(1, y, label_w-2, wk_h, hsv_to_rgba(part_hues[p], lbl_sat, lbl_val, lbl_alpha));
            /* ソロカーソル枠 */
            if (p == solo_cursor) {
                unsigned int cc = make_rgba(255, 255, 80, 220);
                emit_rect(0, y-1, 480, 1, cc);           /* 上辺 */
                emit_rect(0, y+wk_h, 480, 1, cc);        /* 下辺 */
            }
            if (p == 10) {
                float rgap = 3.0f;
                float ind_w = (kb_total_w - rgap*5) / 6.0f;
                for (int r = 0; r < 6; r++) {
                    float rx = kb_x + r * (ind_w + rgap);
                    float bright = rhy_bright[r];
                    static const float rhy_hues[6] = {0,50,180,120,280,30};
                    float rv = 0.15f+bright*0.85f;
                    int ra = (int)(80+bright*175);
                    emit_rect(rx, y, ind_w, wk_h,
                              hsv_to_rgba(rhy_hues[r], 0.5f, rv, ra));
                }
                continue;
            }
            int cur_note = notes[p];
            int cur_oct = -1, cur_key = -1;
            if (cur_note >= 0 && cur_note != 255) {
                cur_key = cur_note & 0x0F; cur_oct = (cur_note >> 4) & 0x0F;
                if (cur_key > 11) cur_key = -1;
            }
            unsigned int wc_off = part_on ? make_rgba(180, 180, 190, 200)
                                          : make_rgba(60, 60, 65, 100);
            for (int wi = 0; wi < total_whites; wi++) {
                float kx = kb_x + wi * wk_w;
                int oct = wi / whites_per_oct, w = wi % whites_per_oct;
                int note12 = w2note[w];
                int active = part_on && (cur_oct == oct && cur_key == note12);
                unsigned int wc = active ? hsv_to_rgba(part_hues[p], 0.8f, 1.0f, 255) : wc_off;
                emit_quad(kx+0.3f, y+0.3f, kx+wk_w-0.3f, y+0.3f,
                          kx+wk_w-0.3f, y+wk_h-0.3f, kx+0.3f, y+wk_h-0.3f, wc,wc,wc,wc);
            }
            for (int oct = 0; oct < num_octaves; oct++) {
                for (int n = 0; n < 12; n++) {
                    if (!is_black_key(n)) continue;
                    int wki = white_key_index(n-1); if (wki < 0) continue;
                    int wi = oct * whites_per_oct + wki;
                    float kx = kb_x + (wi+1) * wk_w - bk_w/2;
                    int active = part_on && (cur_oct == oct && cur_key == n);
                    unsigned int bc = active ? hsv_to_rgba(part_hues[p], 0.9f, 1.0f, 255)
                                             : (part_on ? make_rgba(20, 20, 30, 250)
                                                        : make_rgba(15, 15, 20, 120));
                    emit_quad(kx, y, kx+bk_w, y, kx+bk_w, y+bk_h, kx, y+bk_h, bc,bc,bc,bc);
                }
            }
        }
        /* ラベル — 全ジオメトリ後にまとめて描画 */
        flush_draw();
        for (int p = 0; p < KB_NUM_PARTS; p++) {
            float y = start_y + p * row_h;
            int p_on = (solo_mask >> p) & 1;
            unsigned int lbl_col = p_on ? hsv_to_rgba(part_hues[p], 0.4f, 1.0f, 255)
                                        : make_rgba(80, 80, 80, 120);
            draw_text(3.0f, y+(wk_h-12)/2.0f, 0.5f, part_names[p], lbl_col);
            if (p == 10) {
                float rgap = 3.0f;
                float ind_w = (kb_total_w - rgap*5) / 6.0f;
                for (int r = 0; r < 6; r++) {
                    float rx = kb_x + r * (ind_w + rgap);
                    draw_text(rx+2, y+(wk_h-10)/2.0f, 0.45f, rhythm_names[r],
                              make_rgba(255,255,255,(int)(100+rhy_bright[r]*155)));
                }
            }
        }
    }
}

/* ============================================================
 *  曲一覧UI
 * ============================================================ */
#define LIST_VISIBLE_ROWS 10
#define LIST_ROW_H  22.0f
#define LIST_TOP_Y  36.0f
#define LIST_LEFT_X 10.0f

static void draw_song_list(void) {
    set_alpha_blend();
    int total = num_tracks;
    emit_rect(0, 0, SCREEN_W, SCREEN_H, make_rgba(5, 5, 15, 255));

    /* タイトル + GPL notice Section 5d */
    { char pmd_label[48]; snprintf(pmd_label, sizeof(pmd_label), "PMD%d GPL3", num_tracks);
      draw_text(16.0f, 5.0f, 0.75f, pmd_label, make_rgba(255,255,255,255)); }

    if (total == 0) { draw_text(LIST_LEFT_X, LIST_TOP_Y+10, 0.8f, "No tracks", make_rgba(180,80,80,255)); flush_draw(); return; }

    if (list_cursor < list_scroll) list_scroll = list_cursor;
    if (list_cursor >= list_scroll + LIST_VISIBLE_ROWS) list_scroll = list_cursor - LIST_VISIBLE_ROWS + 1;
    if (list_scroll < 0) list_scroll = 0;
    int vis_end = list_scroll + LIST_VISIBLE_ROWS;
    if (vis_end > total) vis_end = total;

    for (int i = list_scroll; i < vis_end; i++) {
        float y = LIST_TOP_Y + (i - list_scroll) * LIST_ROW_H;
        int is_cur = (i == list_cursor);
        if (is_cur)
            emit_rect(LIST_LEFT_X-4, y-1, SCREEN_W-LIST_LEFT_X*2+8, LIST_ROW_H-2, make_rgba(50,80,180,180));
        else if (i%2==0)
            emit_rect(LIST_LEFT_X-4, y-1, SCREEN_W-LIST_LEFT_X*2+8, LIST_ROW_H-2, make_rgba(20,20,35,120));

        char numstr[8]; int np=0, num=i+1;
        if(num>=10) numstr[np++]='0'+(num/10);
        numstr[np++]='0'+(num%10); numstr[np++]='.'; numstr[np]='\0';
        draw_text(LIST_LEFT_X, y+2, 0.7f, numstr,
                  is_cur ? make_rgba(255,255,100,255) : make_rgba(100,110,140,200));

        const char *name = tracks[i].name;
        draw_text(LIST_LEFT_X+28, y+2, 0.7f, name,
                  is_cur ? make_rgba(255,255,255,255) : make_rgba(160,170,200,220));

        int is_playing = (app_state == APP_STATE_PLAY && i == current_track);
        if (is_playing)
            draw_text(SCREEN_W-55, y+2, 0.65f, "NOW", make_rgba(80,255,80,255));
    }

    if (total > LIST_VISIBLE_ROWS) {
        float sb_x = SCREEN_W - 10;
        float sb_h = LIST_VISIBLE_ROWS * LIST_ROW_H;
        float thumb_h = sb_h * LIST_VISIBLE_ROWS / total; if (thumb_h < 6) thumb_h = 6;
        float thumb_y = LIST_TOP_Y + (sb_h-thumb_h) * list_scroll / (total-LIST_VISIBLE_ROWS);
        emit_rect(sb_x, LIST_TOP_Y, 4, sb_h, make_rgba(40,40,60,100));
        emit_rect(sb_x, thumb_y, 4, thumb_h, make_rgba(100,140,220,200));
    }

    float gy = SCREEN_H - 16;
    emit_rect(0, gy-2, SCREEN_W, 18, make_rgba(0,0,0,160));
    draw_text(10, gy-3, 0.6f, "O:Play Sq:WAV X:Exit ST:ME", make_rgba(150,170,210,200));
    draw_text(SCREEN_W-60, gy-3, 0.6f, me_active ? "ME:ON" : "ME:OFF",
              me_active ? make_rgba(100,255,100,200) : make_rgba(255,100,100,200));
    flush_draw();
}

/* ============================================================
 *  HUD
 * ============================================================ */
static void draw_hud(void) {
    set_alpha_blend();
    unsigned int bg = make_rgba(0, 0, 0, 120);

    emit_rect(0, SCREEN_H-14, SCREEN_W, 14, bg);
    flush_draw(); /* ジオメトリ確定してからテキスト */
    char bot[64];
    snprintf(bot, sizeof(bot), "L< %d/%d >R  X:LIST",
             current_track+1, num_tracks);
    draw_text_center(SCREEN_W/2, SCREEN_H-14, 0.5f, bot, make_rgba(180,200,255,200));

    /* MEステータス表示 */
    const char *me_str = me_active ? "ME:ON" : "ME:OFF";
    draw_text(4, SCREEN_H-14, 0.5f, me_str, me_active ? make_rgba(100,255,100,200) : make_rgba(255,100,100,200));
    /* eDRAMステータス表示 */
    int edram_on = (me_active && me_shared_uncached[6] != 0 && me_shared_uncached[6] != 0xDEAD0000);
    const char *edram_str = edram_on ? "eDRAM:ON" : "eDRAM:OFF";
    draw_text(80, SCREEN_H-14, 0.5f, edram_str, edram_on ? make_rgba(100,255,100,200) : make_rgba(255,100,100,200));
}

/* ============================================================
 *  パッド
 * ============================================================ */

static int handle_pad(void) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);

    unsigned int pressed = pad.Buttons & ~prev_buttons; /* 新規押下 */
    prev_buttons = pad.Buttons;

    /* ×: リスト画面→アプリ終了、再生中→リストに戻る(再生続行) */
    if (pressed & PSP_CTRL_CROSS) {
        if (app_state == APP_STATE_PLAY) {
            app_state = APP_STATE_LIST;
            list_cursor = current_track;
            if (list_cursor < list_scroll || list_cursor >= list_scroll + LIST_VISIBLE_ROWS)
                list_scroll = list_cursor;
            return 1;
        } else {
            return 0; /* アプリ終了 */
        }
    }

    if (app_state == APP_STATE_LIST) {
        /* SELECT: メモステ検索 */
        if (pressed & PSP_CTRL_SELECT) {
            pmd_stop();
            audio_paused = 0;
            search_and_import();
            log_flush();
            { SceCtrlData ps; sceCtrlPeekBufferPositive(&ps, 1); prev_buttons = ps.Buttons; }
        }
        /* 曲が0の場合、検索ダイアログを自動表示 (1回だけ) */
        static int auto_search_shown = 0;
        if (num_tracks == 0 && !auto_search_shown) {
            auto_search_shown = 1;
            pspUtilityMsgDialogParams dialog;
            memset(&dialog, 0, sizeof(dialog));
            dialog.base.size = sizeof(dialog);
            dialog.base.language = PSP_SYSTEMPARAM_LANGUAGE_JAPANESE;
            dialog.base.buttonSwap = get_system_accept();
            dialog.base.graphicsThread = 0x11;
            dialog.base.accessThread = 0x13;
            dialog.base.fontThread = 0x12;
            dialog.base.soundThread = 0x10;
            dialog.mode = PSP_UTILITY_MSGDIALOG_MODE_TEXT;
            dialog.options = PSP_UTILITY_MSGDIALOG_OPTION_TEXT
                           | PSP_UTILITY_MSGDIALOG_OPTION_YESNO_BUTTONS;
            {
                int lang = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
                sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &lang);
                if (lang == PSP_SYSTEMPARAM_LANGUAGE_JAPANESE) {
                    snprintf(dialog.message, sizeof(dialog.message),
                             "\xE3\x82\xB9\xE3\x83\x88\xE3\x83\xAC\xE3\x83\xBC\xE3\x82\xB8\xE5\x86\x85\xE3\x81\x8B\xE3\x82\x89\n"
                             "\xE6\x9B\xB2\xE3\x82\x92\xE6\xA4\x9C\xE7\xB4\xA2\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x99\xE3\x81\x8B\xEF\xBC\x9F");
                } else {
                    snprintf(dialog.message, sizeof(dialog.message),
                             "No songs found.\n\nSearch storage for songs?");
                }
            }
            int confirmed = 0;
            if (sceUtilityMsgDialogInitStart(&dialog) >= 0) {
                int running = 1;
                while (running) {
                    sceGuStart(GU_DIRECT, gu_list);
                    sceGuClearColor(0xFF080205);
                    sceGuClear(GU_COLOR_BUFFER_BIT);
                    sceGuFinish();
                    sceGuSync(0, 0);
                    int status = sceUtilityMsgDialogGetStatus();
                    if (status == 2) {
                        sceUtilityMsgDialogUpdate(1);
                    } else if (status == 3) {
                        sceUtilityMsgDialogShutdownStart();
                    } else if (status == 4 || status == 0) {
                        running = 0;
                    }
                    sceDisplayWaitVblankStart();
                    sceGuSwapBuffers();
                }
                confirmed = (dialog.buttonPressed == PSP_UTILITY_MSGDIALOG_RESULT_YES);
                { SceCtrlData ps; sceCtrlPeekBufferPositive(&ps, 1); prev_buttons = ps.Buttons; }
            }
            if (confirmed) {
                search_and_import();
                log_flush();
            }
        }
        int total = num_tracks;
        if ((pressed & PSP_CTRL_UP) && total > 0) {
            list_cursor--; if (list_cursor < 0) list_cursor = total - 1;
        }
        if ((pressed & PSP_CTRL_DOWN) && total > 0) {
            list_cursor++; if (list_cursor >= total) list_cursor = 0;
        }
        if ((pressed & PSP_CTRL_LTRIGGER) && total > 0) {
            list_cursor -= LIST_VISIBLE_ROWS; if (list_cursor < 0) list_cursor = 0;
        }
        if ((pressed & PSP_CTRL_RTRIGGER) && total > 0) {
            list_cursor += LIST_VISIBLE_ROWS; if (list_cursor >= total) list_cursor = total - 1;
        }
        if ((pressed & PSP_CTRL_CIRCLE) && total > 0) {
            current_track = list_cursor;
            start_track(current_track);
            app_state = APP_STATE_PLAY;
        }
        /* START: ME ON/OFFトグル (警告ダイアログ付き) */
        if (pressed & PSP_CTRL_START) {
            if (me_active) {
                /* ME OFF — 警告ダイアログ (Yes/No) */
                pspUtilityMsgDialogParams dialog;
                memset(&dialog, 0, sizeof(dialog));
                dialog.base.size = sizeof(dialog);
                dialog.base.language = PSP_SYSTEMPARAM_LANGUAGE_JAPANESE;
                dialog.base.buttonSwap = get_system_accept();
                dialog.base.graphicsThread = 0x11;
                dialog.base.accessThread = 0x13;
                dialog.base.fontThread = 0x12;
                dialog.base.soundThread = 0x10;
                dialog.mode = PSP_UTILITY_MSGDIALOG_MODE_TEXT;
                dialog.options = PSP_UTILITY_MSGDIALOG_OPTION_TEXT
                               | PSP_UTILITY_MSGDIALOG_OPTION_YESNO_BUTTONS
                               | PSP_UTILITY_MSGDIALOG_OPTION_DEFAULT_NO;
                {
                    int lang = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
                    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &lang);
                    if (lang == PSP_SYSTEMPARAM_LANGUAGE_JAPANESE) {
                        snprintf(dialog.message, sizeof(dialog.message),
                                 "Media Engine\xE3\x82\x92\xE7\x84\xA1\xE5\x8A\xB9\xE3\x81\xAB\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x99\xE3\x81\x8B?\n\n"
                                 "Main CPU\xE3\x81\xAE\xE3\x81\xBF\xE3\x81\xA7\xE5\x8B\x95\xE4\xBD\x9C\n"
                                 "\xE9\x9F\xB3\xE3\x81\x8C\xE9\x81\x85\xE3\x81\x8F\xE3\x81\xAA\xE3\x82\x8A\xE3\x81\xBE\xE3\x81\x99\n"
                                 "\xE5\x86\x8D\xE7\x94\x9F\xE4\xB8\xAD\xE3\x81\xAF\xE3\x82\xB7\xE3\x82\xB9\xE3\x83\x86\xE3\x83\xA0\xE3\x81\x8C\n"
                                 "\xE4\xB8\x8D\xE5\xAE\x89\xE5\xAE\x9A\xE3\x81\xAB\xE3\x81\xAA\xE3\x82\x8A\xE3\x81\xBE\xE3\x81\x99");
                    } else {
                        snprintf(dialog.message, sizeof(dialog.message),
                                 "Disable Media Engine?\n\n"
                                 "Main CPU only mode\n"
                                 "Audio will slow down\n"
                                 "System may become\n"
                                 "unresponsive during playback");
                    }
                }
                int confirmed = 0;
                if (sceUtilityMsgDialogInitStart(&dialog) >= 0) {
                    int running = 1;
                    while (running) {
                        sceGuStart(GU_DIRECT, gu_list);
                        sceGuClearColor(0xFF080205);
                        sceGuClear(GU_COLOR_BUFFER_BIT);
                        sceGuFinish();
                        sceGuSync(0, 0);
                        int status = sceUtilityMsgDialogGetStatus();
                        if (status == 2) {
                            sceUtilityMsgDialogUpdate(1);
                        } else if (status == 3) {
                            sceUtilityMsgDialogShutdownStart();
                        } else if (status == 4 || status == 0) {
                            running = 0;
                        }
                        sceDisplayWaitVblankStart();
                        sceGuSwapBuffers();
                    }
                    confirmed = (dialog.buttonPressed == PSP_UTILITY_MSGDIALOG_RESULT_YES);
                    { SceCtrlData ps; sceCtrlPeekBufferPositive(&ps, 1); prev_buttons = ps.Buttons; }
                }
                if (confirmed) {
                    me_shared_uncached[1] = ME_STAT_IDLE;
                    asm volatile("sync");
                    me_shared_uncached[0] = ME_CMD_FLUSH;
                    while (me_shared_uncached[1] != ME_STAT_DONE) asm volatile("nop");
                    sceKernelDcacheWritebackInvalidateAll();
                    me_active = 0;
                }
            } else {
                /* ME ON — 即時復帰 (警告不要) */
                sceKernelDcacheWritebackInvalidateAll();
                me_active = 1;
                me_shared_uncached[5] = 1;
            }
        }
        /* △: 曲削除 (メモステファイル削除 + リストから除去) */
        if ((pressed & PSP_CTRL_TRIANGLE) && total > 0) {
            pspUtilityMsgDialogParams dialog;
            memset(&dialog, 0, sizeof(dialog));
            dialog.base.size = sizeof(dialog);
            dialog.base.language = PSP_SYSTEMPARAM_LANGUAGE_JAPANESE;
            dialog.base.buttonSwap = get_system_accept();
            dialog.base.graphicsThread = 0x11;
            dialog.base.accessThread = 0x13;
            dialog.base.fontThread = 0x12;
            dialog.base.soundThread = 0x10;
            dialog.mode = PSP_UTILITY_MSGDIALOG_MODE_TEXT;
            dialog.options = PSP_UTILITY_MSGDIALOG_OPTION_TEXT
                           | PSP_UTILITY_MSGDIALOG_OPTION_YESNO_BUTTONS
                           | PSP_UTILITY_MSGDIALOG_OPTION_DEFAULT_NO;
            {
                /* 曲名から * プレフィクスを除去して表示 */
                const char *dname = tracks[list_cursor].name;
                if (dname[0] == '*') dname++;
                int lang = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
                sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &lang);
                if (lang == PSP_SYSTEMPARAM_LANGUAGE_JAPANESE) {
                    snprintf(dialog.message, sizeof(dialog.message),
                             "%s\xE3\x82\x92\xE5\x89\x8A\xE9\x99\xA4\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x99\xE3\x81\x8B\xEF\xBC\x9F", dname);
                } else {
                    snprintf(dialog.message, sizeof(dialog.message),
                             "Delete %s?", dname);
                }
            }
            int confirmed = 0;
            if (sceUtilityMsgDialogInitStart(&dialog) >= 0) {
                int dlg_run = 1;
                while (dlg_run) {
                    sceGuStart(GU_DIRECT, gu_list);
                    sceGuClearColor(0xFF080205);
                    sceGuClear(GU_COLOR_BUFFER_BIT);
                    sceGuFinish();
                    sceGuSync(0, 0);
                    int status = sceUtilityMsgDialogGetStatus();
                    if (status == 2) {
                        sceUtilityMsgDialogUpdate(1);
                    } else if (status == 3) {
                        sceUtilityMsgDialogShutdownStart();
                    } else if (status == 4 || status == 0) {
                        dlg_run = 0;
                    }
                    sceDisplayWaitVblankStart();
                    sceGuSwapBuffers();
                }
                confirmed = (dialog.buttonPressed == PSP_UTILITY_MSGDIALOG_RESULT_YES);
                { SceCtrlData ps; sceCtrlPeekBufferPositive(&ps, 1); prev_buttons = ps.Buttons; }
            }
            if (confirmed) {
                /* ファイル名を復元してメモステから削除 */
                const char *sname = tracks[list_cursor].name;
                if (sname[0] == '*') sname++;
                char del_path[256];
                snprintf(del_path, sizeof(del_path), "%s/%s.M", songs_dir, sname);
                sceIoRemove(del_path);
                log_write("deleted file");
                log_write(del_path);

                /* ms_bufsからバッファ解放 */
                for (int mi = 0; mi < ms_count; mi++) {
                    if (ms_bufs[mi] == tracks[list_cursor].data) {
                        free(ms_bufs[mi]);
                        /* ms_bufs/ms_names配列をシフト */
                        for (int mj = mi; mj < ms_count - 1; mj++) {
                            ms_bufs[mj] = ms_bufs[mj + 1];
                            memcpy(ms_names[mj], ms_names[mj + 1], MS_NAME_MAX);
                        }
                        ms_bufs[ms_count - 1] = NULL;
                        ms_count--;
                        break;
                    }
                }
                /* tracks配列をシフト */
                for (int ti = list_cursor; ti < num_tracks - 1; ti++) {
                    tracks[ti] = tracks[ti + 1];
                }
                num_tracks--;
                /* ms_names→tracks.nameポインタ再リンク */
                {
                    int mi2 = 0;
                    for (int ti2 = 0; ti2 < num_tracks; ti2++) {
                        if (mi2 < ms_count && tracks[ti2].data == ms_bufs[mi2]) {
                            tracks[ti2].name = ms_names[mi2];
                            mi2++;
                        }
                    }
                }
                /* カーソル補正 */
                if (list_cursor >= num_tracks && num_tracks > 0) list_cursor = num_tracks - 1;
                if (num_tracks == 0) list_cursor = 0;
            }
        }
        /* □: WAV Export */
        if ((pressed & PSP_CTRL_SQUARE) && total > 0) {
            current_track = list_cursor;
            wav_export_current();
        }
    } else {
        /* ○: 一時停止/再開 */
        if (pressed & PSP_CTRL_CIRCLE) {
            audio_paused = !audio_paused;
            if (audio_paused) {
                /* 一時停止前にMEスロットル解除 (デッドロック防止) */
                if (me_active) {
                    me_shared_uncached[53] = 0x7FF;
                }
                audio_ack_paused = 0;
                while (!audio_ack_paused) sceKernelDelayThread(1000);
            } else {
                /* 再開時にソロマスク復帰 */
                if (me_active) {
                    me_shared_uncached[53] = solo_mask;
                    solo_mask_sent = solo_mask;
                }
            }
        }
        /* D-pad左右: ビジュアルモード切替 */
        if (pressed & PSP_CTRL_LEFT) {
            vis_mode = (vis_mode == MODE_KEYBOARD) ? MODE_FMP : MODE_KEYBOARD;
        }
        if (pressed & PSP_CTRL_RIGHT) {
            vis_mode = (vis_mode == MODE_KEYBOARD) ? MODE_FMP : MODE_KEYBOARD;
        }
        /* ソロモード: D-pad上下でカーソル、△でトラックON/OFF */
        if (pressed & PSP_CTRL_UP) {
            solo_cursor--; if (solo_cursor < 0) solo_cursor = SOLO_NUM_PARTS - 1;
        }
        if (pressed & PSP_CTRL_DOWN) {
            solo_cursor++; if (solo_cursor >= SOLO_NUM_PARTS) solo_cursor = 0;
        }
        if (pressed & PSP_CTRL_TRIANGLE) {
            /* △: カーソルのトラックをミュートトグル */
            solo_mask ^= (1 << solo_cursor);
            if (solo_mask == 0) solo_mask = 0x7FF; /* 全OFF防止→全復帰 */
        }
        if (pressed & PSP_CTRL_SQUARE) {
            solo_mask = 0x7FF; /* □: 全復帰 */
        }
        /* ソロマスクをMEに通知 */
        if (me_active && solo_mask != solo_mask_sent) {
            me_shared_uncached[53] = solo_mask;
            solo_mask_sent = solo_mask;
            ring_flush_req = 1; /* リングバッファフラッシュ → 即反映 */
            /* SC側ログ: マスク変更時のSC再生位置とMEデコード位置 */
            {
                char sb[128];
                int me_prog = (int)me_shared_uncached[41];
                snprintf(sb, sizeof(sb),
                    "SOLO_SC mask=0x%03X bi=%d pos=%d me_prog=%d ring=%d",
                    solo_mask, lc_play, lc_play_pos, me_prog, ring_count());
                log_write(sb);
            }
        }
        /* L/R単独押し: 曲送り (R+L同時押しはスクリーンショット、曲送りしない) */
        int both_lr = (pad.Buttons & PSP_CTRL_LTRIGGER) && (pad.Buttons & PSP_CTRL_RTRIGGER);
        if (!both_lr) {
            if ((pressed & PSP_CTRL_LTRIGGER) && num_tracks>1) {
                current_track = (current_track-1+num_tracks) % num_tracks; start_track(current_track);
            }
            if ((pressed & PSP_CTRL_RTRIGGER) && num_tracks>1) {
                current_track = (current_track+1) % num_tracks; start_track(current_track);
            }
        }
    }
    /* R+L同時押し: スクリーンショット保存 */
    if ((pressed & PSP_CTRL_RTRIGGER) && (pad.Buttons & PSP_CTRL_LTRIGGER)) {
        save_screenshot();
    }
    return 1;
}

/* ============================================================
 *  スクリーンショット (BMP 24bit、SELECTで保存)
 * ============================================================ */
static int screenshot_counter = 0;

static void save_screenshot(void) {
    char path[64];
    char num[8];
    int n = screenshot_counter++;
    /* パス組み立て (sprintf不使用 — 軽量) */
    const char *prefix = ss_prefix;
    char *p = path;
    const char *s = prefix;
    while (*s) *p++ = *s++;
    { int v = n; if (v == 0) { *p++ = '0'; } else { char tmp[8]; int tn=0; while(v>0){tmp[tn++]='0'+(v%10);v/=10;} while(tn>0)*p++=tmp[--tn]; } }
    *p++ = '.'; *p++ = 'b'; *p++ = 'm'; *p++ = 'p'; *p = '\0';

    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;

    /* VRAMから表示中バッファを読む */
    unsigned int *vram = (unsigned int *)(0x44000000); /* uncached VRAM base */

    /* BMP header (24bit, 480x272) */
    int w = SCREEN_W, h = SCREEN_H;
    int row_bytes = w * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    int data_size = (row_bytes + pad) * h;
    int file_size = 54 + data_size;

    unsigned char hdr[54];
    memset(hdr, 0, 54);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = file_size; hdr[3] = file_size>>8; hdr[4] = file_size>>16; hdr[5] = file_size>>24;
    hdr[10] = 54; /* data offset */
    hdr[14] = 40; /* DIB header size */
    hdr[18] = w; hdr[19] = w>>8;
    hdr[22] = h; hdr[23] = h>>8;
    hdr[26] = 1; /* planes */
    hdr[28] = 24; /* bpp */
    hdr[34] = data_size; hdr[35] = data_size>>8; hdr[36] = data_size>>16; hdr[37] = data_size>>24;
    sceIoWrite(fd, hdr, 54);

    /* BMPは下→上の行順 */
    unsigned char row[480 * 3 + 4]; /* +pad */
    for (int y = h - 1; y >= 0; y--) {
        unsigned int *line = vram + y * BUF_WIDTH;
        for (int x = 0; x < w; x++) {
            unsigned int px = line[x]; /* ABGR */
            row[x*3]   = (px >> 16) & 0xFF; /* B */
            row[x*3+1] = (px >> 8)  & 0xFF; /* G */
            row[x*3+2] = px & 0xFF;         /* R */
        }
        memset(row + row_bytes, 0, pad);
        sceIoWrite(fd, row, row_bytes + pad);
    }
    sceIoClose(fd);
}

/* ============================================================
 *  PSP exit callback
 * ============================================================ */
static volatile int running = 1;

static int exit_callback(int arg1, int arg2, void *common) {
    (void)arg1; (void)arg2; (void)common;
    running = 0;
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void) {
    int thid = sceKernelCreateThread("cb_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) sceKernelStartThread(thid, 0, 0);
}

/* ============================================================
 *  main
 * ============================================================ */
int main(void) {
    setup_callbacks();
    scePowerSetClockFrequency(333, 333, 166); /* MAX clock */

    detect_storage();
    log_open();
    gu_init();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    /* intraFont初期化 — PSP内蔵フォント */
    intraFontInit();
    g_font = intraFontLoad("flash0:/font/ltn0.pgf",
                           INTRAFONT_CACHE_ALL | INTRAFONT_STRING_ASCII);
    if (g_font) {
        intraFontSetStyle(g_font, 0.7f, 0xFFFFFFFF, 0xFF000000, 0.0f, INTRAFONT_ALIGN_LEFT);
        log_write("FONT LOAD OK: intraFont ready");
    } else {
        log_write("FONT LOAD FAILED: g_font is NULL, fallback to 7seg");
    }
    /* リズムROM読み込み (pmd_initより前) */
    load_rhythm_rom();
    /* pmdmini初期化 */
    char pcmdir[] = "";
    pmd_init(pcmdir);
    pmd_setrate(PSP_SAMPLERATE);
    log_write("=== PMD Visualizer START ===");

    init_tracks();
    log_int("init_tracks: num_tracks", num_tracks);
    log_flush();
    fft_init();
    init_hann();
    memset(spectrum_smooth, 0, sizeof(spectrum_smooth));
    memset(energy_history, 0, sizeof(energy_history));

    /* リングバッファ確保 — 256を試行、失敗なら128にフォールバック */
    {
        ring_blocks = RING_BLOCKS_64MB;  /* まず256を試す */
        log_int("ring_blocks", ring_blocks);
        size_t rb_sz = (size_t)ring_blocks * PSP_AUDIO_SAMPLES * 2 * sizeof(short);
        size_t rv_sz = (size_t)ring_blocks * VIS_SIZE * sizeof(unsigned int);
        size_t rf_sz = (size_t)ring_blocks * FMP_SIZE * sizeof(unsigned int);
        ring_buf = (short (*)[PSP_AUDIO_SAMPLES * 2])memalign(64, rb_sz);
        ring_vis = (unsigned int (*)[VIS_SIZE])malloc(rv_sz);
        ring_fmp = (unsigned int (*)[FMP_SIZE])malloc(rf_sz);
        if (!ring_buf || !ring_vis || !ring_fmp) {
            /* フォールバック: 32MBサイズで再試行 */
            if (ring_buf) { free(ring_buf); ring_buf = NULL; }
            if (ring_vis) { free(ring_vis); ring_vis = NULL; }
            if (ring_fmp) { free(ring_fmp); ring_fmp = NULL; }
            ring_blocks = RING_BLOCKS_32MB;
            rb_sz = (size_t)ring_blocks * PSP_AUDIO_SAMPLES * 2 * sizeof(short);
            rv_sz = (size_t)ring_blocks * VIS_SIZE * sizeof(unsigned int);
            rf_sz = (size_t)ring_blocks * FMP_SIZE * sizeof(unsigned int);
            ring_buf = (short (*)[PSP_AUDIO_SAMPLES * 2])memalign(64, rb_sz);
            ring_vis = (unsigned int (*)[VIS_SIZE])malloc(rv_sz);
            ring_fmp = (unsigned int (*)[FMP_SIZE])malloc(rf_sz);
            log_write("ring: fallback to 32MB size");
        }
        memset(ring_buf, 0, rb_sz);
        memset(ring_vis, 0xFF, rv_sz);
        memset(ring_fmp, 0, rf_sz);
    }

    /* ME (MECC) 初期化 */
    me_shared_uncached = (volatile unsigned int *)(0x40000000 | (unsigned int)_me_shared);
    memset((void *)_me_shared, 0, sizeof(_me_shared));
    sceKernelDcacheWritebackInvalidateAll();
    log_write("ME: calling meLibDefaultInit...");
    {
        int table_id = meLibDefaultInit();
        log_int("ME: table_id", table_id);
        if (table_id >= 0) {
            me_active = 1;
            log_write("ME: ACTIVE");
        } else {
            me_active = 0;
            log_write("ME: INACTIVE (fallback to SC)");
        }
    }
    log_int("me_active", me_active);
    if (me_active) {
        sceKernelDelayThread(50000); /* MEがeDRAM初期化するのを待つ */
        log_hex("ME_eDRAM_addr", me_shared_uncached[6]);

        /* ME FPUテスト結果 */
        log_hex("ME_FPU_marker", me_shared_uncached[36]);
        if (me_shared_uncached[36] == 0xF9010001) {
            union { unsigned int u; float f; } cv;
            char msg[64];
            cv.u = me_shared_uncached[32];
            snprintf(msg, sizeof(msg), "ME_FPU add=%.6f (expect 4.0)", (double)cv.f);
            log_write(msg);
            cv.u = me_shared_uncached[33];
            snprintf(msg, sizeof(msg), "ME_FPU mul=%.6f (expect 6.0)", (double)cv.f);
            log_write(msg);
            cv.u = me_shared_uncached[34];
            snprintf(msg, sizeof(msg), "ME_FPU div=%.6f (expect 3.333)", (double)cv.f);
            log_write(msg);
            cv.u = me_shared_uncached[35];
            snprintf(msg, sizeof(msg), "ME_FPU sin=%.6f (expect 0.8415)", (double)cv.f);
            log_write(msg);
        } else {
            log_write("ME_FPU: TEST NOT COMPLETED");
        }
        /* 円周率テスト結果 */
        log_hex("ME_PI_marker", me_shared_uncached[39]);
        if (me_shared_uncached[39] == 0xF9020002) {
            union { unsigned int u; float f; } cv2;
            char msg[80];
            cv2.u = me_shared_uncached[37];
            snprintf(msg, sizeof(msg), "ME_PI leibniz=%.8f (expect 3.14059265, 1000terms)", (double)cv2.f);
            log_write(msg);
            cv2.u = me_shared_uncached[38];
            snprintf(msg, sizeof(msg), "ME_PI machin =%.8f (expect 3.14159265)", (double)cv2.f);
            log_write(msg);
            snprintf(msg, sizeof(msg), "ME_PI true   =3.14159265");
            log_write(msg);
        }
    }

    /* ループキャッシュ確保 (ME有効時のみ、段階的縮小) */
    if (me_active) {
        int lc_try_sec_64[] = {60, 45, 30, 25, 20, 15, 10, 0};
        int lc_try_sec_32[] = {30, 25, 20, 15, 10, 0};
        int *lc_try_sec = (ring_blocks >= RING_BLOCKS_64MB) ? lc_try_sec_64 : lc_try_sec_32;
        for (int ti = 0; lc_try_sec[ti] > 0; ti++) {
            int max_blk = (lc_try_sec[ti] * PSP_SAMPLERATE / PSP_RENDER_SAMPLES) + 1;
            size_t pcm_sz = (size_t)max_blk * PSP_RENDER_SAMPLES * 2 * sizeof(short);
            size_t vis_sz = (size_t)max_blk * VIS_SIZE * sizeof(unsigned int);
            size_t fmp_sz = (size_t)max_blk * FMP_SIZE * sizeof(unsigned int);
            log_int("loop cache: trying sec", lc_try_sec[ti]);
            log_int("loop cache: pcm_sz", (int)pcm_sz);
            lc_pcm[0] = (short *)memalign(64, pcm_sz);
            lc_pcm[1] = (short *)memalign(64, pcm_sz);
            lc_vis[0] = (unsigned int *)malloc(vis_sz);
            lc_vis[1] = (unsigned int *)malloc(vis_sz);
            lc_fmp[0] = (unsigned int *)malloc(fmp_sz);
            lc_fmp[1] = (unsigned int *)malloc(fmp_sz);
            if (lc_pcm[0] && lc_pcm[1] && lc_vis[0] && lc_vis[1] && lc_fmp[0] && lc_fmp[1]) {
                lc_enabled = 1;
                lc_actual_max_blocks = max_blk;
                log_int("loop cache: ENABLED sec", lc_try_sec[ti]);
                log_int("loop cache: max_blocks", max_blk);
                break;
            }
            /* 失敗 — 解放して次のサイズを試す */
            if (lc_pcm[0]) { free(lc_pcm[0]); lc_pcm[0] = NULL; }
            if (lc_pcm[1]) { free(lc_pcm[1]); lc_pcm[1] = NULL; }
            if (lc_vis[0]) { free(lc_vis[0]); lc_vis[0] = NULL; }
            if (lc_vis[1]) { free(lc_vis[1]); lc_vis[1] = NULL; }
            if (lc_fmp[0]) { free(lc_fmp[0]); lc_fmp[0] = NULL; }
            if (lc_fmp[1]) { free(lc_fmp[1]); lc_fmp[1] = NULL; }
            log_int("loop cache: FAILED sec", lc_try_sec[ti]);
        }
        if (!lc_enabled) log_write("loop cache: ALL SIZES FAILED, fallback");
    }

    /* オーディオ初期化 */
    audio_ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, PSP_AUDIO_SAMPLES, PSP_AUDIO_FORMAT_STEREO);
    log_int("audio_ch", audio_ch);
    if (audio_ch >= 0) {
        /* Output thread: 最高優先度 — sceAudio間ギャップゼロを保証 */
        int out_thid = sceKernelCreateThread("audio_output", audio_output_thread,
                                              0x10, 0x4000, 0, NULL);
        log_int("audio_out_thid", out_thid);
        if (out_thid >= 0) sceKernelStartThread(out_thid, 0, NULL);

        /* Producer thread: mainと同優先度 → OSラウンドロビンでCPU公平分配
         * 0x28: mainに常時プリエンプトされring枯渇, 0x18: yield遅延でring排水 → 0x20が正解 */
        int prod_thid = sceKernelCreateThread("audio_producer", audio_producer_thread,
                                               0x20, 0x10000, 0, NULL);
        log_int("audio_prod_thid", prod_thid);
        if (prod_thid >= 0) sceKernelStartThread(prod_thid, 0, NULL);
    }
    log_write("init complete, entering main loop");
    /* log_close() は終了時まで呼ばない — start_track等のログを取るため */

    /* prev_buttons初期化: 初回フレームの偽ボタン検出防止 */
    {
        SceCtrlData pad_init;
        sceCtrlPeekBufferPositive(&pad_init, 1);
        prev_buttons = pad_init.Buttons;
    }

    app_state = APP_STATE_LIST;

    /* リズムROM未検出ダイアログ */
    if (!rhythm_rom_loaded) {
        pspUtilityMsgDialogParams dialog;
        memset(&dialog, 0, sizeof(dialog));
        dialog.base.size = sizeof(dialog);
        dialog.base.language = PSP_SYSTEMPARAM_LANGUAGE_JAPANESE;
        dialog.base.buttonSwap = get_system_accept();
        dialog.base.graphicsThread = 0x11;
        dialog.base.accessThread = 0x13;
        dialog.base.fontThread = 0x12;
        dialog.base.soundThread = 0x10;
        dialog.mode = PSP_UTILITY_MSGDIALOG_MODE_TEXT;
        dialog.options = PSP_UTILITY_MSGDIALOG_OPTION_TEXT;
        {
            int lang = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
            sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &lang);
            if (lang == PSP_SYSTEMPARAM_LANGUAGE_JAPANESE) {
                snprintf(dialog.message, sizeof(dialog.message),
                         "YM2608 \xE3\x83\xAA\xE3\x82\xBA\xE3\x83\xA0ROM\xE3\x81\x8C\xE8\xA6\x8B\xE3\x81\xA4\xE3\x81\x8B\xE3\x82\x8A\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93\xE3\x80\x82\n"
                         "\xE3\x83\xAA\xE3\x82\xBA\xE3\x83\xA0\xE3\x83\x81\xE3\x83\xA3\xE3\x83\xB3\xE3\x83\x8D\xE3\x83\xAB\xE3\x81\xAF\xE7\x84\xA1\xE9\x9F\xB3\xE3\x81\xAB\xE3\x81\xAA\xE3\x82\x8A\xE3\x81\xBE\xE3\x81\x99\xE3\x80\x82\n\n"
                         "ym2608_adpcm_rom.bin \xE3\x82\x92\n"
                         "PMDVIS\xE3\x83\x95\xE3\x82\xA9\xE3\x83\xAB\xE3\x83\x80\xE3\x81\xAB\xE7\xBD\xAE\xE3\x81\x84\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84\xE3\x80\x82");
            } else {
                snprintf(dialog.message, sizeof(dialog.message),
                         "YM2608 Rhythm ROM not found.\n"
                         "Rhythm channels will be silent.\n\n"
                         "Place ym2608_adpcm_rom.bin in\n"
                         "the PMDVIS folder.");
            }
        }
        if (sceUtilityMsgDialogInitStart(&dialog) >= 0) {
            int dlg_run = 1;
            while (dlg_run) {
                sceGuStart(GU_DIRECT, gu_list);
                sceGuClearColor(0xFF080205);
                sceGuClear(GU_COLOR_BUFFER_BIT);
                sceGuFinish();
                sceGuSync(0, 0);
                int status = sceUtilityMsgDialogGetStatus();
                if (status == 2) sceUtilityMsgDialogUpdate(1);
                else if (status == 3) sceUtilityMsgDialogShutdownStart();
                else if (status == 4 || status == 0) dlg_run = 0;
                sceDisplayWaitVblankStart();
                sceGuSwapBuffers();
            }
            { SceCtrlData ps; sceCtrlPeekBufferPositive(&ps, 1); prev_buttons = ps.Buttons; }
        }
    }

    while (running) {
        /* do_analysis不要 — ピアノロールはFFT使わない */
        if (!handle_pad()) break;

        sceGuStart(GU_DIRECT, gu_list);
        sceGuScissor(0, 0, SCREEN_W, SCREEN_H); /* intraFontがscissor変更するため毎フレームリセット */
        sceGuDisable(GU_TEXTURE_2D); /* intraFontがテクスチャ有効にするため毎フレームリセット */
        sceGuClearColor(0xFF080205); /* ABGR: dark */
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        cur_blend = 0;
        vtx_count = 0;

        if (app_state == APP_STATE_LIST) {
            draw_song_list();
        } else {
            if (vis_mode == MODE_FMP)
                draw_fmp_view();
            else
                draw_keyboard();
            draw_hud();
            flush_draw();
        }

        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }

    /* ME停止 */
    if (me_active) {
        audio_paused = 1;
        sceKernelDelayThread(10000);
        /* DECODE_LOOPが実行中の場合キャンセルして完了を待つ */
        me_shared_uncached[42] = 1; /* cancel flag */
        {
            int wc = 0;
            while (me_shared_uncached[0] != ME_CMD_NONE) {
                asm volatile("nop");
                if (++wc > 10000000) break; /* 安全弁 */
            }
        }
        me_shared_uncached[42] = 0;
        me_shared_uncached[1] = ME_STAT_IDLE;
        asm volatile("sync");
        me_shared_uncached[0] = ME_CMD_STOP;
        {
            int wc = 0;
            while (me_shared_uncached[1] != ME_STAT_DONE) {
                asm volatile("nop");
                if (++wc > 10000000) break;
            }
        }
    }
    audio_alive = 0;
    sceKernelDelayThread(100*1000);
    if (audio_ch >= 0) sceAudioChRelease(audio_ch);
    pmd_stop();
    /* 終了ログは最小限 (タイミングログ廃止 — 終了が遅くなるため) */
    log_write("=== PMD Visualizer EXIT ===");
    log_close();
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
