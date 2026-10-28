/*
 * Grid_Core_Bloom_Fix - Race Driver: GRID (EGO 1.x) fix for modern CPUs / Windows
 * https://github.com/ZUKMAN/Grid_Core_Bloom_Fix
 *
 * SPDX-License-Identifier: MIT
 *
 * Loaded as dinput8.dll proxy (grid.exe imports DINPUT8!DirectInput8Create). The import table of
 * grid.exe is patched from DllMain, before the Steam DRM stub (SteamStub 2.0, it only decrypts
 * .text) and the game's entry point run. Nothing on disk is modified.
 *
 * Analysis of grid.exe (Steam, TimeDateStamp 0x4ACE02BC):
 *   - Threads: unlike DiRT, GRID never calls SetThreadAffinityMask. Worker threads only get
 *     SetThreadIdealProcessor(core % dwNumberOfProcessors). Several pools and per-thread caches
 *     are sized by GetSystemInfo().dwNumberOfProcessors.
 *   - hardware_settings_restrictions.xml picks workerMap2Core/4Core by "cores", and 2Core for any
 *     CPU with "hyperThreading" (the CPUID HTT bit, set on every modern Intel CPU).
 *     workerMap4Core.xml is the real multi-threaded config (solver/integrator/cloth workers are
 *     supported in GRID). workerMap8Core.xml references undefined workers "view_manager_2/3".
 *   - VRAM comes from IDirectDraw7::GetAvailableVidMem (DWORD) and is compared as a signed value:
 *     4 GB cards end up in <res mem="270" maxWidth="1280" />, the resolution is capped at 1280.
 *   - Every D3D9 display mode becomes a node in hardware_settings_info.xml, which lives in the
 *     HARDWARE_SETTINGS pool of system/applicationMemoryMap.xml: 16 KB. Modern Windows reports
 *     dozens of modes and the pool runs out (same crash as DiRT).
 *   - At startup the "virtual memory manager" reserves ONE contiguous block of
 *     (0x325 + 0xA) << 20 = 815 MB of address space (10 MB with -novirtual). Without the Large
 *     Address Aware flag (2 GB) that block often doesn't exist any more on a modern system ->
 *     the game fails to start. The flag is the byte at 0xDAFF08 (1 = virtual).
 *   - WinMain converts the command line with WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS):
 *     with the "Beta: Use Unicode UTF-8" option (ACP 65001) that flag is invalid and conversions
 *     fail. The CRT uses GetACP/GetCPInfo the same way.
 *   - The yellow look: tracks/<track>/effects.xml (text or EGO binary XML) set tintColour to
 *     ~255,220,140 with tintAmount 0.15-0.5, low "colour" and bloomAmount ~1.0.
 *
 * Fixes:
 *   - GetSystemInfo reports 4 CPUs (2/1 on small CPUs), ideal processors are remapped to
 *     physical P-cores (topology code from Dirt1_Core_Bloom_Fix, ported from DiRT 2).
 *   - workerMap*.xml -> the game's own workerMap4Core.xml (2Core / workerMap.xml on small CPUs).
 *   - VRAM clamp, D3D9 display mode filter, HARDWARE_SETTINGS pool enlarged on the fly.
 *   - "novirtual" automatically when grid.exe is not Large Address Aware.
 *   - ANSI code page 1252 instead of UTF-8 for the game's code page APIs.
 *   - Post processing rules for tracks/<track>/effects.xml, text and binary.
 *   - Track surface LOD distance (tracks/tracklodsettings.xml, 30 m in the game).
 */

#include <windows.h>
#include <ddraw.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <vector>

// --------------------------------------------------------------------------------------------
// Settings (Grid_Core_Bloom_Fix.ini next to the dll)
// --------------------------------------------------------------------------------------------

static struct {
    int affinity = 1;            // 0 = no ideal processor, 1 = remap to best cores, 2 = original
    int workerMap = 1;           // 1 = redirect to the 4/2/1 core map, 0 = use game's selection
    int virtualMemory = -1;      // -1 = auto (off without LAA), 0 = like -novirtual, 1 = on
    int codePage = 1;            // 1 = use code page 1252 when Windows runs with UTF-8 (65001)
    int maxVideoMemoryMB = 1024; // 0 = no clamp
    int filterModes = 1;         // 1 = filter D3D9 display modes
    int maxResolutions = 12;     // max distinct resolutions reported
    int minWidth = 800;          // smaller resolutions are dropped
    int hardwareSettingsKB = 256;// HARDWARE_SETTINGS pool size, 0 = keep 16 KB
    int dpiAware = 1;
    int log = 1;
    int postProcess = 1;         // 1 = rewrite tracks/<track>/effects.xml on load
    int lodDistance = 1000;      // SURFACELOD loddistance, 0 = keep the game's value (30)
    int lodBlend = 100;          // SURFACELOD blend, 0 = keep the game's value (10)
} g_cfg;

// Post process parameter overrides: <Param name="X"> inside <Effect chain="Type2"> of track files.
struct ParamOverride {
    char name[48];
    char value[48];
    int chain;
};
static ParamOverride g_pp[64];
static int g_ppCount;
static char g_iniPath[MAX_PATH];

static void add_override(const char* name, const char* value, int chain)
{
    if (!value[0] || _stricmp(value, "keep") == 0 || g_ppCount >= 64) {
        return;
    }
    // later rules win
    for (int i = 0; i < g_ppCount; ++i) {
        if (g_pp[i].chain == chain && _stricmp(g_pp[i].name, name) == 0) {
            g_pp[i] = g_pp[--g_ppCount];
            --i;
        }
    }
    ParamOverride& o = g_pp[g_ppCount++];
    lstrcpynA(o.name, name, sizeof(o.name));
    lstrcpynA(o.value, value, sizeof(o.value));
    o.chain = chain;
}

static void add_amount(const char* key, const char* def, const char* param)
{
    char v[48];
    GetPrivateProfileStringA("PostProcess", key, def, v, sizeof(v), g_iniPath);
    if (!v[0] || _stricmp(v, "keep") == 0) {
        return;
    }
    char f[48];
    snprintf(f, sizeof(f), "%f", atof(v));
    add_override(param, f, 2);
}

static void load_postprocess_config()
{
    g_ppCount = 0;
    if (!g_cfg.postProcess) {
        return;
    }
    // tracks/<track>/effects.xml, Type2 effects (outside, cockpit, transitions, garages).
    add_amount("Bloom", "0.30", "bloomAmount");
    add_amount("Vignette", "0", "vignetteAmount");
    add_amount("Brightness", "keep", "brighness"); // sic, that is the engine's name
    add_amount("Contrast", "keep", "contrast");
    add_amount("Saturation", "keep", "colour");
    add_amount("ColourAmount", "keep", "colourAmount");
    add_amount("Gamma", "keep", "gamma");
    add_amount("MotionBlur", "keep", "blurLength");

    char v[48];
    GetPrivateProfileStringA("PostProcess", "Tint", "0", v, sizeof(v), g_iniPath);
    if (v[0] && _stricmp(v, "keep") != 0 && atof(v) == 0.0) {
        add_override("tintColour", "255, 255, 255, 255", 2);
        add_override("tintAmount", "0.000000", 2);
    }

    // Free form overrides: [PostProcessOverride] paramName=value
    char keys[4096];
    DWORD len = GetPrivateProfileSectionA("PostProcessOverride", keys, sizeof(keys), g_iniPath);
    for (char* line = keys; len && *line; line += strlen(line) + 1) {
        if (*line == ';') {
            continue;
        }
        char* eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        add_override(line, eq + 1, 2);
    }
}

