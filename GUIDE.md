# Using OpenECS

This guide is for people who use a tool made with OpenECS: how to start it, where its files are, and the first keys to learn. How OpenECS works, and every detail this guide leaves out, is in [DESIGN.md](DESIGN.md).

## Contents

1. A tool
2. Starting a tool
3. Panels and workspaces
4. The first keys
5. Settings
6. Where the files are
7. Adding plugins

## 1. A tool

A tool made with OpenECS, such as a paint program or a log viewer, is OpenECS with a **preset**: a file that names the tool's plugins and how its panels are arranged. The same program runs every tool, so what you learn here works in all of them.

## 2. Starting a tool

- From the desktop's application menu, if the tool was added there (`OpenECS --preset NAME --install`).
- From a terminal: `OpenECS --preset NAME`, with the name of a preset or the path of a preset file. `OpenECS --preset NAME FILE...` also opens the files in the tool.
- Started without a preset, OpenECS shows the **launcher**, which lists the presets and the saved sessions, and opens the one you choose.

A tool starts as its preset arranges it. With the setting `ecs.keepSession` on, it saves its arrangement when you quit and comes back to it next time; `--fresh` starts from the preset all the same. `OpenECS --help` lists every option.

## 3. Panels and workspaces

- The window is divided into **panels**, such as a canvas, a palette or a list. Panels in the same place share a **tab row**; a panel alone shows none, and a small **grip** appears when the pointer comes near its top edge.
- Drag a tab or a grip to move a panel: onto the centre of another panel to share its place, near an edge to split it, or out of the window into a window of its own. The place where it will land is highlighted.
- Drag the gap between panels to change their sizes. Where gaps meet, the pointer shows four ways, and a drag changes the sizes both ways at once.
- To keep a place for a panel you will open later, such as a split's other side, open a **Blank** panel from the list of panels (the prefix, then .) and drag it there. A panel that you open from the list while the blank panel has the focus opens in its place, beside it.
- Scroll the wheel over a tab row to show the next or the previous tab.
- Right-click a tab, a grip or the rest of a tab row, click a grip, or click the `⋮` at the end of a tab row, for the panel's menu. It is the same menu from each: close the panel or all the panels of its place, maximize, pop out, show another tab, move to another workspace or a new one, and what the panel adds.
- Middle-click a tab, or click its close button, to close its panel.
- The window's title bar holds the menus File, Window and Tools, and the tool's own buttons; at its right, the current workspace's name opens a list of the workspaces. Rest the pointer on a button to see the keys that do the same.
- A **workspace** is an arrangement of panels for one job, such as drawing or organizing. Switching between them is instant.
- Before a panel with unsaved work closes, OpenECS asks whether to save it, discard it or cancel.

## 4. The first keys

OpenECS's own keys come after one key combination, the **prefix**: Alt+W unless the tool or you change it. Press the prefix, then one more key. While OpenECS waits for that key, it shows the keys you can press now; Escape cancels.

| After the prefix  | Does                                  |
| ----------------- | ------------------------------------- |
| Arrows            | Move the focus to a neighbouring panel |
| Shift+Arrows      | Move the focused panel                |
| Tab               | Show the next tab                     |
| M                 | Maximize the panel, or restore it     |
| P                 | Pop the panel out into its own window |
| X                 | Close the panel                       |
| T                 | Reopen the last closed panel          |
| 1 to 0            | Switch to workspace 1 to 10           |
| Shift+1 to 0      | Move the panel to workspace 1 to 10   |
| Shift+N           | Move the panel to a new workspace     |
| ,                 | The settings                          |
| K                 | The list of every key, to change them |
| .                 | The list of panels, to open one       |
| ;                 | The list of plugins                   |
| /                 | The launcher                          |
| S, O              | Save the session, open one            |
| Q                 | Quit                                  |

Other keys belong to the panels: each panel's keys work while it has the focus. Click a panel to focus it. The key list (the prefix, then K) shows every key by where it works, and changes, removes or adds one.

## 5. Settings

The settings window (the prefix, then ,) shows every setting with its value and what it does, and changes it. Its changes go to your settings file, which you can also edit by hand; it is a Lua file:

```lua
return {
  ["ecs.focus"] = "hover",        -- the panel under the pointer has the focus
  keys = { prefix = { Q = false } }, -- no quit key after the prefix
  tools = {
    ["org.example.Paint"] = { ["ecs.keepSession"] = true }, -- for one tool only
  },
}
```

What you set wins over the tool's preset and over the defaults. A setting for one tool goes under `tools`, by the tool's id. DESIGN.md, section 12, explains the file.

## 6. Where the files are

On Linux:

| What                    | Where                                          |
| ----------------------- | ---------------------------------------------- |
| Your settings           | `~/.config/OpenECS/settings.lua`               |
| Your presets            | `~/.config/OpenECS/presets/`                   |
| Your plugins            | `~/.local/share/OpenECS/plugins/`              |
| Saved sessions          | `~/.local/share/OpenECS/sessions/`             |
| A tool's last session   | `~/.local/state/OpenECS/<tool id>/session.lua` |
| The log                 | `~/.local/state/OpenECS/OpenECS.log`           |

The `XDG_CONFIG_HOME`, `XDG_DATA_HOME` and `XDG_STATE_HOME` variables move them, as for other Linux programs. On Windows, they are all in `%APPDATA%\OpenECS\`. The plugins and presets that come with OpenECS are in `plugins/` and `presets/` next to the program.

## 7. Adding plugins

A plugin is a folder. Put it in your plugins folder, and name it in your settings' `plugins` to load it in every tool:

```lua
return {
  plugins = { "my-scripts" },
}
```

The list of plugins (the prefix, then ;) turns plugins on and off while the tool runs. A plugin in your folder with the name of one that came with OpenECS replaces it. To write plugins, start from [OpenECS-template](https://github.com/omerfuyar/OpenECS-template) and the [examples](https://github.com/omerfuyar/OpenECS-examples).
