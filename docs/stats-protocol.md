# Stats protocol

The engine's performance figures, for any tool that reads a game's output (a dashboard, a test runner, a
person reading a cart's log). Version 1, 2026-10-29.

## Turning it on

`--stats` (a common flag every game accepts, also as cart launch arguments) or `ALIA_STATS=1`.
Off by default.

## Where it goes

One line per second on stdout. For a cart that is its log (`alia logs`, `docs/project.md`), which a tool can
follow (`tail -F`). Nothing extra is needed: no socket, no new path.

## Format

```
alia-stats {"fps": 59.9, "steps": 60, "update_ms": 3.81, "render_ms": 1.02, "present_ms": 11.70, "worst_frame_ms": 18.40, "late": 0, "instances": 78, "culled": 41, "triangles": 953, "voices": 1, "audio_late": 0}
```

The prefix `alia-stats ` followed by one JSON object on the same line. Consumers ignore keys they don't know.
New keys can be added without a version change; changing what an existing key means bumps the version, which will
then be a `"v"` key (absent means 1).

| Key | Meaning (over the last second) |
|---|---|
| `fps` | rendered frames per second |
| `steps` | fixed logic steps run (60 at the default 60 Hz) |
| `update_ms` | mean time per frame in the game's `fixed_update` calls |
| `render_ms` | mean time per frame in the game's `render` plus building and submitting the frame, up to the swap |
| `present_ms` | mean time per frame in the swap; in a window that is mostly waiting for vsync, i.e. time to spare |
| `worst_frame_ms` | longest gap between two presented frames |
| `late` | frames where more time passed than the runtime catches up on (6 steps): the game slowed down; a gap over 0.25 s (frozen by the system, or a long stall) is dropped entirely and counts here too |
| `instances`, `culled`, `triangles` | last frame's 3D draw list: instances submitted, culled by the frustum, triangles drawn |
| `voices` | sounds playing in the mixer now (music and effects) |
| `audio_late` | audio callbacks so far that came more than 1.5 buffers after the previous one: likely dropouts (0 without a device). Cumulative, not per second |

The CPU cost of a frame is `update_ms + render_ms`; at 60 Hz `present_ms` is roughly what is left of the 16.7 ms.
The budget in `architecture.md` §6.2 says how much of each is available. GPU time (timer queries) is still to come.