static char g_dllDir[MAX_PATH];
static FILE* g_log;

static void logf(const char* fmt, ...)
{
    if (!g_log) {
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

static void load_config()
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%s\\Grid_Core_Bloom_Fix.ini", g_dllDir);
    lstrcpynA(g_iniPath, ini, MAX_PATH);
    g_cfg.affinity = GetPrivateProfileIntA("Threads", "Affinity", g_cfg.affinity, ini);
    g_cfg.workerMap = GetPrivateProfileIntA("Threads", "WorkerMap", g_cfg.workerMap, ini);
    g_cfg.virtualMemory = GetPrivateProfileIntA("Memory", "VirtualMemory", g_cfg.virtualMemory, ini);
    g_cfg.hardwareSettingsKB = GetPrivateProfileIntA("Memory", "HardwareSettingsPoolKB", g_cfg.hardwareSettingsKB, ini);
    g_cfg.maxVideoMemoryMB = GetPrivateProfileIntA("Video", "MaxVideoMemoryMB", g_cfg.maxVideoMemoryMB, ini);
    g_cfg.filterModes = GetPrivateProfileIntA("Video", "FilterModes", g_cfg.filterModes, ini);
    g_cfg.maxResolutions = GetPrivateProfileIntA("Video", "MaxResolutions", g_cfg.maxResolutions, ini);
    g_cfg.minWidth = GetPrivateProfileIntA("Video", "MinWidth", g_cfg.minWidth, ini);
    g_cfg.codePage = GetPrivateProfileIntA("Misc", "FixUtf8CodePage", g_cfg.codePage, ini);
    g_cfg.dpiAware = GetPrivateProfileIntA("Misc", "DPIAware", g_cfg.dpiAware, ini);
    g_cfg.log = GetPrivateProfileIntA("Misc", "Log", g_cfg.log, ini);
    g_cfg.postProcess = GetPrivateProfileIntA("PostProcess", "Enable", g_cfg.postProcess, ini);
    g_cfg.lodDistance = GetPrivateProfileIntA("Graphics", "SurfaceLodDistance", g_cfg.lodDistance, ini);
    g_cfg.lodBlend = GetPrivateProfileIntA("Graphics", "SurfaceLodBlend", g_cfg.lodBlend, ini);
}

static void open_log()
{
    if (!g_cfg.log) {
        return;
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\Grid_Core_Bloom_Fix.log", g_dllDir);
    g_log = fopen(path, "w");
    if (!g_log) {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(path, sizeof(path), "%sGrid_Core_Bloom_Fix.log", tmp);
        g_log = fopen(path, "w");
    }
}

// --------------------------------------------------------------------------------------------
// CPU topology
// --------------------------------------------------------------------------------------------

struct Core {
    BYTE efficiency;   // higher = faster (P-core)
    int logical[4];    // logical CPU indices (< 32) of this core, first = primary
    int count;
};

static Core g_cores[32];
static int g_coreCount;

static int g_map[32];        // engine core index -> logical CPU
static int g_mapCount;       // number of CPUs reported to the engine
static int g_realLogical;    // real logical CPU count (in process mask)
static DWORD g_processMask;

static int popcount32(DWORD v)
{
    int n = 0;
    for (; v; v &= v - 1) {
        ++n;
    }
    return n;
}

static void detect_topology()
{
    DWORD_PTR processMask = 0, systemMask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
    g_processMask = DWORD(processMask);
    if (!g_processMask) {
        g_processMask = 1;
    }
    g_realLogical = popcount32(g_processMask);

    typedef BOOL(WINAPI * GLPIEX)(LOGICAL_PROCESSOR_RELATIONSHIP, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, PDWORD);
    auto glpi = GLPIEX(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetLogicalProcessorInformationEx"));

    g_coreCount = 0;

    if (glpi) {
        DWORD size = 0;
        glpi(RelationProcessorCore, nullptr, &size);
        auto buffer = (BYTE*)HeapAlloc(GetProcessHeap(), 0, size);
        if (buffer && glpi(RelationProcessorCore, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX(buffer), &size)) {
            for (DWORD offset = 0; offset < size && g_coreCount < 32;) {
                auto info = PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX(buffer + offset);
                offset += info->Size;

                if (info->Relationship != RelationProcessorCore) {
                    continue;
                }

                // A 32-bit process only sees processor group 0 and CPUs 0..31.
                for (WORD g = 0; g < info->Processor.GroupCount; ++g) {
                    if (info->Processor.GroupMask[g].Group != 0) {
                        continue;
                    }
                    DWORD mask = DWORD(info->Processor.GroupMask[g].Mask) & g_processMask;
                    if (!mask) {
                        continue;
                    }
                    Core& core = g_cores[g_coreCount++];
                    core.efficiency = info->Processor.EfficiencyClass;
                    core.count = 0;
                    for (int bit = 0; bit < 32 && core.count < 4; ++bit) {
                        if (mask & (1u << bit)) {
                            core.logical[core.count++] = bit;
                        }
                    }
                }
            }
        }
        if (buffer) {
            HeapFree(GetProcessHeap(), 0, buffer);
        }
    }

    if (g_coreCount == 0) {
        // Fallback: every logical CPU is a core.
        for (int bit = 0; bit < 32; ++bit) {
            if (g_processMask & (1u << bit)) {
                Core& core = g_cores[g_coreCount++];
                core.efficiency = 0;
                core.logical[0] = bit;
                core.count = 1;
            }
        }
    }

    // Stable sort: fastest cores first (P-cores before E-cores), keep OS order otherwise.
    for (int i = 1; i < g_coreCount; ++i) {
        Core c = g_cores[i];
        int j = i - 1;
        while (j >= 0 && g_cores[j].efficiency < c.efficiency) {
            g_cores[j + 1] = g_cores[j];
            --j;
        }
        g_cores[j + 1] = c;
    }

    BYTE best = g_cores[0].efficiency;
    int fastCores = 0;
    for (int i = 0; i < g_coreCount; ++i) {
        if (g_cores[i].efficiency == best) {
            ++fastCores;
        }
    }

    // Number of worker cores: the 4 core map at most. The 8 core map is the only one that
    // puts workers into the physics "solver"/"cloth" groups, which the engine does not
    // support (crash on race start), and 4 workers are plenty for this game.
    int k = fastCores >= 4 ? 4 : fastCores >= 2 ? 2 : 1;
    if (k == 1 && g_realLogical >= 2) {
        k = 2; // one fast core plus SMT/E-cores
    }

    // Mapping table: primary thread of each core (fast cores first), then SMT siblings.
    // Unlike DiRT 2's Win7 path (which pairs siblings: 0,0',1,1'...) we fill physical cores
    // first, because the 2/4/8 core maps assume one high priority worker per core.
    int order[32];
    int n = 0;
    for (int i = 0; i < g_coreCount && n < 32; ++i) {
        order[n++] = g_cores[i].logical[0];
    }
    for (int s = 1; s < 4; ++s) {
        for (int i = 0; i < g_coreCount && n < 32; ++i) {
            if (g_cores[i].count > s) {
                order[n++] = g_cores[i].logical[s];
            }
        }
    }

    g_mapCount = k;
    for (int i = 0; i < k; ++i) {
        g_map[i] = order[i % n];
    }

    logf("CPU: %d logical CPUs in process mask %08lX, %d cores (%d fastest, efficiency class %d)",
         g_realLogical, g_processMask, g_coreCount, fastCores, best);
    for (int i = 0; i < g_coreCount; ++i) {
        char list[64] = {};
        for (int s = 0; s < g_cores[i].count; ++s) {
            char item[8];
            snprintf(item, sizeof(item), s ? ",%d" : "%d", g_cores[i].logical[s]);
            strcat_s(list, item);
        }
        logf("  core %2d: efficiency=%d logical CPUs=%s", i, g_cores[i].efficiency, list);
    }
    logf("Engine sees %d CPU(s), workerMap for %d core(s)", g_mapCount, g_mapCount);
    for (int i = 0; i < g_mapCount; ++i) {
        logf("  engine core %d -> logical CPU %d", i, g_map[i]);
    }
}

// --------------------------------------------------------------------------------------------
// Thread hooks
// --------------------------------------------------------------------------------------------

static decltype(&GetSystemInfo) real_GetSystemInfo;
static decltype(&SetThreadIdealProcessor) real_SetThreadIdealProcessor;
static decltype(&CreateFileA) real_CreateFileA;
static decltype(&CreateFileW) real_CreateFileW;
static decltype(&GetProcAddress) real_GetProcAddress;

static bool g_isKnownExe;       // grid.exe Steam (TimeDateStamp 0x4ACE02BC)
static char g_workerMapPath[MAX_PATH];

static void WINAPI hook_GetSystemInfo(LPSYSTEM_INFO info)
{
    real_GetSystemInfo(info);
    if (g_cfg.affinity != 2) {
        info->dwNumberOfProcessors = g_mapCount;
        info->dwActiveProcessorMask = (DWORD_PTR(1) << g_mapCount) - 1;
    }
}

static DWORD WINAPI hook_SetThreadIdealProcessor(HANDLE thread, DWORD ideal)
{
    switch (g_cfg.affinity) {
    case 0:
        return 0;
    case 2:
        return real_SetThreadIdealProcessor(thread, ideal);
    default: {
        DWORD mapped = DWORD(g_map[ideal % g_mapCount]);
        DWORD result = real_SetThreadIdealProcessor(thread, mapped);
        return result == DWORD(-1) ? 0 : result;
    }
    }
}

// --------------------------------------------------------------------------------------------
// File rewriting
// --------------------------------------------------------------------------------------------

static const char* base_name(const char* path)
{
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            name = p + 1;
        }
    }
    return name;
}

static void normalize(const char* path, char* out, size_t cap)
{
    lstrcpynA(out, path, int(cap));
    CharLowerA(out);
    for (char* p = out; *p; ++p) {
        if (*p == '\\') *p = '/';
    }
}

static bool is_worker_map(const char* path)
{
    const char* name = base_name(path);
    size_t len = strlen(name);
    return len > 13 && _strnicmp(name, "workerMap", 9) == 0 && _stricmp(name + len - 4, ".xml") == 0;
}

// tracks/<track>/effects.xml and tracks/<track>/<route>/effects.xml
static bool is_track_effects(const char* path)
{
    if (_stricmp(base_name(path), "effects.xml") != 0) {
        return false;
    }
    char lower[MAX_PATH * 2];
    normalize(path, lower, sizeof(lower));
    return strstr(lower, "tracks/") != nullptr;
}

static bool is_track_lod_settings(const char* path)
{
    return _stricmp(base_name(path), "tracklodsettings.xml") == 0;
}

static bool is_application_memory_map(const char* path)
{
    return _stricmp(base_name(path), "applicationMemoryMap.xml") == 0;
}

// Replace the text of <Param name="X">...</Param> for matching overrides inside <Effect> blocks.
static int rewrite_effects(char* in, size_t inLen, char* out, size_t outCap, size_t* outLen, bool trackFile)
{
    size_t o = 0;
    int changes = 0;
    int chain = 0;
    auto emit = [&](const char* p, size_t n) {
        if (o + n <= outCap) {
            memcpy(out + o, p, n);
        }
        o += n;
    };

    const char* p = in;
    const char* end = in + inLen;
    while (p < end) {
        const char* lt = (const char*)memchr(p, '<', end - p);
        if (!lt) {
            emit(p, end - p);
            break;
        }
        emit(p, lt - p);
        p = lt;
        const char* gt = (const char*)memchr(p, '>', end - p);
        if (!gt) {
            emit(p, end - p);
            break;
        }
        size_t tagLen = gt - p + 1;

        if (tagLen > 8 && strncmp(p, "<Effect ", 8) == 0) {
            chain = 0;
            const char* c = strstr(p, "chain=\"Type");
            if (c && c < gt) {
                chain = atoi(c + 11);
            }
            if (gt[-1] == '/') {
                chain = 0;
            }
            emit(p, tagLen);
            p = gt + 1;
            continue;
        }
        if (tagLen >= 9 && strncmp(p, "</Effect>", 9) == 0) {
            chain = 0;
        }
        if (chain && tagLen > 13 && strncmp(p, "<Param name=\"", 13) == 0) {
            const char* nameStart = p + 13;
            const char* nameEnd = (const char*)memchr(nameStart, '"', gt - nameStart);
            const char* close = nameEnd ? strstr(gt, "</Param>") : nullptr;
            const ParamOverride* rule = nullptr;
            if (nameEnd && close) {
                size_t nameLen = nameEnd - nameStart;
                for (int i = 0; i < g_ppCount; ++i) {
                    // track rules only touch track files, menu rules only the global file
                    // (it also holds in-game Type2 effects like damage flashes - left alone)
                    bool scope = trackFile ? g_pp[i].chain == 2 : g_pp[i].chain == 5;
                    if (scope && g_pp[i].chain == chain && strlen(g_pp[i].name) == nameLen &&
                        _strnicmp(g_pp[i].name, nameStart, nameLen) == 0) {
                        rule = &g_pp[i];
                    }
                }
            }
            if (rule) {
                // keep the whitespace around the value
                const char* v = gt + 1;
                while (v < close && (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')) ++v;
                const char* ve = close;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r' || ve[-1] == '\n')) --ve;
                emit(p, v - p);
                emit(rule->value, strlen(rule->value));
                emit(ve, close - ve);
                p = close;
                ++changes;
                continue;
            }
        }
        emit(p, tagLen);
        p = gt + 1;
    }
    *outLen = o;
    return o <= outCap ? changes : -1;
}

// EGO binary XML (files starting with 1A 22 52 72). Every chunk is [u32 tag][u32 size][data],
// tag = 0x72522200 | type:
//   0x1A document { 0x17 strings { 0x1D string pool, 0x1E u32 offsets }, 0x1B nodes, 0x1C attributes }
//   node      = u32 name, text, attrCount, firstAttr, childCount, firstChild  (string/attr/node indices)
//   attribute = u32 name, value                                               (string indices)
// Identical strings are shared, so a value is changed by appending a new string and pointing
// the Param node's text at it. The string pool is padded to 32 bytes like the original.
namespace bxml {

struct Chunk {
    DWORD type;
    const BYTE* data;
    DWORD size;
};

static bool find_chunks(const BYTE* p, size_t len, Chunk* out, int maxOut, int* count)
{
    size_t off = 0;
    while (off + 8 <= len) {
        DWORD tag = *(const DWORD*)(p + off);
        DWORD size = *(const DWORD*)(p + off + 4);
        if ((tag & 0xFFFFFF00u) != 0x72522200u || off + 8 + size > len) {
            return false;
        }
        DWORD type = tag & 0xFF;
        if (type == 0x1A || type == 0x17) {
            if (!find_chunks(p + off + 8, size, out, maxOut, count)) {
                return false;
            }
        } else if (*count < maxOut) {
            out[(*count)++] = {type, p + off + 8, size};
        }
        off += 8 + size;
    }
    return off == len;
}

static void put_chunk(std::vector<BYTE>& v, DWORD type, const void* data, size_t size)
{
    DWORD hdr[2] = {0x72522200u | type, DWORD(size)};
    v.insert(v.end(), (const BYTE*)hdr, (const BYTE*)hdr + 8);
    v.insert(v.end(), (const BYTE*)data, (const BYTE*)data + size);
}

static bool is_binary(const BYTE* p, size_t len)
{
    return len >= 8 && *(const DWORD*)p == 0x7252221Au;
}

// Returns the number of changed params, -1 on a format error.
static int rewrite(const BYTE* in, size_t len, std::vector<BYTE>& out)
{
    Chunk ch[8];
    int count = 0;
    if (!find_chunks(in, len, ch, 8, &count)) {
        return -1;
    }
    const Chunk *pool = nullptr, *offs = nullptr, *nodes = nullptr, *attrs = nullptr;
    for (int i = 0; i < count; ++i) {
        switch (ch[i].type) {
        case 0x1D: pool = &ch[i]; break;
        case 0x1E: offs = &ch[i]; break;
        case 0x1B: nodes = &ch[i]; break;
        case 0x1C: attrs = &ch[i]; break;
        }
    }
    if (!pool || !offs || !nodes || !attrs || count != 4 || nodes->size % 24 || attrs->size % 8 || offs->size % 4) {
        return -1;
    }

    std::vector<BYTE> strPool(pool->data, pool->data + pool->size);
    std::vector<DWORD> strOffs((const DWORD*)offs->data, (const DWORD*)(offs->data + offs->size));
    std::vector<DWORD> node((const DWORD*)nodes->data, (const DWORD*)(nodes->data + nodes->size));
    const DWORD* attr = (const DWORD*)attrs->data;
    DWORD nodeCount = nodes->size / 24, attrCount = attrs->size / 8, strCount = DWORD(strOffs.size());

    // used length of the pool (without padding)
    size_t used = 0;
    for (DWORD i = 0; i < strCount; ++i) {
        if (strOffs[i] >= strPool.size()) {
            return -1;
        }
        size_t e = strOffs[i] + strnlen((const char*)&strPool[strOffs[i]], strPool.size() - strOffs[i]) + 1;
        used = e > used ? e : used;
    }
    strPool.resize(used);

    auto str = [&](DWORD idx) -> const char* {
        return idx < strCount ? (const char*)&strPool[strOffs[idx]] : "";
    };
    auto add_string = [&](const char* s) -> DWORD {
        for (DWORD i = 0; i < DWORD(strOffs.size()); ++i) {
            if (strcmp((const char*)&strPool[strOffs[i]], s) == 0) return i;
        }
        strOffs.push_back(DWORD(strPool.size()));
        strPool.insert(strPool.end(), s, s + strlen(s) + 1);
        return DWORD(strOffs.size() - 1);
    };
    auto attr_value = [&](DWORD n, const char* name) -> const char* {
        DWORD na = node[n * 6 + 2], fa = node[n * 6 + 3];
        for (DWORD k = 0; k < na && fa + k < attrCount; ++k) {
            if (strcmp(str(attr[(fa + k) * 2]), name) == 0) return str(attr[(fa + k) * 2 + 1]);
        }
        return nullptr;
    };

    int changes = 0;
    // walk: Effect (chain Type2) -> any depth -> Param name=X
    std::vector<DWORD> stack;
    for (DWORD n = 0; n < nodeCount; ++n) {
        if (strcmp(str(node[n * 6]), "Effect") != 0) continue;
        const char* chainName = attr_value(n, "chain");
        int chain = chainName && strncmp(chainName, "Type", 4) == 0 ? atoi(chainName + 4) : 0;
        if (chain != 2) continue;
        stack.assign(1, n);
        while (!stack.empty()) {
            DWORD cur = stack.back();
            stack.pop_back();
            DWORD nc = node[cur * 6 + 4], fc = node[cur * 6 + 5];
            for (DWORD k = 0; k < nc && fc + k < nodeCount; ++k) {
                DWORD c = fc + k;
                if (strcmp(str(node[c * 6]), "Param") == 0) {
                    const char* pname = attr_value(c, "name");
                    for (int i = 0; pname && i < g_ppCount; ++i) {
                        if (g_pp[i].chain == 2 && _stricmp(g_pp[i].name, pname) == 0) {
                            node[c * 6 + 1] = add_string(g_pp[i].value);
                            ++changes;
                            break;
                        }
                    }
                } else {
                    stack.push_back(c);
                }
            }
        }
    }

    strPool.resize((strPool.size() + 31) & ~size_t(31), 0);

    std::vector<BYTE> strings;
    put_chunk(strings, 0x1D, strPool.data(), strPool.size());
    put_chunk(strings, 0x1E, strOffs.data(), strOffs.size() * 4);

    std::vector<BYTE> body;
    put_chunk(body, 0x17, strings.data(), strings.size());
    put_chunk(body, 0x1B, node.data(), node.size() * 4);
    put_chunk(body, 0x1C, attrs->data, attrs->size);

    out.clear();
    put_chunk(out, 0x1A, body.data(), body.size());
    return changes;
}

} // namespace bxml

// <memorypool name="HARDWARE_SETTINGS" ... size="16384" ...> -> configured size
static int rewrite_memory_map(const char* in, size_t len, std::string& out)
{
    out.assign(in, len);
    size_t p = out.find("name=\"HARDWARE_SETTINGS\"");
    if (p == std::string::npos) return 0;
    size_t tagStart = out.rfind('<', p), tagEnd = out.find('>', p);
    size_t s = out.find("size=\"", tagStart);
    if (tagStart == std::string::npos || tagEnd == std::string::npos || s == std::string::npos || s > tagEnd) return 0;
    s += 6;
    size_t e = out.find('"', s);
    if (e == std::string::npos || e > tagEnd) return 0;
    char value[32];
    snprintf(value, sizeof(value), "%dkb", g_cfg.hardwareSettingsKB);
    logf("HARDWARE_SETTINGS pool: size=\"%s\" -> \"%s\"", out.substr(s, e - s).c_str(), value);
    out.replace(s, e - s, value);
    return 1;
}

// <SURFACELOD loddistance="30" blend="10" /> -> configured values (attributes replaced in place)
static int rewrite_track_lod(const char* in, size_t len, std::string& out)
{
    out.assign(in, len);
    int changes = 0;
    struct {
        const char* attr;
        int value;
    } items[] = {{"loddistance", g_cfg.lodDistance}, {"blend", g_cfg.lodBlend}};
    size_t tag = out.find("<SURFACELOD");
    if (tag == std::string::npos) return 0;
    for (auto& it : items) {
        if (it.value <= 0) continue;
        size_t tagEnd = out.find('>', tag);
        std::string key = std::string(it.attr) + "=\"";
        size_t s = out.find(key, tag);
        if (s == std::string::npos || s > tagEnd) continue;
        s += key.size();
        size_t e = out.find('"', s);
        if (e == std::string::npos || e > tagEnd) continue;
        char v[32];
        snprintf(v, sizeof(v), "%d", it.value);
        logf("SURFACELOD %s=\"%s\" -> \"%s\"", it.attr, out.substr(s, e - s).c_str(), v);
        out.replace(s, e - s, v);
        ++changes;
    }
    return changes;
}

struct Redirect {
    char from[MAX_PATH];   // lower case, '/' separators
    char to[MAX_PATH];
};
static Redirect g_redirects[512];
static int g_redirectCount;
static CRITICAL_SECTION g_redirectLock;

static bool read_file(const char* path, std::vector<BYTE>& data)
{
    HANDLE file = real_CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD size = GetFileSize(file, nullptr), read = 0;
    bool ok = size != INVALID_FILE_SIZE && size <= 16 * 1024 * 1024;
    if (ok) {
        data.resize(size);
        ok = ReadFile(file, data.data(), size, &read, nullptr) && read == size;
    }
    CloseHandle(file);
    return ok;
}

// Writes data to %TEMP%\Grid_Core_Bloom_Fix\<flattened relative path>
static bool write_copy(const char* path, const void* data, size_t size, char* out)
{
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    strcat_s(dir, "Grid_Core_Bloom_Fix");
    CreateDirectoryA(dir, nullptr);

    char flat[MAX_PATH];
    normalize(path, flat, sizeof(flat));
    const char* rel = strstr(flat, "tracks/");
    if (!rel) rel = strstr(flat, "system/");
    if (!rel) rel = base_name(flat);
    char name[MAX_PATH];
    lstrcpynA(name, rel, sizeof(name));
    for (char* q = name; *q; ++q) {
        if (*q == '/' || *q == ':') *q = '_';
    }
    snprintf(out, MAX_PATH, "%s\\%s", dir, name);

    HANDLE w = real_CreateFileA(out, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(w, data, DWORD(size), &written, nullptr) && written == size;
    CloseHandle(w);
    return ok;
}

static bool make_effects_copy(const char* path, char* out)
{
    std::vector<BYTE> in;
    if (!read_file(path, in)) {
        return false;
    }
    int changes = -1;
    bool redirected = false;
    const char* kind = "text";
    if (bxml::is_binary(in.data(), in.size())) {
        kind = "binary";
        std::vector<BYTE> result;
        changes = bxml::rewrite(in.data(), in.size(), result);
        if (changes > 0) {
            redirected = write_copy(path, result.data(), result.size(), out);
        }
        logf("effects (%s): %s -> %s (%d params, %u -> %u bytes)", kind, path, changes > 0 ? (redirected ? out : "WRITE FAILED") : "unchanged",
             changes, unsigned(in.size()), unsigned(result.size()));
    } else {
        std::vector<char> text(in.begin(), in.end());
        text.push_back(0);
        size_t cap = in.size() * 2 + 4096, outLen = 0;
        std::vector<char> result(cap);
        changes = rewrite_effects(text.data(), in.size(), result.data(), cap, &outLen, true);
        if (changes > 0) {
            redirected = write_copy(path, result.data(), outLen, out);
        }
        logf("effects (%s): %s -> %s (%d params, %u -> %u bytes)", kind, path, changes > 0 ? (redirected ? out : "WRITE FAILED") : "unchanged",
             changes, unsigned(in.size()), unsigned(outLen));
    }
    return redirected;
}

static bool make_track_lod_copy(const char* path, char* out)
{
    std::vector<BYTE> in;
    if (!read_file(path, in)) {
        return false;
    }
    std::string result;
    if (!rewrite_track_lod((const char*)in.data(), in.size(), result)) {
        logf("%s: SURFACELOD not changed", path);
        return false;
    }
    return write_copy(path, result.data(), result.size(), out);
}

static bool make_memory_map_copy(const char* path, char* out)
{
    std::vector<BYTE> in;
    if (!read_file(path, in)) {
        return false;
    }
    std::string result;
    if (!rewrite_memory_map((const char*)in.data(), in.size(), result)) {
        logf("%s: HARDWARE_SETTINGS pool not found, unchanged", path);
        return false;
    }
    return write_copy(path, result.data(), result.size(), out);
}

// Returns the replacement path for a file the game reads, or nullptr.
static const char* find_redirect(const char* path)
{
    if (g_workerMapPath[0] && is_worker_map(path)) {
        return g_workerMapPath;
    }
    bool effects = g_ppCount > 0 && is_track_effects(path);
    bool memoryMap = g_cfg.hardwareSettingsKB > 0 && is_application_memory_map(path);
    bool trackLod = (g_cfg.lodDistance > 0 || g_cfg.lodBlend > 0) && is_track_lod_settings(path);
    if (!effects && !memoryMap && !trackLod) {
        return nullptr;
    }

    char key[MAX_PATH];
    normalize(path, key, sizeof(key));

    EnterCriticalSection(&g_redirectLock);
    const char* result = nullptr;
    bool found = false;
    for (int i = 0; i < g_redirectCount; ++i) {
        if (strcmp(g_redirects[i].from, key) == 0) {
            result = g_redirects[i].to[0] ? g_redirects[i].to : nullptr;
            found = true;
            break;
        }
    }
    if (!found && g_redirectCount < 512) {
        Redirect& r = g_redirects[g_redirectCount++];
        lstrcpynA(r.from, key, MAX_PATH);
        r.to[0] = 0;
        bool ok = effects    ? make_effects_copy(path, r.to)
                  : trackLod ? make_track_lod_copy(path, r.to)
                             : make_memory_map_copy(path, r.to);
        if (ok) {
            result = r.to;
        } else {
            r.to[0] = 0;
        }
    }
    LeaveCriticalSection(&g_redirectLock);
    return result;
}

static bool to_narrow(const WCHAR* path, char* out, int cap)
{
    return WideCharToMultiByte(CP_ACP, 0, path, -1, out, cap, nullptr, nullptr) != 0;
}

static decltype(&GetFileAttributesExA) real_GetFileAttributesExA;
static decltype(&GetFileAttributesA) real_GetFileAttributesA;
static decltype(&FindFirstFileA) real_FindFirstFileA;

static HANDLE WINAPI hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                      DWORD flags, HANDLE templ)
{
    if (name && !(access & GENERIC_WRITE)) {
        if (const char* to = find_redirect(name)) {
            logf("CreateFileA(%s) -> %s", name, to);
            name = to;
        }
    }
    return real_CreateFileA(name, access, share, sa, disposition, flags, templ);
}

static HANDLE WINAPI hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                      DWORD flags, HANDLE templ)
{
    char narrow[MAX_PATH * 2];
    if (name && !(access & GENERIC_WRITE) && to_narrow(name, narrow, sizeof(narrow))) {
        if (const char* to = find_redirect(narrow)) {
            logf("CreateFileW(%s) -> %s", narrow, to);
            return real_CreateFileA(to, access, share, sa, disposition, flags, templ);
        }
    }
    return real_CreateFileW(name, access, share, sa, disposition, flags, templ);
}

// The engine sometimes takes the file size before opening it, keep both consistent.
static BOOL WINAPI hook_GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info)
{
    if (name) {
        if (const char* to = find_redirect(name)) {
            name = to;
        }
    }
    return real_GetFileAttributesExA(name, level, info);
}

static DWORD WINAPI hook_GetFileAttributesA(LPCSTR name)
{
    if (name) {
        if (const char* to = find_redirect(name)) {
            name = to;
        }
    }
    return real_GetFileAttributesA(name);
}

static HANDLE WINAPI hook_FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA data)
{
    const char* to = name && !strpbrk(name, "*?") ? find_redirect(name) : nullptr;
    if (!to) {
        return real_FindFirstFileA(name, data);
    }
    HANDLE h = real_FindFirstFileA(to, data);
    if (h != INVALID_HANDLE_VALUE && data) {
        lstrcpynA(data->cFileName, base_name(name), MAX_PATH);
        data->cAlternateFileName[0] = 0;
    }
    return h;
}

