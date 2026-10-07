/*
 * tod_scale.asi
 * ---------------------------------------------------------------------------
 * The Typing of the Dead (`Tod_e.exe`, Sega HGL engine, 2000) -- run at
 * 1920x1200 (or any resolution you like).
 *
 * The executable on disk is never touched: this plugin patches the running
 * process at load time, before the game's WinMain runs (it is loaded by the
 * Ultimate ASI Loader, which we install as `dinput.dll`, and `Tod_e.exe`
 * statically imports `DINPUT.dll`).
 *
 * There is no resolution setting anywhere in the game: the PC port is hard
 * wired to 640x480.  Two things decide the size of the game's back buffer and
 * the window it renders into:
 *
 *   1. The display setup routine at VA 0x4bddb0, which does
 *
 *          mov   dword [0x8490ac], 640      ; screen width
 *          mov   dword [0x8490b0], 480      ; screen height
 *
 *      and then creates the window and the D3DX context from those globals.
 *      The two immediates are signature-scanned and rewritten.
 *
 *   2. The engine's static video-mode list (VA 0x5eb6c8), 16 entries of
 *
 *          DWORD width, DWORD height, DWORD bpp, DWORD fullscreen
 *
 *      holding only 320x240 and 640x480 for 8/16/24/32 bpp.  Every entry whose
 *      (width,height) equals the source resolution is rewritten to the target,
 *      so any menu / runtime mode switch asks for the new size too.
 *
 * The globals themselves are also written directly, so code that reads them
 * before the setup routine runs already sees the target size.
 *
 *   3. The 2D layer (HUD, text, rain) is drawn with pre-transformed
 *      screen-space vertices (FVF 0x1c4 = XYZRHW|DIFFUSE|SPECULAR|TEX1) at
 *      hard-coded 640x480 pixel coordinates, so it stays stuck in the top-left
 *      corner.  The plugin hooks IDirect3DDevice7::DrawPrimitive (vtable index
 *      25) on the live device and scales the xy of every XYZRHW vertex to the
 *      target resolution before forwarding the call.
 *
 * Target binary (verified):
 *     Tod_e.exe  size 2,048,000   PE32   image base 0x400000
 *                MD5 d6fd9e2eafb90871196d66881688b738
 *
 * Configuration: tod_scale.ini next to this .asi
 *     [TodScale]
 *     width=1920
 *     height=1200
 *     source_width=640
 *     source_height=480
 *     patch_modes=1
 *     write_globals=1
 *     hud_scale=1      ; 0 = off, 1 = stretch to fill, 2 = uniform (by height)
 *     hud_scale_x=0    ; explicit multiplier (0 = auto)
 *     hud_scale_y=0
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/* ---- configuration ------------------------------------------------------- */
static int g_dst_w         = 1920;
static int g_dst_h         = 1200;
static int g_src_w         = 640;
static int g_src_h         = 480;
static int g_patch_modes   = 1;
static int g_write_globals = 1;
static int g_hud_scale     = 1;       /* 0 off, 1 stretch, 2 uniform */
static int g_hud_scale_x   = 0;       /* 0 = auto */
static int g_hud_scale_y   = 0;

/* device pointer global (VA 0x8491ec); the 2D draw wrappers and the
   SetTextureStageState code all load the IDirect3DDevice7 from here. */
#define RVA_DEVICE_GLOBAL 0x004491ecu
#define VTABLE_DRAWPRIMITIVE 25
#define D3DFVF_XYZRHW 0x004
#define D3DFVF_POSITION_MASK 0x00e

/* ---- runtime ------------------------------------------------------------- */
static BYTE  *g_base       = 0;
static DWORD  g_image_size = 0;
static char   g_dir[MAX_PATH] = { 0 };

static void log_line(const char *fmt, ...);

/* -------------------------------------------------------------------------- */
/* helpers                                                                     */
/* -------------------------------------------------------------------------- */

