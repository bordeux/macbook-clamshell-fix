# CLAUDE.md

Three small C tools for macOS (Apple Silicon) that work around a stuck `AppleClamshellState` flag by using the real lid angle sensor. Read README.md for background and usage.

## Layout

- `display-on.c`: CLI to enable or disable the built-in display (`on`, `off`, `list`, `on <id>`).
- `lid-angle.c`: CLI to read the lid angle sensor (`watch` for continuous output).
- `lid-sleepguard.c`: root daemon that polls the angle and toggles `pmset -a disablesleep`, so the stuck flag can't put the Mac into Clamshell Sleep. Its plist is `com.noname.lid-sleepguard.plist` in this folder.
- `lid-watch.c`: daemon that polls the angle and enables or disables the built-in display. Self-contained: it repeats the helpers from the other two files on purpose, so each tool builds from a single file. Keep it that way unless you add a shared header for all three.
- Compiled binaries (`display-on`, `lid-angle`, `lid-watch`) sit next to their sources. This is not a git repo.

## Build

```sh
clang -O2 -Wall -o display-on display-on.c -framework CoreGraphics -framework IOKit -framework CoreFoundation
clang -O2 -Wall -o lid-angle  lid-angle.c  -framework IOKit -framework CoreFoundation
clang -O2 -Wall -o lid-watch  lid-watch.c  -framework CoreGraphics -framework IOKit -framework CoreFoundation
clang -O2 -Wall -o lid-sleepguard lid-sleepguard.c -framework IOKit -framework CoreFoundation
```

`-framework CoreFoundation` must be passed explicitly. Without it, linking fails on `CFRelease`, `CFBooleanGetValue` and similar.

## Installed locations on this Mac

| Binary | Installed at | Notes |
|---|---|---|
| display-on | `/usr/local/bin/display-on` | root-owned, so updating needs `sudo install` |
| lid-angle | `/usr/local/bin/lid-angle` | root-owned, so updating needs `sudo install` |
| lid-watch | `~/.local/bin/lid-watch` | run by LaunchAgent `com.noname.lid-watch` |

| lid-sleepguard | `/usr/local/bin/lid-sleepguard` | root; run by LaunchDaemon `com.noname.lid-sleepguard` (`/Library/LaunchDaemons/`), log `/var/log/lid-sleepguard.log` |

Anything under `/usr/local/bin` or `/Library/LaunchDaemons` needs `sudo`. The `!` prompt prefix can't read a sudo password, so give the user the command to run in their own terminal.
Update the sleep guard with: `sudo install -m 755 lid-sleepguard /usr/local/bin/ && sudo launchctl kickstart -k system/com.noname.lid-sleepguard`.

LaunchAgent plist: `~/Library/LaunchAgents/com.noname.lid-watch.plist`. Log: `~/Library/Logs/lid-watch.log`.
After rebuilding lid-watch: `install -m 755 lid-watch ~/.local/bin/lid-watch && launchctl kickstart -k gui/$(id -u)/com.noname.lid-watch`.

## Key facts and gotchas

- **Private display API.** `CGSConfigureDisplayEnabled(config, id, bool)` is resolved with `dlsym`: CoreGraphics first, then SkyLight (`SLSConfigureDisplayEnabled`). On macOS 27.0.1 both exist. It must run between `CGBeginDisplayConfiguration` and `CGCompleteDisplayConfiguration(..., kCGConfigureForSession)`. `nm` on the framework stubs shows nothing because the symbols live in the dyld shared cache, so check with `dlsym`.
- **It must run in the user's GUI (Aqua) session.** That's why lid-watch is a LaunchAgent and not a LaunchDaemon. Running it as root from a system daemon won't reach WindowServer.
- **Finding the built-in display.** While the panel is off, it's missing from `CGGetOnlineDisplayList`. The code also checks `CGSGetDisplayList` and probes ids 1–8. On this Mac the panel is **id 1** (1512x982 points = 3024x1964 Retina). `CGDisplayIsBuiltin` also returns true for some phantom ids (7, 8) that report 0x0, so always require `CGDisplayPixelsWide(id) > 0`.
- **CoreGraphics display state goes stale in a long-running process.** On macOS 27.0.1, once a process has applied a display configuration, its `CGDisplayIsOnline`/`CGDisplayIsActive` stop tracking reality. Running the CFRunLoop and registering `CGDisplayRegisterReconfigurationCallback` did **not** help. So the lid-watch parent never calls CoreGraphics. It reads the angle and, when the angle is ≥ OPEN_DEG or ≤ CLOSE_DEG, `posix_spawn`s itself as `lid-watch --once <angle> [--dry-run]`. The child makes one decision with fresh state and exits with 10 if it changed something, which triggers the parent's cooldown. Cost is about 43M instructions per check. Don't move CG calls back into the parent loop.
- **With no external display and the panel off**, every built-in candidate (including the real id 1) reports 0x0 and asleep, and WindowServer creates a placeholder display (seen as id 13/14, 1920x1080, not builtin). lid-watch therefore falls back to `last_builtin` (default 1) when no sized built-in exists. Don't treat "asleep" as off: that's normal idle display sleep.
- **Enabling the panel makes it the main display.** `restore_main()` in lid-watch shifts every display's origin so the previous main display is back at (0,0). It skips this when the previous main is built-in, offline or inactive, which covers the placeholder that disappears once the panel comes on.
- **Clamshell Sleep.** On battery with no external display, the stuck flag makes powerd sleep the Mac within ~5 s. A key press then gives only a DarkWake, and the Mac goes back to sleep. Only `lid-sleepguard` (`disablesleep 1` while angle ≥ 20°) prevents this. lid-watch alone can't.
- **Lid angle sensor.** HID device `las`: VID `0x05AC`, PID `0x8104`, usage page `0x20`, usage `0x8A`, transport SPU. Read feature report id 1 with an 8-byte buffer; angle = `report[1] | report[2] << 8`. PID `0x8104` also hosts `accel`, `gyro` and `als-temp`, so always match on usage page/usage as well. No TCC/Input Monitoring prompt was needed.
- **Clamshell flag.** Read `AppleClamshellState` from `IOPMrootDomain`. On this Mac it's stuck at `Yes` with the lid open (angle 115°). That's the bug being worked around, so don't "fix" code that ignores it.

## Safety rules when changing behavior

- Never disable the built-in display when it is the only active display. `other_active_displays()` guards this, so keep that guard.
- Keep the gap between the two thresholds (`OPEN_DEG` 20 / `CLOSE_DEG` 5) and the `COOLDOWN_SEC` delay, so the code doesn't fight WindowServer or flip the display on and off.
- Treat a failed angle read as "do nothing" and reopen the sensor; never use it as a reason to change the display.
- Test with `lid-watch --dry-run` first. To test the enable path safely, keep external monitors connected, run `./display-on off`, and check that lid-watch turns the panel back on.
- `lid-sleepguard` must always fail safe to `disablesleep 0`: when the sensor is missing, reads keep failing, or it gets SIGTERM/SIGINT/SIGHUP. If it got stuck at `disablesleep 1`, a Mac with its lid closed in a bag would stay awake. Keep these paths. Don't make any other root power changes without asking the user.

## Useful diagnostics

```sh
./display-on list
./lid-angle watch
ioreg -r -k AppleClamshellState -d 1 | grep AppleClamshell
pmset -g log | grep -E "Clamshell|Display is turned" | tail
ioreg -r -c IOHIDDevice -l | grep -E '"ProductID" = 33028' -B12 -A20 | grep -E '"(Product|PrimaryUsagePage|PrimaryUsage)"'
```
