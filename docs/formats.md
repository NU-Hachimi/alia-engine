# Baked formats

What the engine reads at run time. Everything is baked on the PC (architecture §1: no asset import on the
device) and shipped inside the cart. Little-endian throughout; the board (aarch64) and the PC are both
little-endian, so arrays are read as they are.

## Resource pack `.apak`, version 1 (2026-10-29)

Written by `tools/alia_pack.py` (pure Python), read by `src/asset/pack.cpp`; in a game:
`meshes, materials = engine.load_pack("gen/meshes.apak")` (or `engine.load_meshes(...)` for the meshes) gives
`{name: id}`. Relative paths are taken from the cart's
`content/` (`$ALIA_CART_DIR/content`), or from the working directory when there is no cart.

```
header (16 B)   "APAK"  u16 version = 1  u16 0  u32 entry count  u32 directory offset
entries         each starts on a 4-byte boundary
directory       per entry: u8 type  u8 flags (0)  u16 name length  u32 offset  u32 size  name (UTF-8)
```

Names are unique per type. The writer sorts entries by (type, name), so the same input gives the same bytes
(generated files can be checked in and compared with `--check`). Readers ignore entry types they don't know,
so a newer pack with extra types still loads in an older engine; a change to an existing type's layout bumps
the version.

### Type 1: mesh

```
u32 vertex count (1..65535)  u32 triangle count  u8 colours  u8 flags  u16 0
f32 positions[vertices][3]          metres
u16 indices[triangles][3]           counter-clockwise front faces
colours bit 0: u8 palette index[triangles]
colours bit 1: u8 rgb[vertices][3]                   (after the palette indices when both are there)
flags bit 0: double-sided (every part, whatever its material says)
flags bit 1: a part table follows:
    u32 count (>= 1), then per part: u32 first triangle  u32 triangle count  u16 name length  material name
    (an empty name is the default material)
```

Without a part table the mesh is one part with the **default material**, which picks from the colours the mesh has:
palette indices → flat shaded, palette colours; otherwise vertex RGB → smooth shaded (normals averaged from the
faces). The colour values 1 and 2 of the first version of this table mean the same as before, so older packs read
unchanged. Normals are not stored: the engine derives them from the part's shading when it builds the GPU buffers.

### Type 2: material (docs/api.md §2)

```
u8 shading (0 flat, 1 smooth, 2 unlit)  u8 colour (0 palette, 1 vertex)  u8 palette index (0..31)  u8 light (0 sun, 1 character)
f32 ambient  f32 diffuse        negative: the light's own values
f32 emissive                    0..1
u8 alpha mode (0 opaque, 1 blend)  u8 flags (bit 0 double-sided, bit 1 fog, bit 2 ground fog)  u16 0
f32 opacity                     0..1, blend only
```

Meshes name materials of the same pack. The engine loads every material first; a part naming a material the pack
doesn't have is an error (the writer checks it too).

### Type 3: rig (docs/api.md §4)

```
str = u16 length + UTF-8 bytes
collider = u8 shape (0 none, 1 sphere, 2 capsule)  f32 a[3]  f32 b[3]  f32 r  u32 layers  u32 user  i8 priority  (38 B)
u16 bones; per bone:  str name  i16 parent (an earlier bone, -1 for the root)  f32 offset[3]  str mesh ("" none)
                      u8 n; n x collider (in the bone's frame)
u16 slots; per slot:  u16 bone  f32 offset[3]  f32 rot[3] (degrees)  u8 variants  str mesh[variants] ("" = empty)
                      collider (in the slot's frame; shape 0: none)
u16 attach; per point: str name  u16 bone  f32 offset[3]
u16 anims; per anim:  str name  u8 loop  u16 keys  u8 has_root_y  [f32 root_y[keys]]
                      u16 tracks; per track: u16 bone  f32 angles[keys][3] (degrees, Ry * Rx * Rz)
                      u16 events; per event: u8 phase  str type
                                             u8 n; n x (str key, str value)
                                             u8 n; n x (str key, f32 value)
```

Meshes are named, not stored: the engine resolves them against the meshes loaded from packs when a rig is spawned.
`tools/alia_pack.py` `add_rig()` takes the shape of Megalith's `assets/rigs/*.json` (colliders as `{"type", "c" or "a"/"b",
"r", "layers", "user", "priority"}`); keys the engine doesn't use (hp, tags, hookable) are left out.

**Not quantized yet.** The architecture (§1, §3) plans 16–24-byte vertices for the board's memory bandwidth.
The E2 budget (§6.2) shows no game needs it yet (Megalith draws about a thousand triangles), and float positions
keep pictures identical to the source. When a game's vertex bandwidth shows up in `alia-bench`, a mesh entry
with int16 positions (per-mesh scale and offset) and packed normals becomes a new colour/position mode, with the
GPU vertex format to match.

Still to come: textures (ETC2 in KTX2) when the 2D work and
textured materials need them (a material colour value 2 and an alpha mode for cutout will refer to them).
