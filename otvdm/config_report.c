/*
 * otvdm.ini config report (launcher side)
 *
 * Self-contained implementation compiled into otvdm.exe / otvdmw.exe.
 * It deliberately does NOT depend on krnl386.exe16 so that the report can
 * be produced even when the 16-bit subsystem fails to initialize, and so
 * that linking the launcher has no extra symbol requirements.
 *
 * When enabled ([otvdm] ConfigReport=1 in otvdm.ini, or OTVDM_CONFIG_REPORT=1
 * in the environment when the ini cannot be found) it writes a log file
 * "otvdm_config_report.log" next to otvdm.exe (falling back to the current
 * directory, then stderr) containing:
 *   - the resolved otvdm.ini path and whether the file actually exists
 *   - every setting lookup performed by the launcher itself, showing either
 *     the value found in the ini or the default used when the key is missing
 *   - the win16 app about to be launched, plus a summary of lookups
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CHAR cfg_filename[MAX_PATH];
static BOOL cfg_init_done = FALSE;
static BOOL cfg_enabled = FALSE;
static LONG cfg_count = 0;
static FILE *cfg_stream = NULL;
static BOOL launcher_summary_written = FALSE;

/* forward declarations: MSVC/C23-style compilers treat an implicit function
 * declaration as 'int f()', which then collides with the real definitions
 * below (C2371: redefinition; different basic types). Declaring them up
 * front keeps every use type-checked. */
static void cfg_init( void );
static int cfg_get_int(LPCSTR section, LPCSTR key, INT def, BOOL *found);
void config_report_launch( LPCSTR appname );

/* single shared log file: the launcher (this module) and krnl386.exe16 are
 * separate PE images loaded into the SAME process and both append to
 * otvdm_config_report.log next to the exe. krnl386 used to keep its own
 * static FILE* and write a second handle to the same path; two independent
 * append streams interleave unpredictably and duplicate whole sections of
 * the report (observed in the field: two headers, two "about to launch"
 * lines, lookups continuing after the summary). To serialize the two
 * halves we publish our stream here so krnl386 writes through it. */
FILE *config_report_shared_stream = NULL;
volatile LONG config_report_shared_lock = 0;

#define CFG_LOCK_ACQUIRE() while (InterlockedCompareExchange(&config_report_shared_lock, 1, 0)) Sleep(0)
#define CFG_LOCK_RELEASE() InterlockedExchange(&config_report_shared_lock, 0)

/* exported (see wine.def / otvdm.def): krnl386 resolves these with
 * GetProcAddress on the main exe and routes its logging through the
 * launcher's stream + lock. Returns TRUE when the report is enabled. */
BOOL get_config_report_streams(FILE **out_stream, volatile LONG **out_lock)
{
    cfg_init();
    if (!cfg_enabled || !cfg_stream) return FALSE;
    *out_stream = cfg_stream;
    *out_lock = &config_report_shared_lock;
    return TRUE;
}

/* accessor used by krnl386/conf.c (resolved with GetProcAddress at runtime)
 * so the 16-bit side mirrors the launcher's report decision */
BOOL get_config_report_enabled(void)
{
    cfg_init();                       /* parses the ini/env and sets cfg_enabled */
    return cfg_enabled;
}

/* read a string setting directly from the ini (bypassing krnl386, which may
 * not be loaded yet at this point); 'found' reports whether the key exists */
static DWORD cfg_get_string(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR ret, DWORD size, BOOL *found)
{
    DWORD len;
    lstrcpynA( ret, def, size );
    if (!cfg_filename[0])
    {
        *found = FALSE;
        return 0;
    }
    len = GetPrivateProfileStringA( section, key, "\x01__missing__", ret, size, cfg_filename );
    if (!len || !strcmp( ret, "\x01__missing__" ))
    {
        lstrcpynA( ret, def, size );
        *found = FALSE;
        return 0;
    }
    *found = TRUE;
    return len;
}

static int cfg_get_int(LPCSTR section, LPCSTR key, INT def, BOOL *found)
{
    CHAR buf[32];
    cfg_get_string( section, key, NULL, buf, sizeof(buf), found );
    if (!*found) return def;
    return atoi( buf );
}

static void cfg_write( const char *fmt, ... )
{
    va_list ap;

    if (!cfg_enabled) return;
    if (!cfg_stream)
    {
        CHAR path[MAX_PATH];
        DWORD dir;
        LPSTR slash;

        /* prefer next to the exe, fall back to cwd, finally stderr */
        GetModuleFileNameA( NULL, path, MAX_PATH );
        if ((slash = strrchr( path, '\\' )))
        {
            dir = (DWORD)(slash - path);
            memcpy( path + dir, "\\otvdm_config_report.log", sizeof("\\otvdm_config_report.log") );
            cfg_stream = fopen( path, "a" );
        }
        if (!cfg_stream) cfg_stream = fopen( "otvdm_config_report.log", "a" );
        if (!cfg_stream) cfg_stream = stderr;
        config_report_shared_stream = cfg_stream;   /* publish for krnl386 */
    }
    CFG_LOCK_ACQUIRE();
    va_start( ap, fmt );
    vfprintf( cfg_stream, fmt, ap );
    va_end( ap );
    fflush( cfg_stream );
    CFG_LOCK_RELEASE();
}