// --------------------------------------------------------------------------------------------
// UTF-8 system code page
// --------------------------------------------------------------------------------------------
// With "Beta: Use Unicode UTF-8 for worldwide language support" the ANSI code page is 65001.
// The game (and its CRT) assume a single/double byte code page: CP_ACP conversions with
// WC_NO_BEST_FIT_CHARS fail with ERROR_INVALID_FLAGS. The game gets code page 1252 instead.

static decltype(&GetACP) real_GetACP;
static decltype(&MultiByteToWideChar) real_MultiByteToWideChar;
static decltype(&WideCharToMultiByte) real_WideCharToMultiByte;
static bool g_utf8System;

static UINT fix_cp(UINT cp)
{
    if (g_utf8System && (cp == CP_ACP || cp == CP_THREAD_ACP || cp == CP_UTF8)) {
        return cp == CP_UTF8 ? cp : 1252;
    }
    return cp;
}

static UINT WINAPI hook_GetACP()
{
    return g_utf8System ? 1252 : real_GetACP();
}

static int WINAPI hook_MultiByteToWideChar(UINT cp, DWORD flags, LPCCH src, int srcLen, LPWSTR dst, int dstLen)
{
    return real_MultiByteToWideChar(fix_cp(cp), flags, src, srcLen, dst, dstLen);
}

