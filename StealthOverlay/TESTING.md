# StealthOverlay — Build & Test Guide (v2)

## Prerequisites

| Requirement | Notes |
|---|---|
| Windows 11 (or Windows 10 ≥ build 19041) | Required for `WDA_EXCLUDEFROMCAPTURE` |
| Visual Studio 2022 with "Desktop development with C++" workload | Includes MSVC v143 + Windows SDK 10.0 |

---

## Build

### GUI
1. Open `StealthOverlay\StealthOverlay.sln` in Visual Studio 2022.
2. Set configuration **Release**, platform **x64**.
3. `Ctrl+Shift+B` → Build Solution.
4. Output: `x64\Release\StealthOverlayApp.exe`

### Command line
```bat
cd StealthOverlay
msbuild StealthOverlay.sln /p:Configuration=Release /p:Platform=x64 /m
```

---

## Complete Hotkey Reference

| Hotkey | Action |
|---|---|
| `Ctrl+Shift+N` | Interview: advance (show question → show answer → next question) |
| `Ctrl+Shift+C` | Interview: clear / reset to beginning |
| `Ctrl+Shift+O` | Toggle overlay visibility |
| `Ctrl+Shift+P` | Toggle capture preview window |
| `Ctrl+Alt+→` | Move overlay → right half *(default)* |
| `Ctrl+Alt+←` | Move overlay → left half |
| `Ctrl+Alt+↑` | Move overlay → narrow right column (~35% width) |
| `Ctrl+Alt+↓` | Move overlay → bottom-right strip |

---

## Test Scenarios

### Test 1 — Overlay renders with no title

Launch `StealthOverlayApp.exe`.

**Expected:**
- Overlay appears on the **right half** of the screen.
- No "STEALTH OVERLAY POC" title — just the welcome message in dim gray.
- Color scheme is **near-black** background with no blue; hints are barely visible.
- Overlay is semi-transparent (~82% opacity).

---

### Test 2 — Interview Q&A flow (core feature)

1. Press **Ctrl+Shift+N** (first press).

**Expected:** Amber badge `Q  1 / 8` appears at top. Question text below in warm white:
> *"Tell me about yourself and what you've been working on lately."*
Hint at bottom: `Ctrl+Shift+N  show answer`

2. Press **Ctrl+Shift+N** again (second press).

**Expected:** Badge changes to green `A  1 / 8`. Question disappears. Answer text appears in cool white. No code block for Q1.

3. Press **Ctrl+Shift+N** again (third press).

**Expected:** Badge changes to `Q  2 / 8`. New question (RAII):
> *"What is RAII and why is it critical in C++?"*

4. Press **Ctrl+Shift+N** again (fourth press).

**Expected:** Green `A  2 / 8` badge. Answer text about RAII. Below the answer, a **code block** appears with:
- Dark charcoal background
- `C++` label in muted green
- Code text in pale green showing `ComPtr` vs raw pointer example.

5. Continue pressing **Ctrl+Shift+N** through all 8 pairs.

**Expected:** After Q8/A8, the next press wraps back to `Q  1 / 8`.

---

### Test 3 — Clear / reset

1. While any question or answer is showing, press **Ctrl+Shift+C**.

**Expected:** Overlay returns to idle state — dim gray center text:
> *"Interview Prep Ready — Press Ctrl+Shift+N to start"*

2. Press **Ctrl+Shift+N** — should start from Q 1 again.

---

### Test 4 — Position shortcuts

Start from default (right half).

**Ctrl+Alt+←** → overlay jumps to left half of screen.

**Ctrl+Alt+→** → overlay jumps back to right half.

**Ctrl+Alt+↑** → overlay becomes a narrow column on the right (~35% of screen width, full height). Good for extra-wide monitors.

**Ctrl+Alt+↓** → overlay becomes a strip in the bottom-right quadrant (right 50%, bottom 40% of screen).

**Expected for all:** Overlay moves instantly. Currently displayed Q/A content is preserved. Overlay remains click-through and excluded from capture at the new position.

---

### Test 5 — Position + capture preview sync

1. Press **Ctrl+Shift+P** to start capture.
2. Press **Ctrl+Alt+←** to move overlay to left.

**Expected:** Capture preview gray mask rectangle also moves to the left half (the engine's overlay rect is updated automatically).

---

### Test 6 — Overlay invisible to screen capture

1. Open Snipping Tool (`Win+Shift+S`) or OBS/Teams/Zoom.
2. Take a screenshot or share your screen.

**Expected:** The overlay is **completely absent** from the captured image — screen looks clean as if the overlay doesn't exist.

---

### Test 7 — Click-through

With overlay visible, try clicking on desktop icons, windows, or the taskbar in the region the overlay covers.

**Expected:** Clicks pass through to whatever is underneath. The overlay is fully non-interactive.

---

### Test 8 — Focus / Alt-Tab

Press `Alt+Tab`.

**Expected:** StealthOverlay does not appear as a selectable window. No taskbar icon.

---

### Test 9 — Code blocks render correctly

Press **Ctrl+Shift+N** until you reach Q2 (RAII), then press again to show the answer.

**Expected code block:**
```
C++  ← muted green label

// BAD: manual — leaks on exception        ← pale green Consolas text
ID3D11Device* dev = nullptr;
...
```

Repeat for Q3 (move semantics), Q4 (binary search), Q5 (atomic vs mutex), Q6 (vtable), Q8 (false sharing) — all have code blocks.

Q1 (intro), Q7 (system design) have **no** code block — just answer text.

---

## Known POC Limitations

| Limitation | Fix in production |
|---|---|
| Long answers may run off-screen | Add scrollable content with WM_MOUSEWHEEL on a transparent input sink window |
| Position presets are hardcoded | Add drag-to-reposition with a semi-transparent drag handle |
| `DXGI_ERROR_ACCESS_LOST` stops capture | Auto-reinitialise on screen lock/resume, monitor change |
| Font size fixed (designed for 96 DPI) | MulDiv scaling is wired in; test at 125% and 150% display scale |

---

## Common Build/Run Issues

| Symptom | Cause | Fix |
|---|---|---|
| Black overlay, no text | GDI font creation failed | Check `GetLastError()` in debugger; usually means invalid font name |
| Hotkeys not responding | Another app claimed same combo | Change letter keys in `main.cpp` HK enum |
| Capture preview black | `DXGI_ERROR_ACCESS_LOST` | Press `Ctrl+Shift+P` twice to restart |
| Overlay shows in OBS | Windows 10 < build 19041 | Upgrade OS or accept this limitation |
| Linker error `LNK2001 d3d11` | SDK mismatch | In project properties → Linker → Input, add `d3d11.lib;dxgi.lib` explicitly |
