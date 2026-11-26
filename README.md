# tpscrollfix

ThinkPad-style middle button for the Elan TrackPoint on the Lenovo ThinkPad X13s (Windows on ARM64).

- **Tap** the middle button → middle click (open links in new tabs, close tabs, paste on X-style apps)
- **Hold and push** the stick → scroll vertically and horizontally; the cursor stays put
- Middle buttons on other mice (USB, Bluetooth) pass through untouched

## The problem

On the X13s, Elan's TrackPoint driver (`ETD.sys`) ships configured for scroll-only middle button behavior, and that mode is buggy:

| Action | What Elan's driver sends |
|---|---|
| Tap middle button | A lone middle-button **release**, with no press |
| Hold + move stick | Wheel events, then a lone middle-button **release** |

Browsers need both a press and a release to recognize a middle click, so tapping does nothing. Sites that react to the release alone (bsky.app's post cards, for example) "click" whatever is under the cursor at the end of every scroll.

## How the fix works

### 1. Switch Elan's driver to a plain middle button

`ETD.sys` reads `n3A2PST_Middle_Mode` from its registry settings at startup (Elan's service copies `HKCU\Software\Elantech` into `HKLM\SYSTEM\CurrentControlSet\Control\Elantech`):

| Value | Behavior |
|---|---|
| `0` (default) | Scroll-only, with the broken lone-release behavior |
| `1` or `2` | Plain middle button: real press and release, stick moves the cursor |

Set it to `1`:

```powershell
reg export HKCU\Software\Elantech "$HOME\elantech-backup.reg" /y
Set-ItemProperty HKCU:\Software\Elantech\SmartPad -Name n3A2PST_Middle_Mode -Value 1
Restart-Service ETDService    # admin; sign out and back in if it doesn't take
```

Disassembly of `ETD.sys` (ARM64, driver 37.21.3.1) found no combined scroll-and-click mode, so the combined behavior comes from step 2.

### 2. `tpscrollfix.exe` adds scrolling back

A small background program with no window. It uses:

- **A low-level mouse hook (`WH_MOUSE_LL`)** to intercept middle-button and movement events system-wide.
- **Raw Input** to learn which physical device sent each middle press. A device whose path contains `ELAN06F0` (the X13s TrackPoint's HID ID) gets TrackPoint behavior. Anything else is replayed immediately and passes through untouched. If Raw Input doesn't report within 60 ms, the press is treated as the TrackPoint.

TrackPoint state machine:

```
press ──► DECIDING ──(TrackPoint)──► PENDING ──(moved ≥ threshold)──► SCROLLING
              │                         │                                │
        (other mouse)               release                          release
              ▼                         ▼                                ▼
          PASSTHRU                 middle click                     nothing sent
```

While the button is held, movement is swallowed (so the cursor stays put) and converted into high-resolution wheel events (`MOUSEEVENTF_WHEEL` / `MOUSEEVENTF_HWHEEL`). Windows pointer acceleration still applies, so pushing harder scrolls faster. Events the program injects itself are tagged through `dwExtraInfo` so the hook ignores them.

## Usage

```
tpscrollfix.exe [speed] [threshold]
```

| Argument | Default | Meaning |
|---|---|---|
| `speed` | `3` | Wheel units per pixel of stick movement (120 = one wheel notch). Higher scrolls faster. |
| `threshold` | `4` | Pixels of stick movement before a press becomes a scroll. Raise it if taps come out as tiny scrolls. |

Only one copy runs at a time. To stop it:

```
taskkill /im tpscrollfix.exe
```

### Start at login

From an admin prompt:

```
schtasks /create /tn tpscrollfix /tr "C:\Users\ryan\src\elan\tpscrollfix\tpscrollfix.exe" /sc onlogon /rl highest /f
```

`/rl highest` runs it elevated so it also works while an administrator window has focus. A non-elevated copy can't intercept input aimed at elevated windows.

To remove: `schtasks /delete /tn tpscrollfix /f`

## Building

Cross-compiled from Linux with Zig (`pip install ziglang`):

```
python3 -m ziglang cc -target aarch64-windows-gnu -O2 -Wall -municode \
  -Wl,--subsystem,windows -s -o tpscrollfix.exe tpscrollfix.c -luser32 -lshell32
```

It's one C file with no dependencies beyond `user32` and `shell32`, so it also builds with MSVC for ARM64:

```
cl /O2 tpscrollfix.c user32.lib shell32.lib /link /SUBSYSTEM:WINDOWS
```

## Caveats

- **Driver updates.** A Lenovo or Elan driver update may reset `n3A2PST_Middle_Mode` to `0`. If tapping stops working and scrolling clicks things again, check that value first.
- **Other machines.** `TRACKPOINT_ID` in `tpscrollfix.c` is the X13s's Elan HID ID. On other hardware, change it to match the pointing stick's device path.
- **Untested with an external mouse.** Detection of other mice is in place but hasn't been tried yet.

## Undo everything

```powershell
taskkill /im tpscrollfix.exe
schtasks /delete /tn tpscrollfix /f
reg import "$HOME\elantech-backup.reg"
Restart-Service ETDService
```