static int WINAPI hook_WideCharToMultiByte(UINT cp, DWORD flags, LPCWCH src, int srcLen, LPSTR dst, int dstLen, LPCCH def,
                                           LPBOOL usedDef)
{
    return real_WideCharToMultiByte(fix_cp(cp), flags, src, srcLen, dst, dstLen, def, usedDef);
}

// IDirectDraw7::GetAvailableVidMem (vtable slot 23) clamp.
typedef HRESULT(WINAPI* GetAvailableVidMem_t)(void*, LPDDSCAPS2, LPDWORD, LPDWORD);
static GetAvailableVidMem_t real_GetAvailableVidMem;

static HRESULT WINAPI hook_GetAvailableVidMem(void* self, LPDDSCAPS2 caps, LPDWORD total, LPDWORD free)
{
    HRESULT hr = real_GetAvailableVidMem(self, caps, total, free);
    if (SUCCEEDED(hr) && g_cfg.maxVideoMemoryMB > 0) {
        DWORD limit = DWORD(g_cfg.maxVideoMemoryMB) << 20;
        DWORD t = total ? *total : 0, f = free ? *free : 0;
        if (total && *total > limit) {
            *total = limit;
        }
        if (free && *free > limit) {
            *free = limit;
        }
        logf("GetAvailableVidMem: total %lu MB free %lu MB -> %lu / %lu MB", t >> 20, f >> 20,
             total ? *total >> 20 : 0, free ? *free >> 20 : 0);
    }
    return hr;
}

