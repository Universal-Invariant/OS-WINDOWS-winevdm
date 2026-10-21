#ifndef __WINE_KRNL386_CONF_H
#define __WINE_KRNL386_CONF_H

#include <windows.h>

DWORD WINAPI krnl386_get_config_int(LPCSTR appname, LPCSTR keyname, INT def);
DWORD WINAPI krnl386_get_config_string(LPCSTR appname, LPCSTR keyname, LPCSTR def, LPSTR ret, DWORD size);

/* report the otvdm.ini location and every setting lookup performed before
 * and during the launch of a win16 app; enable with [otvdm] ConfigReport=1
 * in otvdm.ini */
void init_config(void);
void config_report_enable_check(void);
void config_report_lookup(LPCSTR section, LPCSTR key, const CHAR *value_or_default, BOOL found);

/* free-form diagnostic line appended to the shared config report; used by
 * user.exe16 to log the monitor/work-area rects that the FixScreenSize code
 * paths compute (resolved with GetProcAddress at runtime) */
void WINAPI config_report_note(LPCSTR text);

#endif /* __WINE_KRNL386_CONF_H */