static void get_module_dir(HMODULE self, char *out, DWORD cap)
{
    DWORD n = GetModuleFileNameA(self, out, cap);
    if (n == 0 || n >= cap) { out[0] = '\0'; return; }
    char *slash = out;
    for (char *p = out; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    *slash = '\0';
}

static void join_path(char *out, DWORD cap, const char *dir, const char *file)
{
    if (dir[0]) _snprintf(out, cap, "%s\\%s", dir, file);
    else        _snprintf(out, cap, "%s", file);
    out[cap - 1] = '\0';
}

static int read_int_ini(const char *section, const char *key, int fallback)
{
    char ini[MAX_PATH], tmp[32];
    join_path(ini, MAX_PATH, g_dir, "tod_scale.ini");
    GetPrivateProfileStringA(section, key, "", tmp, sizeof(tmp), ini);
    if (tmp[0]) return atoi(tmp);
    return fallback;
}

static int read_int_env(const char *name, int fallback)
{
    char tmp[32];
    if (GetEnvironmentVariableA(name, tmp, sizeof(tmp)) > 0)
        return atoi(tmp);
    return fallback;
}

static void read_config(void)
{
    g_dst_w = read_int_env("TOD_SCALE_WIDTH",
              read_int_ini("TodScale", "width", g_dst_w));
    g_dst_h = read_int_env("TOD_SCALE_HEIGHT",
              read_int_ini("TodScale", "height", g_dst_h));
    g_src_w = read_int_env("TOD_SCALE_SOURCE_WIDTH",
              read_int_ini("TodScale", "source_width", g_src_w));
    g_src_h = read_int_env("TOD_SCALE_SOURCE_HEIGHT",
              read_int_ini("TodScale", "source_height", g_src_h));
    g_patch_modes = read_int_env("TOD_SCALE_PATCH_MODES",
              read_int_ini("TodScale", "patch_modes", g_patch_modes));
    g_write_globals = read_int_env("TOD_SCALE_WRITE_GLOBALS",
              read_int_ini("TodScale", "write_globals", g_write_globals));
    g_hud_scale = read_int_env("TOD_SCALE_HUD",
              read_int_ini("TodScale", "hud_scale", g_hud_scale));
    g_hud_scale_x = read_int_env("TOD_SCALE_HUD_SCALE_X",
              read_int_ini("TodScale", "hud_scale_x", g_hud_scale_x));
    g_hud_scale_y = read_int_env("TOD_SCALE_HUD_SCALE_Y",
              read_int_ini("TodScale", "hud_scale_y", g_hud_scale_y));
}

static void put_u32(BYTE *p, unsigned int v)
{
    p[0] = (BYTE)(v      );
    p[1] = (BYTE)(v >>  8);
    p[2] = (BYTE)(v >> 16);
    p[3] = (BYTE)(v >> 24);
}

static unsigned int get_u32(const BYTE *p)
{
    return (unsigned int)p[0]
         | ((unsigned int)p[1] <<  8)
         | ((unsigned int)p[2] << 16)
         | ((unsigned int)p[3] << 24);
}

static int write_u32(BYTE *p, unsigned int v)
{
    DWORD old = 0;
    if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) {
        log_line("VirtualProtect(%p) failed: %lu", p, GetLastError());
        return 0;
    }
    put_u32(p, v);
    FlushInstructionCache(GetCurrentProcess(), p, 4);
    VirtualProtect(p, 4, old, &old);
    return 1;
}

/* -------------------------------------------------------------------------- */
/* patch 1: the 640/480 immediates in the display setup routine                */
/* -------------------------------------------------------------------------- */
/*
 * Search for the signature
 *
 *     C7 05 <addrW> <srcW>   C7 05 <addrH> <srcH>
 *     mov  dword[addrW], srcW ; mov dword[addrH], srcH
 *
 * (the `push 0x20` that precedes it is not required, the two back-to-back
 *  stores to consecutive screen-size globals are already unique).
 */