/* report one setting lookup. Every distinct lookup is printed exactly once,
 * numbered; repeats of an identical line are collapsed into a 'seen xN'
 * counter (AdjustDPI and IAmNotNT are read per GDI/window operation and can
 * otherwise flood the log while the app runs). */
static struct { CHAR line[192]; LONG n; } cfg_seen[32];
static int cfg_seen_count = 0;

static void cfg_lookup( LPCSTR section, LPCSTR key, const CHAR *value_or_default, BOOL found )
{
    CHAR text[192];
    int i;

    if (!cfg_enabled) return;

    _snprintf( text, sizeof(text), "[%s] %s = %s (%s)",
               section ? section : "?", key ? key : "?",
               value_or_default ? value_or_default : "(null)",
               found ? "found in ini" : "NOT FOUND - using default" );
    text[ sizeof(text) - 1 ] = 0;

    for (i = 0; i < cfg_seen_count; i++)
    {
        if (!strcmp( cfg_seen[i].line, text ))
        {
            cfg_seen[i].n++;
            return;
        }
    }
    if (cfg_seen_count < (int)(sizeof(cfg_seen)/sizeof(cfg_seen[0])))
    {
        lstrcpynA( cfg_seen[cfg_seen_count].line, text, sizeof(cfg_seen[0].line) );
        cfg_seen[cfg_seen_count].n = 1;
        cfg_seen_count++;
    }
    cfg_write( "otvdm: [%ld] %s\n", InterlockedIncrement( &cfg_count ), text );
}

/* print the repeat counters for deduped lookups; called once from the final
 * launcher summary so every line appears exactly once in the log */
static void cfg_flush_dedupes( void )
{
    int i;
    for (i = 0; i < cfg_seen_count; i++)
    {
        if (cfg_seen[i].n > 1)
            cfg_write( "otvdm:      ^ '%s' was requested %ld times in total (repeats collapsed)\n",
                       cfg_seen[i].line, cfg_seen[i].n );
    }
}

static BOOL cfg_env_flag(void)
{
    CHAR env[16];
    DWORD len = GetEnvironmentVariableA( "OTVDM_CONFIG_REPORT", env, sizeof(env) );
    return len && (atoi(env) || !lstrcmpiA(env, "TRUE") || !lstrcmpiA(env, "yes"));
}

