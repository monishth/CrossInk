# Simulator Verification with grim and hyprctl

How to drive the CrossInk simulator on Hyprland/Wayland and capture what it
renders, so BookOrbit work can be verified visually without an X4 Pro on the
desk.

Everything below was probed on this machine. Where something does **not** work,
that is stated rather than papered over.

---

## What was verified

| Tool | Status | Notes |
|---|---|---|
| `grim` | **Works** | Captured a 5120×1440 PNG — but only with DPMS on. See the gotcha below. |
| `hyprctl` (IPC, `movecursor`, `dpms`) | **Works** | Needs `HYPRLAND_INSTANCE_SIGNATURE`. |
| `wtype` (keyboard) | **Installed** | Drives the entire simulator UI — every screen is keyboard-navigable. |
| `hyprctl dispatch click` | **Does not exist** | `Invalid dispatcher`. Hyprland cannot synthesize mouse clicks. |
| `wlrctl` / `ydotool` / `dotool` | **Not installed** | `/dev/uinput` is root-only and the user is not in an `input` group, so `ydotool` would need a udev rule. |
| `pio` (PlatformIO) | **Not installed** | Prerequisite for building the simulator at all. |

**The headline:** clicks cannot be synthesized today, but they are not needed.
The simulator is fully keyboard-driven, so `wtype` + `grim` covers the whole UI.
Click synthesis is only required for genuinely touch-specific code paths on
`x4-pro-simulator`, and the section at the end covers how to enable it if that
becomes necessary.

---

## Prerequisites

### 1. Install PlatformIO

Not currently present — `pio: command not found`, and `import platformio` fails.

```bash
python3 -m pip install --user pipx && pipx install platformio
# or the official bootstrap:
#   curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
#   python3 /tmp/get-platformio.py
```

Verify with `pio --version`.

### 2. Export the Wayland environment

A shell started from a TTY (or from an agent session) inherits none of this.
`hyprctl` hangs indefinitely without the instance signature.

```bash
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-1
export HYPRLAND_INSTANCE_SIGNATURE=$(ls /run/user/1000/hypr | head -1)
```

Confirm: `hyprctl version` returns promptly.

### 3. Wake the display before any screenshot

**This is the one that will waste your afternoon.** `grim` hangs forever — not
an error, just a hang — when the monitor is DPMS-off, because the compositor
produces no frames and the `wlr-screencopy` request never completes.

```bash
hyprctl -j monitors | python3 -c "import json,sys; print([m['dpmsStatus'] for m in json.load(sys.stdin)])"
# false means asleep — wake it first:
hyprctl dispatch dpms on
```

Always wrap `grim` in `timeout` so a regression here fails fast instead of
hanging a test run:

```bash
timeout 20 grim -o HDMI-A-2 shot.png || echo "grim failed or timed out"
```

---

## Two verification modes

### Mode A — headless scripted (regression, CI)

The existing harness. No window, no screenshots, deterministic exit code. This
is the tripwire to run after every task.

```bash
./scripts/run_simulator_smoke_test.py --env x4-pro-simulator --page-turns 60
```

It builds the env, copies an EPUB fixture into an isolated temp `fs_`, sets
`SDL_VIDEODRIVER=dummy`, drives the app through env vars
(`CROSSINK_SIMULATOR_SMOKE_TEST`, `CROSSINK_SIMULATOR_SMOKE_PAGE_TURNS`), then
fails on any crash pattern or a missing success marker.

`--page-turns 60` matters for P1 specifically: it crosses the 50-event
auto-flush boundary of the reading event log.

### Mode B — windowed, driven and screenshotted

For seeing what actually rendered. `--window` makes SDL create a real Wayland
window instead of using the dummy driver, which Hyprland then manages like any
other client — so `grim` can capture it and `wtype` can type into it.

---

## Driving the simulator

### Keyboard map

From `docs/simulator.md`:

| Key | Action |
|---|---|
| Up / Down | Page back / forward (side buttons) |
| Left / Right | Left / right front buttons |
| Return | Confirm / Select |
| Escape | Back |
| P | Power |
| H | X4 Pro Home key — tap for Home, hold 700 ms for the reader menu (`x4-pro-simulator` only) |

### A reusable driver script

Save as `scripts/sim_drive.sh` (not committed unless it proves useful):