typedef HRESULT(WINAPI* DirectDrawCreateEx_t)(GUID*, LPVOID*, REFIID, IUnknown*);
static DirectDrawCreateEx_t real_DirectDrawCreateEx;

static HRESULT WINAPI hook_DirectDrawCreateEx(GUID* guid, LPVOID* dd, REFIID iid, IUnknown* outer)
{
    HRESULT hr = real_DirectDrawCreateEx(guid, dd, iid, outer);
    if (SUCCEEDED(hr) && dd && *dd && !real_GetAvailableVidMem) {
        void** vtable = *(void***)*dd;
        DWORD old;
        if (VirtualProtect(&vtable[23], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            real_GetAvailableVidMem = GetAvailableVidMem_t(vtable[23]);
            vtable[23] = (void*)&hook_GetAvailableVidMem;
            VirtualProtect(&vtable[23], sizeof(void*), old, &old);
        }
    }
    return hr;
}

static FARPROC WINAPI hook_GetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC proc = real_GetProcAddress(module, name);
    if (proc && HIWORD(name) && g_cfg.maxVideoMemoryMB > 0 && strcmp(name, "DirectDrawCreateEx") == 0) {
        real_DirectDrawCreateEx = DirectDrawCreateEx_t(proc);
        return FARPROC(&hook_DirectDrawCreateEx);
    }
    return proc;
}