/* parse the ini/env and emit the header + per-setting lookups exactly once */
static void cfg_init( void )
{
    CHAR buf[32];
    CHAR env[MAX_PATH];
    DWORD len = 0;
    BOOL found, ini_found;
    int i;
    static const CHAR *keys[] = {
        "ConfigReport", "FixScreenSize", "DIBPalette", "EnableVisualStyle",
        "DefaultInstallDir", "UseGlobalLocal", "SharedWow", "ConsoleBuffer",
    };

    if (cfg_init_done) return;
    cfg_init_done = TRUE;

    /* resolve the ini path the same way krnl386/conf.c does: next to the
     * exe, with an OTVDM_INI environment override for diagnostics */
    len = GetEnvironmentVariableA( "OTVDM_INI", env, sizeof(env) );
    if (len && len < sizeof(env))
        lstrcpynA( cfg_filename, env, sizeof(cfg_filename) );
    else
    {
        GetModuleFileNameA( NULL, cfg_filename, MAX_PATH );
        LPSTR last = strrchr( cfg_filename, '\\' );
        if (last) lstrcpynA( last + 1, "otvdm.ini", MAX_PATH - (int)(last + 1 - cfg_filename) );
    }
    ini_found = (GetFileAttributesA( cfg_filename ) != INVALID_FILE_ATTRIBUTES);

    /* enable via ini [otvdm] ConfigReport, or via the OTVDM_CONFIG_REPORT
     * environment variable regardless of whether the ini was found (so a
     * missing/misplaced ini can still be diagnosed and forced into the log) */
    cfg_get_string( "otvdm", "ConfigReport", "0", buf, sizeof(buf), &found );
    if (found && (atoi(buf) || !lstrcmpiA(buf, "TRUE") || !lstrcmpiA(buf, "yes")))
        cfg_enabled = TRUE;
    if (!cfg_enabled && cfg_env_flag())
        cfg_enabled = TRUE;
    if (!cfg_enabled) return;

    cfg_write( "otvdm: ==== otvdm.ini config report (launcher) ====\n" );
    cfg_write( "otvdm: ini path: '%s'%s\n", cfg_filename,
               (len && len < sizeof(env)) ? " (overridden by OTVDM_INI env var)" : "" );
    cfg_write( "otvdm: ini file %s\n",
               ini_found ? "FOUND" : "NOT FOUND (all settings fall back to defaults)" );

    /* report the launcher-side settings we consult ourselves */
    for (i = 0; i < (int)(sizeof(keys)/sizeof(keys[0])); i++)
    {
        DWORD r = cfg_get_string( "otvdm", keys[i], "", buf, sizeof(buf), &found );
        cfg_lookup( "otvdm", keys[i], r ? buf : "", found );
    }
    cfg_get_int( "otvdm", "FixScreenSize", 0, &found );
    cfg_lookup( "otvdm", "FixScreenSize(int)", found ? "1" : "0", found );

    /* DPI diagnostics: the process awareness context tells us whether
     * Windows hands this process physical pixels (aware/unaware+bitblt) or
     * DPI-logical pixels (unaware virtualized). On a 125%-scaled primary
     * monitor an unaware process sees SM_CXSCREEN = 3440/1.25 = 2752, which
     * is exactly what the krnl386 probes reported — while the VDM host
     * window itself lives in physical pixels. Log it once per process. */
    {
        HMODULE hUser = GetModuleHandleA( "user32.dll" );
        int aware = -1;
        BOOL dpi10 = FALSE;
        typedef BOOL (WINAPI *pSetProcessDpiAwarenessContext_t)(HANDLE);
        pSetProcessDpiAwarenessContext_t pSetCtx = NULL;
        if (hUser)
            pSetCtx = (pSetProcessDpiAwarenessContext_t)(void *)GetProcAddress(hUser, "SetProcessDpiAwarenessContext");
        /* probe + set: PerMonitorV2 (-4) if the OS supports it, else System Aware (-3).
         * SetProcessDpiAwarenessContext returns FALSE when awareness was
         * already fixed earlier (e.g. by a manifest), so also read back the
         * current context via the legacy API result and GetDpiForWindow. */
        if (pSetCtx && pSetCtx((HANDLE)-4)) dpi10 = TRUE;
        else if (pSetCtx) pSetCtx((HANDLE)-3);
        {
            HDC dc = GetDC(NULL);
            int horz = dc ? GetDeviceCaps(dc, HORZRES) : -1;
            int vert = dc ? GetDeviceCaps(dc, VERTRES) : -1;
            int dpx  = dc ? GetDeviceCaps(dc, DESKTOPVERTRES) : -1;   /* physical px on Win8.1+ */
            float scale = (dpx > 0 && vert > 0) ? (float)dpx / (float)vert : 0.0f;
            cfg_write( "otvdm: note: launcher DPI: ctx=%s SetProcessDPIAware->%d desktop DC %dx%d DESKTOPVERTRES=%d scale=%.3f\n",
                       dpi10 ? "PerMonitorV2 accepted (Win10 1703+)" : "ctx API absent/already-set (legacy)",
                       SetProcessDPIAware() ? 1 : 0, horz, vert, dpx, scale );
            aware = (horz > 0) ? (int)(scale * 1000) : -1;  /* stash for dedupe-free single line */
            (void)aware;
            if (dc) ReleaseDC(NULL, dc);
        }
    }
    cfg_write( "otvdm: ---- launcher section done (%ld lookups so far) ----\n", cfg_count );
    /* krnl386.exe16 is a separate PE image loaded into this same process.
     * It routes every lookup report through our shared stream + lock (see
     * get_config_report_streams / krnl386/conf.c), so there is exactly ONE
     * otvdm_config_report.log and no interleaved duplicate sections. */
    cfg_write( "otvdm: note: krnl386 lookups follow below, sharing this log via the launcher's stream\n" );
}

/* final summary: written by winevdm.c AFTER exec16() returns (i.e. once the
 * win16 app has been launched and all of its config lookups have been
 * reported). This guarantees the summary is the last thing in the log and
 * appears exactly once per process. */
void config_report_finish( void )
{
    if (!cfg_enabled) return;
    if (launcher_summary_written) return;
    launcher_summary_written = TRUE;
    cfg_flush_dedupes();
    cfg_write( "otvdm: ---- config report finished: %ld distinct setting lookups logged (launcher + krnl386) ----\n",
               cfg_count );
}

void config_report_launch( LPCSTR appname )
{
    cfg_init();                         /* one-time: parse ini/env, write header */
    if (!cfg_enabled || !appname) return;
    cfg_write( "otvdm: about to launch win16 app: '%s'\n", appname );
}
