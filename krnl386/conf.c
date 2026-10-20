#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BOOL init;

static CHAR filename[MAX_PATH];
static CRITICAL_SECTION critical_section;

/* config report: when enabled (otvdm.ini [otvdm] ConfigReport=1) we print
 * the ini path and every setting lookup so users can verify otvdm.ini is
 * found and which values are read before launching a win16 app. */
static BOOL config_report_enabled = FALSE;
static FILE *config_report_stream = NULL;
static LONG config_report_count = 0;

DWORD WINAPI krnl386_get_config_string(LPCSTR appname, LPCSTR keyname, LPCSTR def, LPSTR ret, DWORD size);
DWORD WINAPI krnl386_get_config_int(LPCSTR appname, LPCSTR keyname, INT def);

static void config_report_open(void)
{
    if (config_report_stream)
        return;
    config_report_stream = fopen("otvdm_config_report.log", "a");
    if (!config_report_stream)
        config_report_stream = stderr;
}

void config_report_enable_check(void)
{
    CHAR buf[16];
    DWORD len;

    if (!init)
        init_config();
    if (config_report_enabled)
        return;
    EnterCriticalSection(&critical_section);
    len = GetPrivateProfileStringA("otvdm", "ConfigReport", "0", buf, sizeof(buf), filename);
    LeaveCriticalSection(&critical_section);
    if (len && (atoi(buf) || !lstrcmpiA(buf, "TRUE") || !lstrcmpiA(buf, "yes")))
    {
        config_report_enabled = TRUE;
        config_report_open();
        fprintf(config_report_stream, "otvdm: ---- config report enabled ([otvdm] ConfigReport=%s) ----\n", buf);
        fflush(config_report_stream);
    }
}

/* report one setting lookup: 'value_or_default' is the value actually found
 * in the ini, or the default that was used because the key was missing */
void config_report_lookup(LPCSTR section, LPCSTR key, const CHAR *value_or_default, BOOL found)
{
    LONG n;

    if (!config_report_enabled)
        return;
    n = InterlockedIncrement(&config_report_count);
    fprintf(config_report_stream, "otvdm: [%ld] [%s] %s = %s (%s)\n",
            n, section ? section : "?", key ? key : "?",
            value_or_default ? value_or_default : "(null)",
            found ? "found in ini" : "NOT FOUND - using default");
    fflush(config_report_stream);
}

static void config_report_summary(void)
{
    if (!config_report_enabled)
        return;
    fprintf(config_report_stream, "otvdm: ---- config report: %ld setting lookups logged ----\n",
            config_report_count);
    fflush(config_report_stream);
}

void init_config(void)
{
    DWORD filename_len;
    BOOL ini_found = FALSE;
    CHAR inienv[MAX_PATH];
    DWORD len;

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
     * missing ini file gets reported */
    config_report_enable_check();
    if (config_report_enabled)
    {
        config_report_open();
        fprintf(config_report_stream, "otvdm: ==== otvdm.ini config report ====\n");
        fprintf(config_report_stream, "otvdm: ini path: '%s'%s\n", filename,
                (len && len < sizeof(inienv)) ? " (overridden by OTVDM_INI env var)" : "");
        fprintf(config_report_stream, "otvdm: ini file %s\n",
                ini_found ? "FOUND" : "NOT FOUND (all settings fall back to defaults)");
        fflush(config_report_stream);
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
    if (config_report_enabled)
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