// --------------------------------------------------------------------------------------------
// D3D9 display mode filter
// --------------------------------------------------------------------------------------------

typedef UINT(WINAPI* GetAdapterModeCount_t)(IDirect3D9*, UINT, D3DFORMAT);
typedef HRESULT(WINAPI* EnumAdapterModes_t)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
typedef HRESULT(WINAPI* GetAdapterDisplayMode_t)(IDirect3D9*, UINT, D3DDISPLAYMODE*);

static GetAdapterModeCount_t real_GetAdapterModeCount;
static EnumAdapterModes_t real_EnumAdapterModes;
static GetAdapterDisplayMode_t real_GetAdapterDisplayMode;

struct ModeList {
    bool used;
    UINT adapter;
    D3DFORMAT format;
    UINT count;
    D3DDISPLAYMODE modes[128];
};
static ModeList g_modeLists[8];
static CRITICAL_SECTION g_modeLock;

static bool same_res(const D3DDISPLAYMODE& a, const D3DDISPLAYMODE& b)
{
    return a.Width == b.Width && a.Height == b.Height;
}

static void build_mode_list(IDirect3D9* d3d, ModeList& list)
{
    D3DDISPLAYMODE desktop = {};
    real_GetAdapterDisplayMode(d3d, list.adapter, &desktop);

    UINT total = real_GetAdapterModeCount(d3d, list.adapter, list.format);
    static D3DDISPLAYMODE all[1024];
    UINT n = 0;
    for (UINT i = 0; i < total && n < 1024; ++i) {
        D3DDISPLAYMODE m;
        if (FAILED(real_EnumAdapterModes(d3d, list.adapter, list.format, i, &m))) {
            continue;
        }
        if (int(m.Width) < g_cfg.minWidth || m.Height < 600 || m.Height > m.Width) {
            continue;
        }
        all[n++] = m;
    }

    // Distinct resolutions, largest first; the desktop resolution is always kept.
    static D3DDISPLAYMODE res[1024];
    UINT resCount = 0;
    for (UINT i = 0; i < n; ++i) {
        bool found = false;
        for (UINT j = 0; j < resCount && !found; ++j) {
            found = same_res(res[j], all[i]);
        }
        if (!found) {
            res[resCount++] = all[i];
        }
    }
    for (UINT i = 1; i < resCount; ++i) {
        D3DDISPLAYMODE m = res[i];
        UINT j = i;
        while (j > 0 && UINT64(res[j - 1].Width) * res[j - 1].Height < UINT64(m.Width) * m.Height) {
            res[j] = res[j - 1];
            --j;
        }
        res[j] = m;
    }
    UINT maxRes = g_cfg.maxResolutions > 0 ? UINT(g_cfg.maxResolutions) : 1024;
    if (resCount > maxRes) {
        bool desktopKept = false;
        for (UINT i = 0; i < maxRes; ++i) {
            desktopKept |= same_res(res[i], desktop);
        }
        if (!desktopKept) {
            for (UINT i = maxRes; i < resCount; ++i) {
                if (same_res(res[i], desktop)) {
                    res[maxRes - 1] = res[i];
                    break;
                }
            }
        }
        resCount = maxRes;
    }

    // Per resolution: desktop refresh rate and 60 Hz, otherwise the highest one.
    list.count = 0;
    for (int r = int(resCount) - 1; r >= 0; --r) { // ascending, like D3D9 does
        UINT best = 0;
        bool haveDesktop = false, have60 = false;
        for (UINT i = 0; i < n; ++i) {
            if (!same_res(all[i], res[r])) {
                continue;
            }
            best = all[i].RefreshRate > best ? all[i].RefreshRate : best;
            haveDesktop |= all[i].RefreshRate == desktop.RefreshRate;
            have60 |= all[i].RefreshRate == 60;
        }
        UINT rates[2];
        UINT rateCount = 0;
        if (have60) {
            rates[rateCount++] = 60;
        }
        if (haveDesktop && desktop.RefreshRate != 60) {
            rates[rateCount++] = desktop.RefreshRate;
        }
        if (rateCount == 0) {
            rates[rateCount++] = best;
        }
        for (UINT k = 0; k < rateCount && list.count < 128; ++k) {
            D3DDISPLAYMODE m = res[r];
            m.RefreshRate = rates[k];
            m.Format = list.format;
            list.modes[list.count++] = m;
        }
    }

    logf("D3D9 modes (adapter %u, format %d): %u reported, %u kept (desktop %ux%u@%u)", list.adapter, int(list.format),
         total, list.count, desktop.Width, desktop.Height, desktop.RefreshRate);
    for (UINT i = 0; i < list.count; ++i) {
        logf("  %ux%u @ %u Hz", list.modes[i].Width, list.modes[i].Height, list.modes[i].RefreshRate);
    }
}

