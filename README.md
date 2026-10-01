# macbook-clamshell-fix

Small macOS tools that keep a MacBook's built-in screen working when the **lid-closed signal is stuck**. They use the real lid **angle** sensor instead.

## The issue: false "lid closed" on macOS 27

On some Apple Silicon MacBook Pros, since **macOS 27.0 / 27.0.1**, macOS believes the lid is **closed** while it's open. The built-in screen goes black, or the Mac sleeps, even though the lid angle sensor correctly reports that the lid is open.

### Symptoms

- The built-in screen goes **black every ~10 seconds** and comes back on a key press or trackpad movement.
- With external monitors connected, the built-in screen **stays off completely**, as if in clamshell mode.
- On battery with no external display, the Mac **goes to sleep** within ~5 s. Key presses only cause a short DarkWake with the screen still off.
- **Touch ID stops working.** The log shows `Clamshell is preventing the use of internal Touch ID`.
- Brightness glitches are possible: WindowServer floods the log with `swap brightness: nan`. On another affected Mac, this coincided with a 0.2 s "GUI zoom-out" flash and dimming.

### Affected machines (known so far)

| Mac | macOS | Lid flag | Notes |
|---|---|---|---|
| MacBook Pro 14" M2 Max (this repo) | 27.0 → 27.0.1 | **Stuck at "closed"** from the day after the 27.0 upgrade (2026-09-24) until it cleared by itself on 2026-10-01 | Lid angle sensor works |
| MacBook Pro 14" M2 Pro (2023), [reported on r/mac](https://www.reddit.com/r/mac/comments/1wuakq5/macbook_pro_14_m2_pro_intermittent_blackouts_02s/) | 27.0 → 27.0.1 | Short false "closed" blips (~100 ms), sometimes 4.5 s apart | Apple Diagnostics (Cmd+D) passed: lid angle sensor, sensors, display |

### Evidence

**1. The lid flag and the lid angle disagree.** Here's the reading with the lid open:

| Signal | Value |
|---|---|
| `AppleClamshellState` (IOPMrootDomain) | **Yes**: macOS thinks the lid is closed |
| Lid angle sensor (`las` HID device, read with `lid-angle`) | **114–115°**: the lid really is open |

```
$ ioreg -r -k AppleClamshellState -d 1 | grep AppleClamshellState
      "AppleClamshellState" = Yes
$ lid-angle
09:14:35  angle: 115°   clamshell: closed
```

**2. powerd keeps entering Clamshell Sleep.** On this Mac, `pmset -g log` recorded 2,000–5,000 of these events per day:

```
Entering DarkWake state due to 'Clamshell Sleep':TCPKeepAlive=active Using AC (Charge:100%)
Notification   Display is turned off
DarkWake to FullWake from Deep Idle [CDNVA] : due to HID Activity Using AC (Charge:100%)
Notification   Display is turned on
```

**3. The kernel and the display controller (DCP) receive "closed" with the lid open:**

```
kernel  PMRD: clamshell closed 1, disabled 0/0, desktopMode 0, ac 1
kernel  (IOMobileGraphicsFamily-DCP) get_clamshell_state fAPClamshellState 1 from_kernel 0 fReceivedClamshellStateFromSkyLight 0
```

The M2 Pro report shows the same thing as a short blip:

```
powerd: Sending clamshell state closed: 1 to WindowServer
corebrightnessd: Clamshell state update: 1
powerd: Turning off display for Clamshell Sleep
WindowServer: Display 1 set power state 0
powerd: Sending clamshell state closed: 0 to WindowServer
WindowServer: Display 1 set power state 1
```

**4. The ambient light pipeline breaks at the same time.** This shows up on **both** Macs:

```
corebrightnessd  Failed to copy kIOHIDEventTypeAmbientLightSensor event
WindowServer     Display 1 swap brightness: nan, limit: nan, indicator brightness: nan, ambient lux: nan, ...
```

On this Mac there were **~95,000 `brightness: nan` lines in 3 hours** (`log show --last 3h`).

### What it points to

- The **lid angle sensor works**. Only the lid-closed flag is wrong, and the ambient-light readings are bad at the same time.
- The angle sensor (`las`), the ambient light sensor (`als-temp`), the accelerometer and the gyro are all devices on the same **SPU sensor hub** (VID `0x05AC`, PID `0x8104`).
- **Two different Macs show the same log signature, both starting with macOS 27.0.** One of them passes Apple Diagnostics. This looks more like a macOS 27 regression in sensor or clamshell handling than a hardware fault. That isn't proven, and a failing magnetic lid sensor near the hinge can't be ruled out.
- On this Mac the flag **cleared by itself** on 2026-10-01 at 11:04:47 (`PMRD: clamshell closed 0`), with no reboot, sleep or other visible trigger. A hardware fault doesn't usually fix itself like that.

