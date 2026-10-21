#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

static BOOL init;

static CHAR filename[MAX_PATH];
static CRITICAL_SECTION critical_section;

void init_config(void);

/* config report: when enabled (otvdm.ini [otvdm] ConfigReport=1) we print
 * the ini path and every setting lookup so users can verify otvdm.ini is
 * found and which values are read before launching a win16 app. */
static BOOL config_report_enabled = FALSE;
static FILE *config_report_stream = NULL;
static volatile LONG *config_report_lock = NULL;
static LONG config_report_count = 0;
static BOOL config_report_header_written = FALSE;
static BOOL config_report_summary_written = FALSE;

/* the launcher (otvdm.exe) enables its copy of the report in
 * otvdm/config_report.c before krnl386 is even loaded; mirror that decision
 * here so both halves write to the same log without requiring a second
 * ConfigReport key read. The symbol lives in the launcher exe, which always
 * loads krnl386.exe16, so it is guaranteed present at runtime; resolve it
 * dynamically to avoid a load-time import on an exe that may be linked
 * against us by other tools. */
static BOOL launcher_report_enabled(void)
{
    typedef BOOL (*get_config_report_enabled_t)(void);
    static get_config_report_enabled_t pfn = NULL;
    static int tried = 0;

    if (!tried)
    {
        tried = 1;
        pfn = (get_config_report_enabled_t)GetProcAddress(GetModuleHandleA(NULL),
                                                          "get_config_report_enabled");
    }
    return pfn ? pfn() : FALSE;
}

DWORD WINAPI krnl386_get_config_string(LPCSTR appname, LPCSTR keyname, LPCSTR def, LPSTR ret, DWORD size);
DWORD WINAPI krnl386_get_config_int(LPCSTR appname, LPCSTR keyname, INT def);

/* SINGLE SHARED LOG: the launcher and krnl386 are two PE images inside the
 * same process and both used to fopen(otvdm_config_report.log, "a") with
 * their own stdio buffers. Two independent append streams on one file
 * interleave unpredictably and duplicated whole report sections (two
 * headers, two 'about to launch' lines, lookups after the summary). Ask the
 * launcher for its FILE* + spinlock and write through them instead; fall
 * back to our own handle only when running under a host exe that does not
 * export the accessor (e.g. wine preloader builds). */
static BOOL launcher_shared_stream(FILE **out, volatile LONG **lock)
{
    typedef BOOL (*get_streams_t)(FILE **, volatile LONG **);
    static get_streams_t pfn = NULL;
    static int tried = 0;

    if (!tried)
    {
        tried = 1;
        pfn = (get_streams_t)GetProcAddress(GetModuleHandleA(NULL),
                                            "get_config_report_streams");
    }
    if (!pfn) return FALSE;
    return pfn(out, lock);
}

static void config_report_write(const char *fmt, ...)
{
    va_list ap;

    if (!config_report_stream)
        return;
    if (config_report_lock)
    {
        while (InterlockedCompareExchange(config_report_lock, 1, 0)) Sleep(0);
    }
    va_start(ap, fmt);
    vfprintf(config_report_stream, fmt, ap);
    va_end(ap);
    fflush(config_report_stream);
    if (config_report_lock)
        InterlockedExchange(config_report_lock, 0);
}

static void config_report_open(void)
{
    CHAR path[MAX_PATH];

    if (config_report_stream)
        return;
    /* prefer the launcher's shared stream + lock (single serialized log) */
    if (launcher_shared_stream(&config_report_stream, &config_report_lock))
        return;
    /* IMPORTANT: krnl386's 'filename' holds the path of otvdm.ini, not of
     * any exe, so appending a log name to it would write into a bogus
     * '...\Release\otvdm.ini\otvdm_config_report.log' path that can never
     * exist. Always anchor the log on the real executable directory
     * (krnl386 runs inside otvdm.exe / winevdm.com), then fall back to the
     * current directory and finally stderr so the report is never lost. */
    path[0] = 0;
    if (GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH))
    {
        LPSTR slash = strrchr(path, '\\');
        if (slash)
            strcpy(slash + 1, "otvdm_config_report.log");
        else
            path[0] = 0;
    }
    if (path[0])
        config_report_stream = fopen(path, "a");
    if (!config_report_stream)
        config_report_stream = fopen("otvdm_config_report.log", "a");
    if (!config_report_stream)
        config_report_stream = stderr;
}