static ModeList* get_mode_list(IDirect3D9* d3d, UINT adapter, D3DFORMAT format)
{
    EnterCriticalSection(&g_modeLock);
    ModeList* result = nullptr;
    for (auto& list : g_modeLists) {
        if (list.used && list.adapter == adapter && list.format == format) {
            result = &list;
            break;
        }
    }
    if (!result) {
        for (auto& list : g_modeLists) {
            if (!list.used) {
                list.used = true;
                list.adapter = adapter;
                list.format = format;
                build_mode_list(d3d, list);
                result = &list;
                break;
            }
        }
    }
    LeaveCriticalSection(&g_modeLock);
    return result;
}

static UINT WINAPI hook_GetAdapterModeCount(IDirect3D9* d3d, UINT adapter, D3DFORMAT format)
{
    ModeList* list = get_mode_list(d3d, adapter, format);
    return list && list->count ? list->count : real_GetAdapterModeCount(d3d, adapter, format);
}

static HRESULT WINAPI hook_EnumAdapterModes(IDirect3D9* d3d, UINT adapter, D3DFORMAT format, UINT index, D3DDISPLAYMODE* mode)
{
    ModeList* list = get_mode_list(d3d, adapter, format);
    if (!list || !list->count) {
        return real_EnumAdapterModes(d3d, adapter, format, index, mode);
    }
    if (!mode) {
        return D3DERR_INVALIDCALL;
    }
    if (index >= list->count) {
        return D3DERR_INVALIDCALL;
    }
    *mode = list->modes[index];
    return D3D_OK;
}

typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);
static Direct3DCreate9_t real_Direct3DCreate9;

