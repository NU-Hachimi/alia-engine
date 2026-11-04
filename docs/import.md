# 导入外部美术：glTF、PNG 和 `alia assets`

> **定于 2026-11-04**（Tomo 那边提的，用户确认：排在纹理 T3b 前面，分两步）。**I1 已经做了**（main 上，还没发布）；I2 是计划，做的时候
> 按这里来，改了计划就改这份文档。

原则：开发时有原始美术（Blender 文件、导出的 glTF、PNG），卡带里只有做好的资产。这一条以前就成立：`alia cart` 只带 `assets/`，不带
`pipeline/`（`project.md` §1）。缺的是从美术工具的文件到 `assets/` 的通用一步，原来每个游戏都自己写（Megalith 用自己的 JSON 生成网格）。
现在引擎给这一步：导入库 `alia_import`，加上一个命令 `alia assets`。

不归引擎管的：一个部件是什么意思（插件的事，比如 Tomo 的脸部件），一个游戏要构建哪些源文件（游戏的事）。

## 1. 命令：`alia assets`

```sh
alia assets            # 只构建变了的（assets/.alia-assets.json 记着每个产物是从什么做的）
alia assets --force    # 全部重新构建
alia assets --check    # 在内存里构建、和提交的比：过时了就失败（CI 用）
```

在运行时镜像里跑（和 `alia test`、`alia exec` 一样），美术不用装任何东西。导入工具拷进项目的 `build/tools/`，不进卡带。产物写进 `assets/`，
**提交进仓库**（和以前一样：发布时资产已经定好，打卡带不跑生产线）。同样的源文件总是得到同样的字节，所以 `--check` 能在 CI 里查"有没有忘了重新构建"。

`alia.toml`：

```toml
[assets]
build = "pipeline/build.py:build"         # 可选：游戏自己的构建函数 build(ctx)，在资源包之后跑（§3）

[[assets.pack]]                           # 一个资源包；可以有好几个
out = "assets/props.apak"
glb = ["pipeline/models/*.glb"]           # 每个文件：import_glb，prefix = 文件名
png = ["pipeline/textures/*.png"]         # 每个文件：add_png，名字 = 文件名
options = { colors = "palette", mipmaps = true }      # 这个包里每个文件的选项（§2）
[assets.pack.files."pipeline/textures/floor.png"]    # 只给一个文件的
wrap = "repeat"
```

- **增量**：每个产物的"钥匙"是它所有输入文件的内容、选项、导入器的版本；钥匙没变、产物也没被手改过，就跳过。`.gltf` 旁边的 `.bin` 和图片也算输入。
- **记录文件** `assets/.alia-assets.json`：提交进仓库（只有哈希，内容是确定的），不进卡带。
- 不认识的键、匹配不到文件的通配符：报错或警告，不会悄悄忽略。

## 2. 导入库：`alia_import`

`tools/alia_import.py`，纯 Python、没有依赖。`alia assets` 用它，游戏或插件自己的构建脚本也可以直接用：

```python
from alia_pack import PackWriter
from alia_import import import_glb, add_png, read_png, read_glb

w = PackWriter()
names = import_glb(w, "pipeline/models/crate.glb",
                   prefix="crate",        # 包里的名字：网格 "crate"，材质 "crate/<材质>"，纹理 "crate/<图片>"；默认是文件名
                   merge=True,            # True：整个场景合成一个网格、节点的变换烘焙进去（道具）；False：每个带网格的节点一个，"crate/<节点>"，世界坐标
                   colors="vertex",       # 或 "palette"：每个三角形取最近的调色板颜色（掌机的平面风格）；palette= 换一套调色板
                   shading="smooth",      # 或 "flat"、"unlit"
                   scale=1.0,             # 文件不是米的时候
                   filter=None,           # 纹理过滤；默认用文件里采样器的 magFilter，没有就 nearest
                   mipmaps=False,
                   warnings=[])           # 没带过来的东西（下表）记在这里
# -> {"meshes": [...], "materials": [...], "textures": [...]}
add_png(w, "pipeline/textures/wood.png", wrap="repeat")       # 一个纹理，名字是文件名（PackWriter.add_texture）

width, height, rgba = read_png("pipeline/parts/eyes.png")     # 像素（RGBA，从上往下）：自己在 CPU 上合成的时候
scene = read_glb("pipeline/hair/bob.glb", merge=True, scale=1) # GlbScene：.meshes（数组）、.materials、.images
```

**坐标不用转**：glTF 和引擎都是右手系、+Y 向上、模型正面朝 +Z（`kit.aim` 让 +Z 指向目标），单位都是米。

**glTF 怎么对到引擎上**：

