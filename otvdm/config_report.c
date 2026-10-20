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
static BOOL reported_header = FALSE;

/* forward declarations: MSVC/C23-style compilers treat an implicit function
 * declaration as 'int f()', which then collides with the real definitions
 * below (C2371: redefinition; different basic types). Declaring them up
 * front keeps every use type-checked. */
static void cfg_init( void );
static int cfg_get_int(LPCSTR section, LPCSTR key, INT def, BOOL *found);
void config_report_launch( LPCSTR appname );

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
    }
    va_start( ap, fmt );
    vfprintf( cfg_stream, fmt, ap );
    va_end( ap );
    fflush( cfg_stream );
}

static void cfg_lookup( LPCSTR section, LPCSTR key, const CHAR *value_or_default, BOOL found )
{
    if (!cfg_enabled) return;
    cfg_write( "otvdm: [%ld] [%s] %s = %s (%s)\n",
               InterlockedIncrement( &cfg_count ),
               section ? section : "?", key ? key : "?",
               value_or_default ? value_or_default : "(null)",
               found ? "found in ini" : "NOT FOUND - using default" );
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

    if (cfg_init_done || reported_header) return;
    cfg_init_done = TRUE;
    reported_header = TRUE;

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
    cfg_write( "otvdm: ---- launcher section done (%ld lookups so far) ----\n", cfg_count );
    /* NOTE: krnl386.exe16 is a separate PE loaded in-process and keeps its
     * own report state; on Windows it writes its per-setting lookups to its
     * own copy of otvdm_config_report.log in the same directory. */
    cfg_write( "otvdm: note: krnl386 logs its own lookups to a separate 'otvdm_config_report.log' copy in this same directory\n" );
}

void config_report_launch( LPCSTR appname )
{
    cfg_init();                         /* one-time: parse ini/env, write header */
    if (!cfg_enabled || !appname) return;
    cfg_write( "otvdm: about to launch win16 app: '%s'\n", appname );
    cfg_write( "otvdm: ---- config report: %ld launcher setting lookups logged ----\n", cfg_count );
}