void config_report_enable_check(void)
{
    CHAR buf[16];
    CHAR env[16];
    DWORD len;

    if (!init)
        init_config();
    if (config_report_enabled)
        return;
    /* the launcher already parsed [otvdm] ConfigReport before krnl386 was
     * loaded (see otvdm/config_report.c); honour its decision so the two
     * halves of the report always agree */
    if (launcher_report_enabled())
    {
        config_report_enabled = TRUE;
        config_report_open();
        config_report_write("otvdm: ---- krnl386 section: config report enabled (mirrored from launcher) ----\n");
        return;
    }
    EnterCriticalSection(&critical_section);
    len = GetPrivateProfileStringA("otvdm", "ConfigReport", "0", buf, sizeof(buf), filename);
    LeaveCriticalSection(&critical_section);
    if (len && (atoi(buf) || !lstrcmpiA(buf, "TRUE") || !lstrcmpiA(buf, "yes")))
    {
        config_report_enabled = TRUE;
        config_report_open();
        config_report_write("otvdm: ---- config report enabled ([otvdm] ConfigReport=%s) ----\n", buf);
        return;
    }
    /* OTVDM_CONFIG_REPORT env var works regardless of whether otvdm.ini was
     * found, so a missing/unreadable ini can still be diagnosed */
    len = GetEnvironmentVariableA("OTVDM_CONFIG_REPORT", env, sizeof(env));
    if (len && (atoi(env) || !lstrcmpiA(env, "TRUE") || !lstrcmpiA(env, "yes")))
    {
        config_report_enabled = TRUE;
        config_report_open();
        config_report_write("otvdm: ---- config report enabled (env OTVDM_CONFIG_REPORT=%s) ----\n", env);
    }
}

/* report one setting lookup: 'value_or_default' is the value actually found
 * in the ini, or the default that was used because the key was missing.
 * Identical repeated lookups (AdjustDPI per font op, IAmNotNT per API call)
 * are counted, not re-printed, so the log stays bounded and readable. */
static struct { CHAR line[192]; LONG n; } config_report_seen[40];
static int config_report_seen_count = 0;

void config_report_lookup(LPCSTR section, LPCSTR key, const CHAR *value_or_default, BOOL found)
{
    CHAR text[192];
    LONG n;
    int i;

    if (!config_report_enabled)
        return;
    _snprintf(text, sizeof(text), "[%s] %s = %s (%s)",
              section ? section : "?", key ? key : "?",
              value_or_default ? value_or_default : "(null)",
              found ? "found in ini" : "NOT FOUND - using default");
    text[sizeof(text) - 1] = 0;

    for (i = 0; i < config_report_seen_count; i++)
    {
        if (!strcmp(config_report_seen[i].line, text))
        {
            config_report_seen[i].n++;
            return;
        }
    }
    if (config_report_seen_count < (int)(sizeof(config_report_seen)/sizeof(config_report_seen[0])))
    {
        lstrcpynA(config_report_seen[config_report_seen_count].line, text,
                  sizeof(config_report_seen[0].line));
        config_report_seen[config_report_seen_count].n = 1;
        config_report_seen_count++;
    }
    n = InterlockedIncrement(&config_report_count);
    config_report_write("otvdm: [%ld] %s\n", n, text);
}

/* print one diagnostic line to the shared report stream. This is the
 * entry point used by user.exe16 (resolved with GetProcAddress on
 * krnl386.exe16, see user/user.c config_report_note()) so that geometry
 * probes — e.g. the monitor rects computed by the FixScreenSize code paths —
 * land in the same single log without pulling config-report symbols into
 * user.exe16's import table at link time.
 *
 * Geometry probes are emitted from hot paths (every top-level window
 * creation, every SetWindowPos), so identical lines are collapsed here:
 * the first occurrence prints normally, repeats are counted, and the
 * total is flushed with the section summary ("... requested N times").
 * Lines differing only in the window class name stay distinct because
 * the whole formatted string is the dedupe key. */
#define CONFIG_REPORT_NOTE_MAX 64
static struct
{
    CHAR line[256];
    LONG n;
} config_report_notes[CONFIG_REPORT_NOTE_MAX];
static int config_report_notes_count;

void WINAPI config_report_note(LPCSTR text)
{
    int i;
    LONG n;

    if (!config_report_enabled || !text)
        return;
    for (i = 0; i < config_report_notes_count; i++)
    {
        if (!lstrcmpiA(config_report_notes[i].line, text))
        {
            n = InterlockedIncrement(&config_report_notes[i].n);
            /* print the repeat suffix occasionally so long-lived apps show
             * the line is still firing, without flooding the log */
            if (n == 2 || (n % 50) == 0)
                config_report_write("otvdm: note: %s [x%ld]\n", text, n);
            return;
        }
    }
    if (config_report_notes_count < CONFIG_REPORT_NOTE_MAX)
    {
        lstrcpynA(config_report_notes[config_report_notes_count].line, text,
                  sizeof(config_report_notes[0].line));
        config_report_notes[config_report_notes_count].n = 1;
        config_report_notes_count++;
    }
    config_report_write("otvdm: note: %s\n", text);
}

/* print the repeat counters collected by config_report_lookup; called once
 * from the launcher's final summary (config_report_finish) so every line in
 * the log appears exactly once */
static void config_report_flush_repeats(void)
{
    int i;
    for (i = 0; i < config_report_seen_count; i++)
    {
        if (config_report_seen[i].n > 1)
            config_report_write("otvdm:      ^ '%s' was requested %ld times in total (repeats collapsed)\n",
                                config_report_seen[i].line, config_report_seen[i].n);
    }
    for (i = 0; i < config_report_notes_count; i++)
    {
        if (config_report_notes[i].n > 1)
            config_report_write("otvdm:      ^ note '%s' emitted %ld times in total (repeats collapsed)\n",
                                config_report_notes[i].line, config_report_notes[i].n);
    }
}