static int patch_init(void)
{
    BYTE pat[20];
    int  patlen = 0;
    int  matches = 0;

    memset(pat, 0, sizeof(pat));
    pat[patlen++] = 0xC7; pat[patlen++] = 0x05;
    patlen += 4;                                   /* addrW, wildcarded */
    put_u32(pat + patlen, (unsigned int)g_src_w); patlen += 4;
    pat[patlen++] = 0xC7; pat[patlen++] = 0x05;
    patlen += 4;                                   /* addrH, wildcarded */
    put_u32(pat + patlen, (unsigned int)g_src_h); patlen += 4;

    for (BYTE *p = g_base; p + patlen <= g_base + g_image_size; ++p) {
        int ok = 1, i;
        for (i = 0; i < patlen; ++i) {
            /* skip the two 4-byte operand slots (offsets 2..5 and 12..15) */
            if (i >= 2 && i <= 5) continue;
            if (i >= 12 && i <= 15) continue;
            if (p[i] != pat[i]) { ok = 0; break; }
        }
        if (!ok) continue;

        matches++;

        BYTE *addrW = p + 2;
        BYTE *addrH = p + 12;
        unsigned int gw = get_u32(addrW);
        unsigned int gh = get_u32(addrH);

        log_line("display setup @ %p : globals w=%#x h=%#x  %dx%d -> %dx%d",
                 p, gw, gh, g_src_w, g_src_h, g_dst_w, g_dst_h);

        if (write_u32(p + 6,  (unsigned int)g_dst_w) &&
            write_u32(p + 16, (unsigned int)g_dst_h))
            log_line("  immediates patched");

        if (g_write_globals) {
            if (write_u32((BYTE *)(uintptr_t)gw, (unsigned int)g_dst_w) &&
                write_u32((BYTE *)(uintptr_t)gh, (unsigned int)g_dst_h))
                log_line("  globals written");
        }
    }

    log_line("display setup: %d signature(s) found", matches);
    return matches;
}

/* -------------------------------------------------------------------------- */
/* patch 2: the static video-mode list                                         */
/* -------------------------------------------------------------------------- */

static int valid_bpp(unsigned int v)
{
    return v == 8 || v == 15 || v == 16 || v == 24 || v == 32;
}

static int patch_modes(void)
{
    BYTE *start = g_base;
    BYTE *end   = g_base + g_image_size - 16;
    int   found = 0, patched = 0;

    for (BYTE *p = start; p <= end; p += 4) {
        unsigned int w     = get_u32(p);
        unsigned int h     = get_u32(p + 4);
        unsigned int bpp   = get_u32(p + 8);
        unsigned int full  = get_u32(p + 12);

        if (w != (unsigned int)g_src_w || h != (unsigned int)g_src_h) continue;
        if (!valid_bpp(bpp)) continue;
        if (full != 0 && full != 1) continue;

        found++;

        if (write_u32(p,     (unsigned int)g_dst_w) &&
            write_u32(p + 4, (unsigned int)g_dst_h)) {
            log_line("mode entry @ %p : %ux%u@%u full=%u -> %dx%d",
                     p, w, h, bpp, full, g_dst_w, g_dst_h);
            patched++;
        }
    }

    /* The 640x480 modes come in pairs (depth) x (windowed/fullscreen):
       the real list has 8 of them; refuse to touch a single coincidence. */
    if (found < 4) {
        log_line("mode list: only %d candidate(s) found, not patching", found);
        return 0;
    }
    log_line("mode list: %d entr(ies) patched", patched);
    return patched;
}

/* -------------------------------------------------------------------------- */
/* patch 3: scale the screen-space (XYZRHW) 2D layer                           */
/* -------------------------------------------------------------------------- */
/*
 * The engine draws the HUD/menus/rain with pre-transformed vertices whose xy
 * are 640x480 screen pixels.  We hook the live IDirect3DDevice7's DrawPrimitive
 * slot and scale the first two floats of every XYZRHW vertex.  The FVF stride
 * mirrors the wrapper's GetVertexSize()/TexCoordSetOffset().
 */