static IDirect3D9* WINAPI hook_Direct3DCreate9(UINT sdk)
{
    IDirect3D9* d3d = real_Direct3DCreate9(sdk);
    if (d3d && !real_EnumAdapterModes) {
        void** vtable = *(void***)d3d;
        DWORD old;
        if (VirtualProtect(&vtable[6], 3 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            real_GetAdapterModeCount = GetAdapterModeCount_t(vtable[6]);
            real_EnumAdapterModes = EnumAdapterModes_t(vtable[7]);
            real_GetAdapterDisplayMode = GetAdapterDisplayMode_t(vtable[8]);
            vtable[6] = (void*)&hook_GetAdapterModeCount;
            vtable[7] = (void*)&hook_EnumAdapterModes;
            VirtualProtect(&vtable[6], 3 * sizeof(void*), old, &old);
            logf("IDirect3D9 mode enumeration hooked");
        }
    }
    return d3d;
}

// --------------------------------------------------------------------------------------------
// IAT patching
// --------------------------------------------------------------------------------------------

static bool patch_iat(HMODULE module, const char* dll, const char* func, void* hook, void** original)
{
    auto base = (BYTE*)module;
    auto dos = PIMAGE_DOS_HEADER(base);
    auto nt = PIMAGE_NT_HEADERS(base + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        return false;
    }

    for (auto imp = PIMAGE_IMPORT_DESCRIPTOR(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) {
            continue;
        }
        auto names = PIMAGE_THUNK_DATA(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto iat = PIMAGE_THUNK_DATA(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            auto ibn = PIMAGE_IMPORT_BY_NAME(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, func) != 0) {
                continue;
            }
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            *original = (void*)iat->u1.Function;
            iat->u1.Function = (DWORD_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// The game's own system/workerMap4Core.xml (2Core / workerMap.xml on small CPUs) is used for
// whatever map hardware_settings_restrictions.xml / hardware_settings_config.xml ask for.
static void select_worker_map()
{
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char* slash = strrchr(dir, '\\')) {
        *slash = 0;
    }
    const char* name = g_mapCount >= 4 ? "workerMap4Core.xml" : g_mapCount >= 2 ? "workerMap2Core.xml" : "workerMap.xml";
    snprintf(g_workerMapPath, sizeof(g_workerMapPath), "%s\\system\\%s", dir, name);
    if (GetFileAttributesA(g_workerMapPath) == INVALID_FILE_ATTRIBUTES) {
        logf("%s not found, workerMap redirect disabled", g_workerMapPath);
        g_workerMapPath[0] = 0;
        return;
    }
    logf("Worker map: %s", g_workerMapPath);
}

// grid.exe (Steam): byte 0xDAFF08 = use the virtual memory manager (cleared by -novirtual).
static void apply_virtual_memory(bool laa)
{
    int mode = g_cfg.virtualMemory;
    if (mode < 0) {
        mode = laa ? 1 : 0;
    }
    if (!g_isKnownExe) {
        logf("Virtual memory: unknown grid.exe, left as is (use -novirtual if the game fails to start)");
        return;
    }
    BYTE* flag = (BYTE*)0xDAFF08;
    DWORD old;
    if (!VirtualProtect(flag, 1, PAGE_READWRITE, &old)) {
        logf("Virtual memory: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    BYTE before = *flag;
    if (before > 1) {
        VirtualProtect(flag, 1, old, &old);
        logf("Virtual memory: unexpected flag value %u, left as is", before);
        return;
    }
    if (mode == 0) {
        *flag = 0;
    }
    VirtualProtect(flag, 1, old, &old);
    logf("Virtual memory manager: %s (flag %u -> %u, LargeAddressAware=%s)%s", mode ? "on (815 MB reservation)" : "off (-novirtual)",
         before, *flag, laa ? "yes" : "no", mode && !laa ? " WARNING: may fail without LAA" : "");
}

static void install()
{
    load_config();
    open_log();
    logf("Grid_Core_Bloom_Fix loaded");

    HMODULE exe = GetModuleHandleA(nullptr);
    auto nt = PIMAGE_NT_HEADERS((BYTE*)exe + PIMAGE_DOS_HEADER(exe)->e_lfanew);
    bool laa = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    // Steam build; also matches a DRM-free copy of it (same timestamp, the .bind section may be gone).
    // The virtual memory flag is checked as well before it is touched.
    g_isKnownExe = nt->FileHeader.TimeDateStamp == 0x4ACE02BC && nt->OptionalHeader.SizeOfImage >= 0xDB0000;
    logf("Executable timestamp %08lX size %08lX (%s), LargeAddressAware=%s", nt->FileHeader.TimeDateStamp,
         nt->OptionalHeader.SizeOfImage, g_isKnownExe ? "GRID Steam" : "unknown version", laa ? "yes" : "no");

    OSVERSIONINFOW os = {sizeof(os)};
    typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW*);
    if (auto rtl = RtlGetVersion_t(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion"))) {
        rtl(&os);
    }
    UINT acp = GetACP();
    g_utf8System = g_cfg.codePage && acp == CP_UTF8;
    logf("Windows %lu.%lu build %lu, ANSI code page %u%s", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, acp,
         g_utf8System ? " (UTF-8 -> game gets 1252)" : "");

    detect_topology();
    apply_virtual_memory(laa);

    InitializeCriticalSection(&g_redirectLock);
    load_postprocess_config();
    logf("Post process: %s, %d parameter rules", g_cfg.postProcess ? "on" : "off", g_ppCount);
    for (int i = 0; i < g_ppCount; ++i) {
        logf("  %s = %s", g_pp[i].name, g_pp[i].value);
    }

    if (g_cfg.workerMap) {
        select_worker_map();
    }

    struct {
        const char* name;
        void* hook;
        void** original;
    } hooks[] = {
        {"GetSystemInfo", (void*)&hook_GetSystemInfo, (void**)&real_GetSystemInfo},
        {"SetThreadIdealProcessor", (void*)&hook_SetThreadIdealProcessor, (void**)&real_SetThreadIdealProcessor},
        {"CreateFileA", (void*)&hook_CreateFileA, (void**)&real_CreateFileA},
        {"CreateFileW", (void*)&hook_CreateFileW, (void**)&real_CreateFileW},
        {"GetFileAttributesExA", (void*)&hook_GetFileAttributesExA, (void**)&real_GetFileAttributesExA},
        {"GetFileAttributesA", (void*)&hook_GetFileAttributesA, (void**)&real_GetFileAttributesA},
        {"FindFirstFileA", (void*)&hook_FindFirstFileA, (void**)&real_FindFirstFileA},
        {"GetProcAddress", (void*)&hook_GetProcAddress, (void**)&real_GetProcAddress},
        {"GetACP", (void*)&hook_GetACP, (void**)&real_GetACP},
        {"MultiByteToWideChar", (void*)&hook_MultiByteToWideChar, (void**)&real_MultiByteToWideChar},
        {"WideCharToMultiByte", (void*)&hook_WideCharToMultiByte, (void**)&real_WideCharToMultiByte},
    };

    for (auto& h : hooks) {
        bool ok = patch_iat(exe, "KERNEL32.dll", h.name, h.hook, h.original);
        logf("hook %-24s %s", h.name, ok ? "ok" : "not found");
    }

    // Fallbacks so that hooks never call a null pointer.
    if (!real_GetSystemInfo) real_GetSystemInfo = &GetSystemInfo;
    if (!real_SetThreadIdealProcessor) real_SetThreadIdealProcessor = &SetThreadIdealProcessor;
    if (!real_CreateFileA) real_CreateFileA = &CreateFileA;
    if (!real_CreateFileW) real_CreateFileW = &CreateFileW;
    if (!real_GetProcAddress) real_GetProcAddress = &GetProcAddress;
    if (!real_GetFileAttributesExA) real_GetFileAttributesExA = &GetFileAttributesExA;
    if (!real_GetFileAttributesA) real_GetFileAttributesA = &GetFileAttributesA;
    if (!real_FindFirstFileA) real_FindFirstFileA = &FindFirstFileA;
    if (!real_GetACP) real_GetACP = &GetACP;
    if (!real_MultiByteToWideChar) real_MultiByteToWideChar = &MultiByteToWideChar;
    if (!real_WideCharToMultiByte) real_WideCharToMultiByte = &WideCharToMultiByte;

    if (g_cfg.filterModes) {
        InitializeCriticalSection(&g_modeLock);
        bool ok = patch_iat(exe, "d3d9.dll", "Direct3DCreate9", (void*)&hook_Direct3DCreate9, (void**)&real_Direct3DCreate9);
        logf("hook %-24s %s", "Direct3DCreate9", ok ? "ok" : "not found");
    }

    if (g_cfg.dpiAware) {
        typedef BOOL(WINAPI * SetProcessDPIAware_t)();
        if (auto f = SetProcessDPIAware_t(GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware"))) {
            f();
        }
    }
}

// --------------------------------------------------------------------------------------------
// dinput8 proxy
// --------------------------------------------------------------------------------------------

typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DirectInput8Create_t real_DirectInput8Create;

static void load_real_dinput8()
{
    if (real_DirectInput8Create) {
        return;
    }
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH); // SysWOW64 for a 32-bit process
    strcat_s(path, "\\dinput8.dll");
    HMODULE dll = LoadLibraryA(path);
    if (dll) {
        real_DirectInput8Create = DirectInput8Create_t(GetProcAddress(dll, "DirectInput8Create"));
    }
    logf("system dinput8: %s", real_DirectInput8Create ? path : "NOT FOUND");
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid, LPVOID* out, LPUNKNOWN outer)
{
    load_real_dinput8();
    if (!real_DirectInput8Create) {
        return E_FAIL;
    }
    return real_DirectInput8Create(inst, version, iid, out, outer);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dllDir, MAX_PATH);
        if (char* slash = strrchr(g_dllDir, '\\')) {
            *slash = 0;
        }
        install();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) {
            fclose(g_log);
        }
    }
    return TRUE;
}