```bash
#!/usr/bin/env bash
# Drive a running CrossInk simulator window and capture screenshots.
# Usage: ./sim_drive.sh <shot-dir>
set -euo pipefail

export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/1000}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-1}
export HYPRLAND_INSTANCE_SIGNATURE=${HYPRLAND_INSTANCE_SIGNATURE:-$(ls "$XDG_RUNTIME_DIR/hypr" | head -1)}

SHOTS="${1:?screenshot output directory required}"
mkdir -p "$SHOTS"

hyprctl dispatch dpms on >/dev/null

# Locate the simulator window. The SDL window class is "program" (the binary
# name); match on title as a fallback if that changes.
sim_geometry() {
  hyprctl -j clients | python3 -c '
import json, sys
for w in json.load(sys.stdin):
    if "program" in (w.get("class") or "") or "CrossInk" in (w.get("title") or ""):
        x, y = w["at"]; cw, ch = w["size"]
        print(f"{x},{y} {cw}x{ch}")
        break
'
}

focus_sim() {
  hyprctl dispatch focuswindow "class:^(program)$" >/dev/null 2>&1 || true
  sleep 0.2
}

# Capture just the simulator window, not the whole 5120x1440 desktop.
shot() {
  local name="$1"
  local geom
  geom="$(sim_geometry)"
  if [ -z "$geom" ]; then
    echo "simulator window not found" >&2
    return 1
  fi
  timeout 20 grim -g "$geom" "$SHOTS/$name.png"
  echo "captured $SHOTS/$name.png ($geom)"
}

# Send a key to the focused window. wtype -k takes an xkb keysym name.
key() {
  focus_sim
  wtype -k "$1"
  sleep "${2:-0.4}"   # let the e-ink refresh simulation settle before capturing
}

# --- example flow -----------------------------------------------------------
shot 00-home
key Down;   shot 01-after-page-forward
key Return; shot 02-selected
key Escape; shot 03-back
```

The `sleep` after each key is not optional: the simulator models e-ink refresh,
so a screenshot taken immediately after a keypress captures the previous frame.

### Capturing a window rather than the whole desktop

`grim -g "<x>,<y> <w>x<h>"` crops. Pull the geometry from `hyprctl -j clients`
(`at` and `size`), as the script above does. Capturing the full 5120×1440
desktop produces a ~20 KB–2 MB PNG dominated by everything that is not the
simulator.

### Floating the window at a fixed size

Tiling moves the window between runs, which makes screenshots hard to compare.
Pin it:

```bash
hyprctl keyword windowrulev2 'float, class:^(program)$'
hyprctl keyword windowrulev2 'size 800 480, class:^(program)$'
hyprctl keyword windowrulev2 'move 100 100, class:^(program)$'
```

800×480 matches the device framebuffer exactly (`800 * 480 / 8 = 48000` bytes),
so screenshots are 1:1 with what the hardware would show.

---

## Verifying BookOrbit work specifically

The simulator has real limits that matter for this project — from
`.claude/CONTEXT.md` and `docs/simulator.md`:

- **No image rendering.** `platformio.ini` ignores `hal`, `PNGdec`, and
  `JPEGDEC`. `JPEGDEC fallback: open failed (err=-1)` is expected, not a bug.
  This means P5 catalog **thumbnails cannot be verified in the simulator** — that
  check is hardware-only.
- **`HalStorage` uses POSIX files under `./fs_`** and permits multiple readers,
  unlike SdFat on hardware, which allows only one open reader per path. A
  file-handle bug can therefore pass in the simulator and fail on device.
- **`esp_deep_sleep_start()` is a no-op**, so sleep/wake timing around the
  reading event log's RTC gating cannot be verified here.

What each phase *can* be verified with:

| Phase | Simulator check |
|---|---|
| P0 | Settings screen renders; connection-test states (connected / bad password / unreachable) each screenshot correctly. Point at a local mock server on `127.0.0.1`. |
| P1 | Run with `--page-turns 60`, then inspect the isolated `fs_` for `events.bin` and confirm its size is a multiple of 16 bytes. Screenshot the stats screen before and after. |
| P2 | Feed a known xpointer from `imprint-dev-books/*.sdr`, screenshot where the reader lands, confirm it matches the expected chapter. |
| P3 | Screenshot the rating UI at each of 1–5 stars and cleared. |
| P4 | Create highlights, screenshot the annotation list, confirm the exchange payload in the log. |
| P5 | Browse the catalog against a mock server. **Thumbnails will not render** — verify list layout and pagination only. |

### Inspecting the isolated filesystem

The smoke test deletes its temp `fs_` on exit. To keep it, run the binary
directly instead:

```bash
pio run -e x4-pro-simulator
mkdir -p /tmp/sim-run/fs_/books
cp test/epubs/test_br_section_break.epub /tmp/sim-run/fs_/books/
cd /tmp/sim-run && /home/monish/repos/crossink/.pio/build/x4-pro-simulator/program
# afterwards:
ls -la /tmp/sim-run/fs_/.crosspoint/
xxd /tmp/sim-run/fs_/.crosspoint/epub_*/events.bin | head
```

Clear `./fs_/.crosspoint/` between runs when testing anything that touches the
EPUB cache or a binary format — stale cache output is the usual cause of "my
change did nothing".

---

## If touch clicks become necessary

Only needed for touch-specific paths on `x4-pro-simulator`. Two routes, neither
available right now:

**`wlrctl`** — preferred. Uses `wlr-virtual-pointer-unstable-v1`, which Hyprland
supports, and needs no root:

```bash
# from the AUR
wlrctl pointer move 400 240
wlrctl pointer click left
```

**`ydotool`** — works anywhere but needs `/dev/uinput`, currently `root`-only
with the user not in an `input` group. Requires a udev rule plus a running
`ydotoold`. Heavier, and grants broad input-injection rights to the user — worth
avoiding unless `wlrctl` cannot be installed.

`hyprctl dispatch movecursor <x> <y>` positions the pointer and works today, but
there is no companion click dispatcher, so it is only half the story.

Neither is needed for P0–P4. Revisit at P5 if the catalog browse UI turns out to
be touch-only rather than keyboard-navigable.