typedef int (__stdcall *draw_primitive_fn)(void *self, unsigned int prim,
                                           unsigned int fvf, void *verts,
                                           unsigned int count,
                                           unsigned int flags);

static draw_primitive_fn g_orig_draw = 0;
static float g_hud_sx = 0.0f;
static float g_hud_sy = 0.0f;
static float g_hud_off_x = 0.0f;
static float g_hud_off_y = 0.0f;
static volatile LONG g_hud_logged = 0;
static HANDLE g_hud_thread = 0;

/* bytes occupied by the n-th texture-coordinate set */

static unsigned int tex_coord_bytes(unsigned int fvf, unsigned int set)
{
    static const unsigned int kSetBytes[4] = { 8, 12, 16, 4 };
    unsigned int bytes = 0, i;
    for (i = 0; i < set && i < 8; ++i)
        bytes += kSetBytes[(fvf >> (16 + i * 2)) & 3];
    return bytes;
}

static unsigned int fvf_stride(unsigned int fvf)
{
    unsigned int size = 0;

    if (fvf & 0x004)      size += 4 * 4;   /* XYZRHW */
    else if (fvf & 0x002) size += 3 * 4;   /* XYZ    */
    else if (fvf & 0x006) size += 3 * 4;   /* XYZB1  */

    if (fvf & 0x010) size += 3 * 4;        /* NORMAL   */
    if (fvf & 0x020) size += 4;            /* PSIZE    */
    if (fvf & 0x040) size += 4;            /* DIFFUSE  */
    if (fvf & 0x080) size += 4;            /* SPECULAR */

    size += tex_coord_bytes(fvf, (fvf >> 8) & 0xf);
    return size;
}

