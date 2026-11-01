# GPU test kit

How to find out what the GPU does on your device and send it to us. The result feeds the GPU
support table in the app and `docs/VERIFICATION.md`.

## Quick run

In the DroidDesk terminal (or in Debian, for example from Claude Code), with the desktop running:

```
droiddesk-gpu-report            # about 2 to 8 minutes, browser windows open and close
droiddesk-gpu-report --bench    # plus a short glmark2 run
```

It writes `~/droiddesk-gpu-report-<time>.md` and prints the path. Send us that file (a GitHub
issue with the file attached). It contains the device model, the GPU state and test output; no
personal files. Read it before sending if you want to be sure.

What it tests:

- the device, the GPU and the DroidDesk GPU state (Adreno, virgl opt-in, self-test result);
- OpenGL (`glxinfo -B`) and Vulkan (`vulkaninfo --summary`) on the Termux side and in Debian;
- WebGL in every browser it finds (Firefox and Chromium in Termux; Firefox, Chromium, Brave and
  Chrome in Debian): a local test page draws a known colour, checks it, renders a heavy shader for
  5 s and reports the FPS. A browser that crashes or hangs shows up as such;
- with `--bench`: glmark2 (build, texture, phong shading, terrain, refract).

It only reads state and starts programs. It changes no settings and installs nothing. Missing
tools are listed with the command that installs them.

## Running it through Claude Code or Codex

Paste this to the agent on the device:

> Run the DroidDesk GPU test kit (docs/gpu-test-kit.md in github.com/Vyvrnc/DroidDesk-Fold).
>
> 1. Make sure the DroidDesk desktop is running. Run `droiddesk-gpu-report --bench` and wait for
>    it; browser windows will open and close by themselves. Do not touch them.
> 2. If it reports missing tools (glxinfo, vulkaninfo, glmark2), ask me before installing
>    anything. Then run it again.
> 3. Do the manual checks in the report's "Manual checks" table with what is installed. Do not
>    install apps for this, and leave a row empty if the app is missing:
>    - an Electron app (VS Code, Obsidian, Claude Desktop): does it start, is the window drawn;
>    - a 3D app (OrcaSlicer, FreeCAD, Blender): open a model, rotate it;
>    - YouTube 1080p in a browser for 30 s: smooth or not, and the CPU load from `top`.
>    Write what you saw into the table, with the app version.
> 4. Do not change GPU settings (`droiddesk-gpu`, `droiddesk-native-gpu`, browser flags) and do
>    not edit files outside the report.
> 5. At the end tell me the report path and a five-line summary: GPU, renderer, which browsers
>    passed WebGL, what crashed, anything surprising.

## Reading the results

WebGL verdicts:

| Verdict | Meaning |
|---|---|
| `ok` | WebGL works, the drawn pixel is right, FPS measured |
| `wrong-pixels` | WebGL runs but draws wrong colours |
| `context-lost` | the GPU process dropped the WebGL context |
| `no-webgl` | the browser offers no WebGL (blocklisted or GPU off) |
| `exception` | shader compile or link failed (see `error`) |
| `browser-exited-N-after-load` | the browser crashed while drawing |
| `browser-exited-N-before-load` | the browser did not start |
| `timeout-after-load` / `timeout-no-load` | it hung |

The `Renderer` column tells which path the browser took: `ANGLE (... Vulkan ...)`, `zink`,
`virgl` or `llvmpipe` / `SwiftShader` (software).

Bench numbers from different Mesa builds are not comparable; the report records the Mesa
version in "Debian GPU state".