| glTF | 引擎 |
|---|---|
| `POSITION`、`indices`（没有就按顺序）、节点的 TRS 或矩阵 | 网格的顶点、三角形；镜像的变换（行列式为负）把绕序翻过来 |
| 法线 | 不读：引擎按材质的着色方式自己算（和所有网格一样） |
| 每个 primitive | 一段（submesh），用它的材质；所有 primitive 合进一个顶点缓冲，一个网格最多 65535 个顶点（超了报错、说是哪个） |
| `COLOR_0` × `baseColorFactor`（线性） | 顶点色（转成显示值：sRGB）；`colors="palette"` 时是最近的调色板颜色 |
| `baseColorTexture` + `TEXCOORD_0` | `TEXTURE` 材质；内嵌的 PNG / JPEG 原样进包；采样器的 wrap、magFilter 带过来 |
| `alphaMode` MASK / BLEND | `CUTOUT`（阈值固定 0.5）/ `BLEND`（不透明度 = 因子的 alpha） |
| `doubleSided`、`KHR_materials_unlit` | `double_sided`、`UNLIT` |
| 金属度、粗糙度、法线贴图、遮蔽 | 丢掉（掌机的着色模型，`api.md` §2） |
| 自发光、贴图上的颜色因子、`KHR_texture_transform`、镜像重复、蒙皮、变形目标 | 不带过来，记一条警告（蒙皮的模型按静止姿势导入） |
| 线、点；Draco / meshopt 压缩；稀疏访问器；不是 PNG / JPEG 的图片 | 报错（导出时关掉） |

`.gltf`（JSON + 旁边的 `.bin` 和图片）和 `.glb` 都能读。从 Blender 导出：File → Export → glTF 2.0，格式 glb，不开压缩。

## 3. 游戏自己的构建函数

`[assets] build = "pipeline/build.py:build"`：`alia assets` 在资源包之后调用 `build(ctx)`：

```python
def build(ctx):
    src = "pipeline/parts/eyes.png"                       # 相对项目
    if ctx.stale([src], ["parts.bin"]):                   # 输出缺了、或者输入变了（--force、--check 时总是 True）
        w, h, rgba = ctx.read_png(f"{ctx.root}/{src}")
        ctx.write("parts.bin", bake(rgba))                # 相对 ctx.out（assets/），记进记录文件
    pack = ctx.writer()                                   # 一个 PackWriter
    ctx.import_glb(pack, f"{ctx.root}/pipeline/hair/bob.glb")
    ctx.write_pack("hair.apak", pack)
```

`ctx`：`root`、`sources`（`pipeline/`）、`out`（`assets/`）、`options`（`alia.toml` 的 `[assets.options]`）、`check`（`--check` 时为
True：写入只比较、不落盘；所以只通过 `ctx.write` / `write_pack` 写）、`writer()`、`write()`、`write_pack()`、`stale()`、`warn()`，
和导入库的四个函数。

## 4. I2（计划）

- **节点层级当刚体 rig**：`import_glb(..., rig="名字")`：节点是骨骼，偏移是节点的平移。rig 格式（`formats.md` 类型 3）没有骨骼的静止旋转和缩放，
  所以每个节点的旋转、缩放烘焙进它的网格，子节点按父节点的旋转摆好。glTF 动画先不做（Tomo 不需要；以后要的话：重采样成固定帧率的
  Ry·Rx·Rz 角度）。
- **插件的构建函数**：`plugin.toml` 的 `[pipeline] build = "pipeline/x.py:build"`（插件目录里的路径，`alia cart` 不拷 `pipeline/`）。
  在**插件自己的项目**里跑 `alia assets`：`ctx.out` 是插件包里的 `assets/`（`<插件目录>/<包>/assets`），产物在插件的仓库里预先做好、提交。
  在**用这个插件的游戏**里：跳过插件的构建函数，它预先做好的资产照常跟着插件拷进卡带（游戏的机器上可能根本没有插件的源文件）。以后要按游戏构建的，加一个开关。
- **预算检查**（`--check` 时报告；`--strict` 时超了算失败）：每个网格的三角形（默认 3000）、顶点接近 65535、单张纹理超过 1024、每个包和整个
  `assets/` 的纹理显存（RGBA8 + mipmap，ETC2 以后按它的大小算）对 64 MB；`[assets.budget]` 里改上限。

## 5. 顺序

| 步 | 内容 |
|---|---|
| I1 ✅ | `alia_import`（`read_png`、`read_glb`、`import_glb`、`add_png`）、`alia assets [--check|--force]`、游戏的构建函数 |
| I2 | 节点层级当 rig、插件的构建函数、预算检查 |
| 然后 | 纹理 T3b（图集）、T3c（ETC2，也进 `alia assets`）、T3d（`docs/textures.md`） |

测试：`tests/test_import.py`。
