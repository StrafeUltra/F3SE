# F3SE

Script extender and in game Lua executor for **Fable III** (PC).

Requires the latest v1.1.1.3 of the game, works on both the Steam version and the Retail DVD version as long as its updated.

I also recommend using a GFWL emulator with the game.

## Usage

1. Copy `F3SE_Launcher.exe` and `F3SE.dll` into the game’s install folder (next to the Fable III executable).
2. Start the game with **`F3SE_Launcher.exe`**, not the stock launcher.
3. In game, press **F8** to open the executor.

The window has a Kore/Lua editor (syntax highlighting for this VM), an output pane, and **Execute**.

| Output | What you see |
| --- | --- |
| `print` | Normal log line |
| Warnings | Logged as warnings |
| Errors | Red text in the output pane, the editor marks the reported line. Hover the marker for the message. |

On first launch F3SE creates an `F3SE` folder in the game directory. Put extra `.lua` files there if you want them loaded without the GUI. Load/runtime errors from those files still go to the F8 output.

Bytecode buffers are rejected from both the GUI and `F3SE\*.lua`.

## Custom API

Installed into each script’s sandbox.

| API | Returns | Role |
| --- | --- | --- |
| `print(...)` | *(nothing)* | Write to the F8 output (all arguments, like stock `print`). |
| `wait(seconds)` | *(nothing)* | Yield this script for `seconds`, then resume on a later **LF** tick. `seconds` is a number. |
| `F3SE.list_threads()` | `table` | Snapshot of threads on the **main** game Lua state. Each entry: `name` (string), `status` (string), `ours` (boolean — F3SE created / `F3SE_` prefix). |
| `F3SE.force_complete_mistpeakdemondoor()` | `msg` | `msg` is a string. Finds the official thread named `QD030_DemonDoor` and drives **that** quest’s `SetState("OUTRO")`. Not a generic quest completer. Stand near the door so the official outro can play. |
| `coroutine.create(fn)` | `thread` | Sandboxed `create`. Child thread and chunk inherit the **caller’s** environment and instruction limit. `fn` must be a Lua function. |
| `coroutine.wrap(fn)` | `function` | Sandboxed `wrap`. Same inheritance as `create`. Calling the result resumes the child. |

`F3SE` is a table in the sandbox `_G`. Stock game libraries copied in that are allowed (e.g. `GUI`, `DemonDoor`) keep their usual names.
```lua
    print("hello", 1)

    wait(2)

    local threads = F3SE.list_threads()
    for i = 1, #threads do
      print(threads[i].name, threads[i].status, threads[i].ours)
    end

    local msg = F3SE.force_complete_mistpeakdemondoor()
    print(msg)
```

## Sandbox

Every GUI run and every `F3SE\*.lua` file gets:

- Its own environment table (`_G` points at that table). Scripts do not share locals or accidental globals with each other or with the main game state.
- The same env on the **thread** and the **main chunk**. `coroutine.create` / `wrap` copy that env onto the new thread and function. Children share `_G` with the parent script only.
- A **whitelist copy** of globals from the game state, minus loaders and other high risk names (`load`, `loadstring`, `loadfile`, `dofile`, `require`, `package`, `setfenv`, `getfenv`, `newproxy`, `debug`, `RunScript`, script file injectors, and similar).
  “Trusted” only means obvious escape hatches were dropped. Most engine bindings have **not** been audited for memory unsafe arguments.
- An **instruction limit** on F3SE named threads (VM hook + `hookcount`) so tight Lua loops error out instead of freezing the process.
- No bytecode load path.

Game quest threads are untouched except for `F3SE.force_complete_mistpeakdemondoor()`.

## Runtime notes

- Script start, `lua_resume`, and `wait` wakeups are driven from the game’s **LF tick** so Havok TLS and the stock scheduler stay consistent. F3SE does not run Lua on a standalone worker thread.
- F3SE raises the game **LF** tick from 15 Hz to 30 Hz and **HF** from 30 Hz to 60 Hz (keeps the stock 2:1 HF:LF ratio).
- The stock VSync path is adjusted so it can follow the display refresh rate.

Heavy or long scripts still share the LF thread with the game. Prefer `wait` over a busy loop.

## Third-party

| Project | License |
| --- | --- |
| [MinHook](https://github.com/TsudaKageyu/minhook) | BSD-2-Clause |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [ImGuiColorTextEdit](https://github.com/BalazsJako/ImGuiColorTextEdit) | MIT |

Copies of those licenses are under `thirdparty_licenses/`. F3SE’s own code is BSD-3-Clause (`LICENSE`).