void config_report_print_summary(void)
{
    if (!config_report_enabled)
        return;
    config_report_flush_repeats();
    config_report_write("otvdm: ---- krnl386 section done: %ld distinct lookups ----\n",
                        config_report_count);
}

static void config_report_summary(void)
{
    if (!config_report_enabled)
        return;
    if (config_report_summary_written)
        return;
    config_report_summary_written = TRUE;
    /* NOTE: intentionally NOT printing a final summary here. exec16() has
     * just started the win16 task; user/gdi/driver DLLs perform many more
     * lookups afterwards. The launcher writes the true end-of-report
     * summary from winevdm.c after the app is up (config_report_finish),
     * which calls config_report_print_summary() via GetProcAddress. */
}

void init_config(void)
{
    DWORD filename_len;
    BOOL ini_found = FALSE;
    CHAR inienv[MAX_PATH];
    DWORD len;

    /* NOTE: 'init' must be set before any recursive lookup below, and the
     * critical section must exist before we touch it; a previous version
     * re-entered init_config() via config_report_enable_check ->
     * krnl386_get_config_string, which would double-initialize the
     * critical section. */
    if (init)
        return;
    init = TRUE;
    InitializeCriticalSection(&critical_section);
    EnterCriticalSection(&critical_section);
    filename_len = GetModuleFileNameA(GetModuleHandleA(NULL), filename, MAX_PATH);
    if (!filename_len)
    {
        LeaveCriticalSection(&critical_section);
        return;
    }
    CHAR ininame[] = "otvdm.ini";
    if (_countof(ininame) + filename_len >= MAX_PATH)
    {
        LeaveCriticalSection(&critical_section);
        return;
    }
    LPSTR last = strrchr(filename, '\\');
    memcpy(last + 1, ininame, sizeof(ininame));

    /* allow an explicit override so a broken/missing default ini can still
     * be diagnosed by pointing at another copy */
    len = GetEnvironmentVariableA("OTVDM_INI", inienv, sizeof(inienv));
    if (len && len < sizeof(inienv))
        lstrcpynA(filename, inienv, sizeof(filename));

    ini_found = (GetFileAttributesA(filename) != INVALID_FILE_ATTRIBUTES);
    LeaveCriticalSection(&critical_section);

    /* emit the header now (before any other lookups) so even a completely
     * missing ini file gets reported; exactly once per process */
    config_report_enable_check();
    if (config_report_enabled && !config_report_header_written)
    {
        config_report_header_written = TRUE;
        config_report_open();
        if (filename[0])
            config_report_write("otvdm: ==== krnl386 section: ini path '%s'%s, ini file %s ====\n",
                                filename,
                                (len && len < sizeof(inienv)) ? " (overridden by OTVDM_INI env var)" : "",
                                ini_found ? "FOUND" : "NOT FOUND (all settings fall back to defaults)");
        else
            config_report_write("otvdm: ==== krnl386 section: could not resolve otvdm.ini path ====\n");
    }
}

DWORD WINAPI krnl386_get_config_string(LPCSTR appname, LPCSTR keyname, LPCSTR def, LPSTR ret, DWORD size)
{
    DWORD result;

    if (!init)
        init_config();
    EnterCriticalSection(&critical_section);
    result = GetPrivateProfileStringA(appname, keyname, def, ret, size, filename);
    LeaveCriticalSection(&critical_section);
    /* don't report our own bookkeeping lookups (avoids recursion into the
     * enable check when it reads [otvdm] ConfigReport itself) */
    if (config_report_enabled && !(appname && keyname && !lstrcmpiA(appname, "otvdm") && !lstrcmpiA(keyname, "ConfigReport")))
        config_report_lookup(appname, keyname, result ? ret : def, result != 0);
    return result;
}

DWORD WINAPI krnl386_get_config_int(LPCSTR appname, LPCSTR keyname, INT def)
{
    CHAR buf[32];
    DWORD result;
    BOOL found;

    if (!init)
        init_config();
    EnterCriticalSection(&critical_section);
    result = GetPrivateProfileIntA(appname, keyname, def, filename);
    /* detect whether the key really exists (GetPrivateProfileInt returns
     * the default silently when it doesn't) */
    found = GetPrivateProfileStringA(appname, keyname, "\x01__missing__", buf, sizeof(buf), filename)
            && strcmp(buf, "\x01__missing__") != 0;
    LeaveCriticalSection(&critical_section);
    if (config_report_enabled)
    {
        CHAR val[32];
        sprintf(val, "%lu", result);
        config_report_lookup(appname, keyname, val, found);
    }
    return result;
}

/* call from the launcher right before the win16 app is started */
void config_report_launch(LPCSTR appname)
{
    if (!init)
        init_config();
    config_report_enable_check();
    if (!config_report_enabled)
        return;
    config_report_open();
    fprintf(config_report_stream, "otvdm: about to launch win16 app: '%s'\n",
            appname ? appname : "(null)");
    fflush(config_report_stream);
    config_report_summary();
}