### Check whether you're affected

```sh
ioreg -r -k AppleClamshellState -d 1 | grep AppleClamshellState                 # "Yes" with the lid open = affected
pmset -g log | grep -c "Clamshell Sleep"                                         # thousands = affected
/usr/bin/log show --last 1h --predicate 'eventMessage CONTAINS "PMRD: clamshell closed"' --style compact
/usr/bin/log show --last 1h --predicate 'process == "WindowServer" AND eventMessage CONTAINS "brightness: nan"' --style compact | wc -l
./lid-angle                                                                      # real angle vs the flag
```

Use `/usr/bin/log`. In zsh, a plain `log` can run something other than the macOS log tool.

### What to do

1. **Before paying for a repair, try a full shutdown** (not a restart) with everything unplugged, then run **Apple Diagnostics** (hold power at startup, then press Cmd+D).
2. **Report it to Apple** with Feedback Assistant (Mac → Power/Battery) and attach a sysdiagnose (Ctrl+Opt+Cmd+Shift+Period). Mention `PMRD: clamshell closed 1` with the lid open and the `swap brightness: nan` flood, and say it started with 27.0.
3. **Use the tools below as a workaround** until Apple fixes it. They do nothing while the flag is correct.

These tools are a **workaround**, not a fix.

## Tools

| Tool | What it does |
|---|---|
| `display-on` | Turns the built-in display on or off through the private `CGSConfigureDisplayEnabled` API |
| `lid-angle` | Reads the lid angle sensor and shows it next to `AppleClamshellState` |
| `lid-watch` | Background daemon (per-user LaunchAgent): decides from the **angle** whether the built-in display should be on |
| `lid-sleepguard` | Background daemon (root LaunchDaemon): decides from the **angle** whether macOS may sleep, which stops the false Clamshell Sleep |

### display-on

```sh
display-on            # turn the built-in display on
display-on off        # turn the built-in display off
display-on on 1       # turn on a specific display id
display-on list       # list displays + lid sensor state
```

Example `list` output while the lid flag was stuck:

```
lid sensor: closed (Yes)
id         builtin  online  active  asleep size
6          no       yes     yes     no     2560x1440
3          no       yes     yes     no     2560x1440
1          yes      no      no      no     1512x982     <- built-in panel, off
```

### lid-angle

```sh
lid-angle             # read once
lid-angle watch       # print every 0.5 s, Ctrl+C to stop
```

```
09:14:35  angle: 115°   clamshell: closed
```

### lid-watch

Checks the angle once per second:

| Condition | Action |
|---|---|
| angle ≥ 20° and built-in display off | Turn it on, and keep the previous main display as main |
| angle ≤ 5° and built-in on and another display active | Turn it off, like real clamshell mode |
| angle ≤ 5° and built-in is the only display | Nothing, so you never end up with no screen |
| 5° < angle < 20°, or the sensor can't be read | Nothing |

It waits at least 5 seconds between display changes. If the sensor handle stops working after sleep, it reopens it.

```sh
lid-watch --dry-run   # log decisions only, change nothing
```

The long-running process only reads the angle. Each check runs in a short-lived child (`lid-watch --once <angle>`), because CoreGraphics in a long-running process stops seeing display changes after it has changed a display itself. This costs about 1% of one CPU core while the lid is open, and nothing between 5° and 20°.

With no external display and the panel switched off, every built-in display id (the real one and the phantom ones) reports 0×0. lid-watch therefore remembers the last real built-in id (default `1`) and enables that one.

### lid-sleepguard

Without this daemon, unplugging the monitors and the charger made macOS enter **Clamshell Sleep** within ~5 s. After that, key presses only caused a short DarkWake with the screen still off, so lid-watch never got a chance to run.

| Condition | Action |
|---|---|
| angle ≥ 20° | `pmset -a disablesleep 1`: no sleep while the lid is really open |
| angle ≤ 5° | `pmset -a disablesleep 0`: closing the lid sleeps normally |
| sensor missing, 5 failed reads in a row, or the daemon stops (SIGTERM/SIGINT/SIGHUP) | `pmset -a disablesleep 0` (fail safe) |

While the lid is open, **idle system sleep is also disabled**. The display still turns off after the display-sleep timer.

```sh
lid-sleepguard --dry-run   # log decisions only, no pmset calls (no root needed)
```

## Build

You need the Xcode command line tools (`xcode-select --install`).

```sh
clang -O2 -Wall -o display-on display-on.c -framework CoreGraphics -framework IOKit -framework CoreFoundation
clang -O2 -Wall -o lid-angle  lid-angle.c  -framework IOKit -framework CoreFoundation
clang -O2 -Wall -o lid-watch  lid-watch.c  -framework CoreGraphics -framework IOKit -framework CoreFoundation
clang -O2 -Wall -o lid-sleepguard lid-sleepguard.c -framework IOKit -framework CoreFoundation
```