static int __stdcall hook_draw_primitive(void *self, unsigned int prim,
                                         unsigned int fvf, void *verts,
                                         unsigned int count,
                                         unsigned int flags)
{
    static __thread BYTE *tl_buf = 0;
    static __thread unsigned int tl_cap = 0;

    if (g_hud_sx > 0.0f && verts && count &&
        (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
        unsigned int stride = fvf_stride(fvf);
        unsigned int bytes  = stride * count;

        if (stride >= 8 && bytes) {
            if (bytes > tl_cap) {
                BYTE *nb = (BYTE *)HeapAlloc(GetProcessHeap(), 0, bytes);
                if (nb) {
                    if (tl_buf) HeapFree(GetProcessHeap(), 0, tl_buf);
                    tl_buf = nb;
                    tl_cap = bytes;
                }
            }
            if (tl_buf && bytes <= tl_cap) {
                unsigned int i;
                memcpy(tl_buf, verts, bytes);
                for (i = 0; i < count; ++i) {
                    float *v = (float *)(tl_buf + (size_t)i * stride);
                    v[0] = v[0] * g_hud_sx + g_hud_off_x;
                    v[1] = v[1] * g_hud_sy + g_hud_off_y;
                }
                if (InterlockedExchange(&g_hud_logged, 1) == 0)
                    log_line("2D hook active: scaling XYZRHW verts by %.3fx%.3f "
                             "(stride=%u count=%u fvf=%#x)",
                             g_hud_sx, g_hud_sy, stride, count, fvf);
                return g_orig_draw(self, prim, fvf, tl_buf, count, flags);
            }
        }
    }
    return g_orig_draw(self, prim, fvf, verts, count, flags);
}

static DWORD WINAPI hud_hook_thread(LPVOID param)
{
    BYTE *dev_global = g_base + RVA_DEVICE_GLOBAL;
    (void)param;

    for (;;) {
        unsigned int dev = *(volatile unsigned int *)dev_global;
        if (dev) {
            unsigned int *vt = *(unsigned int **)(uintptr_t)dev;
            if (vt) {
                unsigned int orig = vt[VTABLE_DRAWPRIMITIVE];
                if (orig > 0x10000 && orig != (unsigned int)(uintptr_t)hook_draw_primitive) {
                    DWORD old = 0;
                    if (VirtualProtect(&vt[VTABLE_DRAWPRIMITIVE], 4,
                                       PAGE_EXECUTE_READWRITE, &old)) {
                        g_orig_draw = (draw_primitive_fn)(uintptr_t)orig;
                        vt[VTABLE_DRAWPRIMITIVE] =
                            (unsigned int)(uintptr_t)hook_draw_primitive;
                        FlushInstructionCache(GetCurrentProcess(),
                                              &vt[VTABLE_DRAWPRIMITIVE], 4);
                        VirtualProtect(&vt[VTABLE_DRAWPRIMITIVE], 4, old, &old);
                        log_line("hooked IDirect3DDevice7::DrawPrimitive "
                                 "(device=%#x vtable=%p orig=%#x) hud=%.3fx%.3f",
                                 dev, vt, orig, g_hud_sx, g_hud_sy);
                    } else {
                        log_line("VirtualProtect(vtable) failed: %lu",
                                 GetLastError());
                    }
                }
            }
        }
        Sleep(250);
    }
    return 0;
}

static void install_hud_hook(void)
{
    if (g_hud_scale == 0) {
        log_line("2D hook disabled");
        return;
    }

    if (g_hud_scale_x > 0) g_hud_sx = (float)g_hud_scale_x;
    else if (g_hud_scale == 2) g_hud_sx = (float)g_dst_h / (float)g_src_h;
    else g_hud_sx = (float)g_dst_w / (float)g_src_w;

    if (g_hud_scale_y > 0) g_hud_sy = (float)g_hud_scale_y;
    else g_hud_sy = (float)g_dst_h / (float)g_src_h;

    /* uniform mode keeps the 4:3 2D layer centred in the wider surface */
    if (g_hud_scale == 2 && g_hud_scale_x <= 0 && g_hud_scale_y <= 0) {
        g_hud_off_x = ((float)g_dst_w - (float)g_src_w * g_hud_sx) * 0.5f;
        g_hud_off_y = ((float)g_dst_h - (float)g_src_h * g_hud_sy) * 0.5f;
    }

    g_hud_thread = CreateThread(0, 0, hud_hook_thread, 0, 0, 0);
    if (!g_hud_thread)
        log_line("2D hook: CreateThread failed: %lu", GetLastError());
}

/* -------------------------------------------------------------------------- */
/* logging + entry point                                                       */
/* -------------------------------------------------------------------------- */

static void log_line(const char *fmt, ...)
{
    char path[MAX_PATH];
    join_path(path, MAX_PATH, g_dir, "tod_scale.log");

    HANDLE h = CreateFileA(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    char buf[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int len = _snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d] ",
                        st.wYear, st.wMonth, st.wDay,
                        st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(buf + len, sizeof(buf) - len - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)sizeof(buf) - len - 2;
    len += n;
    if (len > (int)sizeof(buf) - 2) len = (int)sizeof(buf) - 2;
    buf[len++] = '\r'; buf[len++] = '\n';

    DWORD written = 0;
    WriteFile(h, buf, (DWORD)len, &written, NULL);
    CloseHandle(h);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls(hinst);
    get_module_dir(hinst, g_dir, MAX_PATH);

    g_base = (BYTE *)GetModuleHandleA(NULL);

    /* SizeOfImage, so scanning never runs off the mapped executable. */
    g_image_size = 0x220000;
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                g_image_size = nt->OptionalHeader.SizeOfImage;
        }
    }

    read_config();

    log_line("tod_scale.asi loaded (base=%p size=%#lx) target %dx%d from %dx%d",
             g_base, g_image_size, g_dst_w, g_dst_h, g_src_w, g_src_h);

    if (g_dst_w == g_src_w && g_dst_h == g_src_h) {
        log_line("target equals source, nothing to do");
        return TRUE;
    }

    patch_init();
    if (g_patch_modes) patch_modes();
    install_hud_hook();

    return TRUE;
}
