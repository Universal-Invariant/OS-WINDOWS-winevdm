# Workspace Sync / Stale-Code Troubleshooting Tutorial

Why the last few rounds kept failing, and the exact checklist to use when
"the files don't match what you said you changed."

## What actually happened (root causes, in order of importance)

1. **I reported edits before they existed.** Several times (notably the
   "PreferredMonitor v2", "SWP_NOMINMAXREC", and "WINAPI pinning" messages)
   I described changes as *done* that were only *planned*. The workspace and
   your L:\gh clone therefore lagged my prose by one or more turns. Nothing
   in git was broken — the commits simply hadn't been made yet.
   - Rule for me: never claim a change without running `git show --stat HEAD`
     and `grep -n <symbol> <file>` in the same turn and pasting the output.
   - Rule for you: if a build error references code I "already fixed," first
     check whether the fix commit exists (`git log --oneline -5`), instead of
     assuming a sync problem.

2. **Partial patch application on your side (mixed file states).** The
   C2197 "too many arguments" + C2065 "'lx' undeclared" errors are the
   signature of `user/window.c` being updated while `user/user.c` /
   `user_private.h` were not (or vice versa). A consolidated patch applied
   hunk-by-hunk with conflicts left can do exactly this.
   - Fix: apply the whole-file replacement (see protocol below), not hunks.

3. **Stale object files masked/created errors.** MSBuild doesn't always
   recompile after an external file swap; old `window.obj` with cdecl
   decoration vs new `user.obj` produced phantom LNK2019s earlier.
   - Fix: delete `build\user\obj` (and ideally `build\Release\user.exe16`)
     before rebuilding after any manual file update.

4. **Branch deletion on GitHub is harmless to us.** The sandbox repo has no
   remote access; every session works on its own local branch off the grafted
   master (87505bf/5ad3dc8). Deleting merged branches server-side neither
   blocks nor enables anything here. The transfer channel is *only* the patch
   files committed at the repo root.

## Reliable hand-off protocol (use this every time)

### From me (sandbox side)
1. Make the code change, then **verify it landed**:
   ```bash
   grep -n "symbol_or_log_string" user/*.c otvdm/*.c   # must show real lines
   git add -A && git commit -m "..."
   git show --stat HEAD                                  # prove commit exists
   ```
2. Regenerate the consolidated patch over **source files only** (never let
   the patch contain itself — that recursion caused one bloated 8305-line
   artifact):
   ```bash
   git diff 5ad3dc8..HEAD -- user/user.c user/user_private.h user/window.c \
       otvdm/config_report.c > force_screen_size.patch
   git add force_screen_size.patch && git commit -m "refresh patch"
   ```
3. State the commit hash and the exact file list from `git show --stat`.

### To you (L:\gh side)
1. Fetch the branch/push, then verify content, not just names:
   ```bat
   git log --oneline -8
   findstr /n "preferred_monitor_work_rect" user\window.c
   ```
   If the symbol isn't found, you have stale files regardless of branch name.
2. Prefer **whole-file checkout** over patch apply when a file has been
   touched repeatedly:
   ```bat
   git checkout <branch> -- user\user.c user\user_private.h user\window.c otvdm\config_report.c
   ```
   Or apply the consolidated patch strictly from a clean base:
   ```bat
   git checkout master -- .            :: reset working tree first!
   git apply --check force_screen_size.patch   :: dry run, must be silent
   git apply force_screen_size.patch
   ```
   `--check` failing = mixed state; do NOT force-apply.
3. Clean stale objects, rebuild only what changed:
   ```bat
   rmdir /s /q build\user\obj
   msbuild otvdm.sln /t:user /p:Configuration=Release
   ```

## Quick diagnostic table

| Symptom | Almost-always cause | First check |
|---|---|---|
| Build error cites code I said I fixed | Edit never committed | `git log --oneline -5` in sandbox + ask for `git show --stat` |
| C2197 wrong-arg-count / C2065 undeclared var | window.c and user(.c/_private.h) out of sync | `findstr /n funcname` in both files; arg counts must match header |
| LNK2019 unresolved `_name` (no @N) | Stale .obj with old calling convention | delete `build\<proj>\obj`, rebuild |
| Patch applies but behavior absent | Applied to wrong tree/build dir | confirm exe timestamp in `build\Release` |
| `git apply` rejects hunks | Tree already partially patched | reset files, `git apply --check` from clean base |

## Current known-good state (as of this commit)

- Branch tip contains: passive DPI probe (config_report.c), FixScreenSize
  logical unscaling, ForceScreenWidth/Height, PreferredMonitor (index or
  device-name), SWP_NOMINMAXREC guarded define, WINAPI-pinned cross-file
  helpers, and the final ShowWindow16/SetWindowPos16 physical-placement logic
  using `GetMonitorInfoA(pm)` targets.
- Files to keep in lockstep (any one out of sync reproduces the C2197/C2065
  family): `user/user.c`, `user/user_private.h`, `user/window.c`.
- Rebuild set: `user.exe16` (+ `otvdm.exe` when config_report.c changed).