## Install

Command line tools:

```sh
sudo install -m 755 display-on lid-angle /usr/local/bin/
```

Daemon (a per-user LaunchAgent, no sudo needed):

```sh
mkdir -p ~/.local/bin && install -m 755 lid-watch ~/.local/bin/lid-watch
# create ~/Library/LaunchAgents/com.noname.lid-watch.plist (see below), then:
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.noname.lid-watch.plist
```

`~/Library/LaunchAgents/com.noname.lid-watch.plist` (replace `YOUR_USER` with your macOS username):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>                  <string>com.noname.lid-watch</string>
    <key>ProgramArguments</key>       <array><string>/Users/YOUR_USER/.local/bin/lid-watch</string></array>
    <key>RunAtLoad</key>              <true/>
    <key>KeepAlive</key>              <true/>
    <key>ThrottleInterval</key>       <integer>10</integer>
    <key>ProcessType</key>            <string>Interactive</string>
    <key>LimitLoadToSessionType</key> <string>Aqua</string>
    <key>StandardOutPath</key>        <string>/Users/YOUR_USER/Library/Logs/lid-watch.log</string>
    <key>StandardErrorPath</key>      <string>/Users/YOUR_USER/Library/Logs/lid-watch.log</string>
</dict>
</plist>
```

Manage it:

```sh
tail -f ~/Library/Logs/lid-watch.log                      # see what it does
launchctl kickstart -k gui/$(id -u)/com.noname.lid-watch  # restart (e.g. after reinstalling the binary)
launchctl bootout gui/$(id -u)/com.noname.lid-watch       # stop
rm ~/Library/LaunchAgents/com.noname.lid-watch.plist ~/.local/bin/lid-watch   # uninstall
```

Sleep guard (a root LaunchDaemon; the plist is in this folder):

```sh
sudo install -m 755 lid-sleepguard /usr/local/bin/lid-sleepguard
sudo install -m 644 -o root -g wheel com.noname.lid-sleepguard.plist /Library/LaunchDaemons/
sudo launchctl bootstrap system /Library/LaunchDaemons/com.noname.lid-sleepguard.plist
pmset -g | grep -i sleepdisabled          # should show 1 with the lid open
tail -f /var/log/lid-sleepguard.log
```

Remove it:

```sh
sudo launchctl bootout system/com.noname.lid-sleepguard && sudo pmset -a disablesleep 0
sudo rm /Library/LaunchDaemons/com.noname.lid-sleepguard.plist /usr/local/bin/lid-sleepguard
```

## Keyboard shortcut (optional)

To turn the screen on with a key combination: open **Shortcuts** → new shortcut → **Run Shell Script** containing `/usr/local/bin/display-on` → **ⓘ Details** → **Add Keyboard Shortcut**. Allow scripts in Shortcuts → Settings → Advanced.

## Limitations

- **Clamshell Sleep is handled by `lid-sleepguard`.** Without it, on battery with no external display, macOS sleeps because of the false lid-closed flag and nothing can turn the screen back on. Tested 2026-10-01: with both daemons running, unplugging the monitors and charger kept the Mac awake, and lid-watch turned the panel back on.
- **Idle sleep is off while the lid is open** (side effect of `disablesleep`). Close the lid or use  → Sleep.
- **It relies on private APIs.** `CGSConfigureDisplayEnabled` / `SLSConfigureDisplayEnabled` are found at runtime with `dlsym`. If a macOS update removes them, the tools print an error and change nothing.
- **The display-change effect lasts only for the session.** After a reboot or logout, `lid-watch` re-applies it automatically.
- **It doesn't fix the cause.** See [What to do](#what-to-do). If you contact Apple Support, show them `lid-angle`: angle 115° while `AppleClamshellState = Yes`.
- **Tested only on a MacBook Pro 14" M2 Max with macOS 27.0.1.** Other models may use different display ids or a different lid angle sensor.

## Hardware reference

Lid angle sensor on this Mac, found with `ioreg -r -c IOHIDDevice -l`:

| Property | Value |
|---|---|
| Product | `las` |
| Transport | `SPU` |
| VendorID / ProductID | `0x05AC` / `0x8104` |
| Usage page / usage | `0x0020` (Sensor) / `0x008A` (Orientation) |
| Read | Feature report id 1; angle in degrees = `report[1] \| report[2] << 8` |

The same PID `0x8104` also hosts the `accel`, `gyro` and `als-temp` devices, which have different usage pages.

Credit: the sensor protocol comes from [lid-angle-rs](https://github.com/wangfu91/lid-angle-rs).
