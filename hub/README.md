# The hub

An experiment: a little 3D world everybody in the chat can drop into, walk around in and play in, like PlayStation
Home or [Abeto's Messenger](https://messenger.abeto.co). This is its first version: a plaza with a fountain, and a
bowling pavilion with two lanes.

`/hub` in zchat (or 🕹️ Hub at the top of its window) opens it, in a window of its own. Everybody in it is a crewmate
of their color with their name over their head:

| | |
|---|---|
| WASD (or the arrows) | walk; with Shift, run |
| Space | hop |
| drag with the mouse, wheel | look around, come closer |
| Enter | talk: what you say is said in `#general`, and shows over your head (what anybody in the hub says in `#general` does too) |
| E, near a lane | bowl: A/D move the ball, Q/E change its angle, hold Space for the power and let go to throw; Esc leaves |

Bowling is ten frames, scored as it should be (strikes and spares count the next balls). The others see your game
as you play it, and its score over the lane.

## How it is made

Handmade Hero style: one compilation unit (`win32_hub.cpp` includes everything else), no libraries but the
system's, few templates.

- `win32_hub.cpp`: the platform: the window, an OpenGL 3.3 context (made by hand with WGL, 4x multisampling), the
  keyboard and mouse, text drawn by GDI, and the pipe to zchat. It calls `hub_frame()` once a frame.
- `hub.cpp`: the game: walking and colliding, the camera, the others, the bubbles, the HUD, talking to zchat.
- `hub_bowling.cpp`: a lane: the ball and pin physics, the rules and the score.
- `hub_render.cpp`: two shaders (meshes lit by a sun and the sky, fading into the fog; textured quads for text), and
  the meshes.
- `hub_gl.h`, `hub_math.h`, `hub_platform.h`: the OpenGL functions it looks up, the math, and what the platform and
  the game say to each other.

There is no networking in the hub: zchat starts it and talks to it in lines of text on its standard input and
output (who is where, who said what), and passes them on to the others as zchat's own Game packets (see
`src/hub.cpp`). Started on its own, the hub is offline, to try it out alone.

It is built with zchat, when `ZCHAT_HUB` is on (the default on Windows; `cmake -DZCHAT_HUB=OFF` leaves it out), and
`/update` builds it too. Windows only for now: another platform needs its own `*_hub.cpp` (a window, a context, the
input and the pipe), the rest stays as it is.

## The models

The plaza, the pavilion, the crewmate, the pin and the ball are made by `assets/make_assets.py`, a Blender script
that builds them from nothing (primitives, a lathe for the pin) and writes them as `.mesh` files, plus `layout.h`:
where the colliders, the lanes and the spawn point are, so that the code and the models always agree. The files it
makes are checked in: building needs no Blender, only changing the models does:

```
blender -b --factory-startup -P hub/assets/make_assets.py
```

No add-on or MCP server is needed for that. (To have an assistant build models live in an open Blender instead,
the community [blender-mcp](https://github.com/ahujasid/blender-mcp) add-on and server do it; the hub does not
depend on it.)

## Working on it

`zchat_hub --screenshot shot.bmp` draws a few frames and saves the last one, then quits: with `--at X Z`,
`--look YAW PITCH DISTANCE`, `--bowl LANE POWER` (a throw; a power below 0 only aims) and `--frames N`, to look at
a change without playing.
