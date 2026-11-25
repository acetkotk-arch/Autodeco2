# Auto Deco (Geode mod)

A one-click procedural decorator for the Geometry Dash editor.

## Use it
1. In the editor, **select a few decoration objects** you like (gears, glow orbs, particles, ...). This is your palette.
2. Open the **pause menu** -> press **Auto Deco** (bottom right).
3. Don't like it? Press **Undo Deco**, change Style / Density / Seed in the mod settings, run again.

No selection? Type object IDs into the **Palette IDs** setting instead (comma-separated). Non-decoration IDs are ignored.

> Back up your level first. Auto Deco does not use the editor's own Ctrl+Z history; use the Undo Deco button.

## Build it

### Easiest: GitHub Actions (nothing to install)
1. Create a GitHub repo and upload this whole folder.
2. Open the **Actions** tab -> run **Build Binaries**.
3. Download the `.geode` file(s) from the build output and put them in your Geode `mods` folder
   (or open the `.geode` with Geometry Dash + Geode installed).

### Local build
1. Install the Geode CLI and SDK: https://docs.geode-sdk.org/getting-started/
2. `geode build` in this folder.

## Before you build
- In `mod.json`, change `yourname` in `"id"` and `"developer"` to your own name (id format: `name.modname`, lowercase).
- If Geode says the mod needs a different `geode` version, set `"geode"` to the version shown in your Geode.

## How the "AI" works
It is rule-based, not a neural network. It scans your blocks, detects exposed faces, outer corners, hazards and background gaps, then places palette objects with style-dependent probabilities, rotations and sizes, with a per-cell limit so deco doesn't pile up.

## Known limits (v1.0)
- Doesn't set colors, glow, Z-layers or triggers (pulses/shaders/glitch effects) - deco uses each object's default look.
- Deco can end up in front of gameplay objects, since Z-layers aren't touched.
- If `updateCustomScaleX/Y` fails to compile on your bindings, replace the body of `applyScale()` in `src/main.cpp` with `obj->setScale(s);`.
