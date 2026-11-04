# 纹理

> **规划定于 2026-11-04**（用户确认：按 T1 → T2 → T3 的顺序，默认 `nearest` 过滤；T3 再分成 a → b → c → d，读回材质、`load_assets`、
> 引擎自带 etcpak 也是那天定的）。**T1、T2、T3a 已经做了**（main 上，还没发布）；T3b–d 是计划，做的时候按这里来，改了计划就改这份文档。

引擎原来没有纹理：材质只有调色板色和顶点色，网格没有 UV，HUD 只能画矩形、文字和相机画面。下面把用到纹理的几件事放在一起规划：
图片（相册、照片、截图）、带纹理的材质、相机画面当纹理、HUD 里的图片和以后的精灵（E7）、公告板粒子。它们共用同一个"纹理"对象。

## 1. 纹理对象

```python
tex = e.load_texture("art/wood.png")             # 卡带 content/ 里的文件，或者 bytes（照片：ae.save.read(...)）
tex = e.load_texture(jpeg_bytes, max_size=1024, filter="linear")  # 大图按比例缩到最长边不超过 max_size（3280×2464 → 1024×769）
tex = e.load_texture_async(jpeg_bytes, max_size=1024)  # 在另一个线程解码：几帧以后 tex.done，然后 tex.ready 或 tex.error
tex = e.create_texture(w, h, rgba_bytes)         # 代码生成的（程序纹理、小图标）；tex.update(rgba_bytes) 换内容
tex = e.load_texture("art/floor.png", wrap="repeat", mipmaps=True)   # 平铺、远处缩小也不闪
tex.width, tex.height, tex.ready, tex.done, tex.error
tex.destroy()                                    # 不用了就放掉（显存）；之后再画它什么也不画
e.lens.texture                                 # 相机画面也是一个纹理（每来一帧更新），可以用在下面任何地方
e.stats["texture_kb"]                            # 所有纹理在 GPU 上占多少
```

- **格式**：PNG、JPEG（运行时用 stb_image 解码；引擎已经带了它，现在只开了 PNG）。GPU 上是 RGBA8。
- **过滤**：默认 `nearest`（掌机的像素感，和现在的画风一致），可选 `linear`（照片、相机画面）。
- **mipmap**：`mipmaps=True` 时生成一串缩小的版本（每次 `update` 后重新生成）：纹理在屏幕上比原图小很多时（远处平铺的地面、细格子）
  不闪烁、不出摩尔纹；多占三分之一的显存。放大时照样是 `nearest` 的硬边。默认关（Tomo 的脸、HUD 图标这种不会缩得很小的用不着）。
- **大小**：最长边最多 2048（更大的按 `max_size` 缩小，按面积平均）；解码一张 800 万像素的照片在板上估计要 1 秒左右（还没测），所以有异步版本 `load_texture_async`，相册用它。
  解码中间要整张图的内存（800 万像素约 32 MB），同时解码的大图别太多（相机测试卡带的相册一次两张）。
- **预算**：板上 3.6 GB 内存，GPU 和 CPU 共用；纹理加起来建议不超过 64 MB（`e.stats` 里给出当前用量）。

## 2. HUD 里的图片（T1，已做）

```python
e.hud_image(tex, x, y, w, h, fit=False, color=0xFFFFFFFF, src=None)   # src=(u0, v0, u1, v1)：只画一部分（图集、精灵）
```

默认裁掉多出的部分填满框，`fit=True` 完整显示、居中；`color` 乘在图上（含透明度）；PNG 的透明部分透出后面的东西。还没加载好的纹理不画。
`hud_lens(...)` 就是 `hud_image(e.lens.texture, ...)`（没有画面时画黑框）。相册、照片预览、UI 图标、简单的精灵都靠它。
例子：`apps/camera/src/album.py`（相机测试卡带的相册：缩略图、全屏看、删除）。测试：`tests/test_textures.py`。

## 3. 带纹理的材质（T2，已做）

- **网格加 UV**：`ae.Mesh(..., uv=array('f', ...).tobytes())`，每个顶点两个数，(0, 0) 是图的左上角；资源包的网格加一段 UV（`formats.md`：
  放在最后，旧引擎读新包时跳过它）。`alia_kit` 有带 UV 的 `quad`、`textured_box`，`plane(..., tiles=)`。
- **材质**：`ae.Material(color=ae.ColorSource.TEXTURE, texture=tex)`；颜色 = 纹理 × 光照（和调色板色一样受太阳、角色光、雾、淡出影响；
  光照还是按顶点算，纹理按像素乘上去）。`tint` 对纹理是乘一个调色板色（把一张灰度图染成不同颜色，省纹理）。
  `update_material` 换纹理对已经建好的网格也生效；纹理是 None、还没加载好或者已经 `destroy()`：画成品红色（"缺纹理"的颜色，和真的白色纹理分得开）；相机还没有画面时是黑色。
- **精灵表**：材质的 `uv_rect=(u0, v0, u1, v1)`（默认 `(0, 0, 1, 1)`，左上角起）：网格的 uv 0..1 落在纹理的这一块上。表情、眨眼、
  闪烁的屏幕：一张纹理放几帧，`update_material` 改 `uv_rect` 换帧，什么都不用上传（Tomo 的表情就这么做，2026-11-04）。
- **改一部分**：`tex.update(rgba, rect=(x, y, w, h))`，左上角起，`w * h * 4` 字节：画画工具、换一个部件时不用整张重传。
- **平铺**：纹理的 `wrap="repeat"`（`load_texture`、`create_texture`），UV 超过 0..1 就重复（地面、墙）；默认 `"clamp"`。
- **半透明**：`BLEND` 时纹理的 alpha 乘 `opacity`。
- **镂空**：`alpha_mode=CUTOUT`（纹理的 alpha 低于一半就不画）：树叶、栅栏、头发。不排序、照常合批，比 `BLEND` 便宜。
- **合批**：同一个网格、同一个材质（所以同一张纹理）的实例照常一次 instanced draw；纹理多了 draw 就多，图集（第三步）把小图合成一张。
- **相机画面贴在物体上**：材质的 `texture=e.lens.texture`（电视屏幕、魔镜）；照片模式里换成那张全分辨率照片。
- **资源包**：材质可以是 `colour 2`（texture）、`alpha mode 2`（cutout），纹理也在包里、按名字引用（§4）。
- 例子：sandbox 的 TEXTURES 展台（箱子、染色、镂空的树丛、平铺的地面、每帧更新的屏幕）。测试：`tests/test_textured_materials.py`。

## 4. 资源包里的纹理（T3a，已做）

手绘的资产（贴图、精灵表）放进资源包，材质按名字引用，加载完就能用：

```python
# 打包（PC 上，游戏的构建脚本；tools/alia_pack.py）
w = PackWriter()
w.add_texture("crate", "art/crate.png")                                  # PNG 或 JPEG（路径或 bytes），原样放进包
w.add_texture("floor", "art/floor.png", wrap="repeat", mipmaps=True)
w.add_texture("face", "art/face_sheet.png")                              # 一张精灵表
w.add_material("crate", color="texture", texture="crate")
w.add_material("face", shading="smooth", color="texture", texture="face", alpha_mode="cutout",
               uv_rect=(0, 0, 1, 0.25))                                  # 第一帧
w.add_mesh("crate", positions, indices, uv=uv, submeshes=[(0, 12, "crate")])
w.write("content/gen/props.apak")

# 游戏里
assets = e.load_assets("gen/props.apak")       # .meshes / .materials：{名字: id}；.textures：{名字: Texture}；.rigs：{名字: Rig}
e.world.spawn(assets.meshes["crate"], position=(0, 0, 3))        # 材质已经带着纹理
m = e.material(assets.materials["face"])       # 读回材质，只改一个字段
m.uv_rect = (0, 0.25, 1, 0.5)                  # 换到第二帧
e.update_material(assets.materials["face"], m)
e.hud_image(assets.textures["crate"], 8, 8, 32, 32)              # 包里的纹理也是普通的 Texture
```

- **条目**：`formats.md` 类型 4。PNG / JPEG 原样放进包，加载时解码（一个包里的几张纹理在几个线程上同时解码），和 `load_texture` 一样上传成
  RGBA8；过滤、平铺、mipmap 写在纹理条目里。最长边超过 2048 的缩小。
- **材质按名字引用**：材质条目在第一版的字段后面加了纹理名和 `uv_rect`，旧引擎读到那里就停（读得了新包，只是没有纹理）；旧包的材质在新引擎上
  没有纹理（品红，和以前一样由游戏设）。名字只在同一个包里找；找不到：画品红色，日志里一条警告（打包工具直接拒绝）。
- **`load_assets(path 或 bytes)`**：包里的一切，一次加载（纹理 → 材质 → 网格 → rig）。出错时这个包的东西一个都不留。`load_pack` 照旧返回
  `(网格, 材质)`，纹理一样加载、接好，只是拿不到 Texture 对象；它不加载 rig（和以前一样，`load_rigs`）。
- **`e.material(id)`**：读回材质的一份拷贝（`texture` 读出来是纹理的 id，原样能再设回去），改字段后 `update_material`。
- **释放**：`tex.destroy()`、`e.destroy_material(id)`、`e.destroy_mesh(id)`，和代码里建的一样。
- 测试：`tests/test_pack_textures.py`。

## 5. 图集、压缩、公告板粒子（T3b–d，计划）

- **图集（T3b）**：打包时把小图拼成一张（`w.add_atlas("ui", {"heart": "art/heart.png", ...})`），纹理条目后面带一张区域表；材质写
  `texture="ui/heart"`，引擎把区域算进 `uv_rect`；HUD 用 `e.hud_image(tex, ..., src=tex.regions["heart"])`。小图之间留边（`nearest`、
  mipmap 不串色）。和 E7 的 2D、精灵、九宫格一起用。
- **ETC2 压缩（T3c）**：GLES 3.0 必有，板子不支持 BC。显存和带宽是 RGBA8 的 1/4（RGBA）到 1/8（RGB）。打包时压缩：编码器用 etcpak（BSD，C++），
  源码放进 `third_party/`，和引擎一起在运行时镜像里编成打包工具用的小程序（不用系统侧改镜像）。纹理条目加一个格式值；mipmap 也在打包时做好。
  PC 上的 GL 一般也能读 ETC2（模拟器、测试照常）。`load_texture` 的 PNG / JPEG 路径不变。
- **公告板粒子（T3d）**：`api.md` §6 的粒子从小网格变成朝相机的方片 + 一张纹理（默认一个软边的圆），加法或普通混合。

## 6. 也会用到纹理的

- **公告板粒子**（`api.md` §8）：朝相机的方块 + 一张软边的圆，加法混合。
- **字体**：中文字体是一张字形图集（E7）。
- **StellarAlia 导出**（E6）：底色贴图 → 纹理材质。

## 7. 顺序

| 步 | 内容 | 用它的 |
|---|---|---|
| T1 ✅ | 纹理对象（PNG / JPEG、`create_texture`、异步加载）、`hud_image`、`e.lens.texture` | 相机测试卡带的相册、照片预览、UI 图标 |
| T2 ✅ | 网格 UV、`TEXTURE` 材质、`CUTOUT`、平铺、相机画面贴在物体上 | 3D 游戏的贴图、树叶、电视屏幕 |
| T3a ✅ | 资源包里的纹理（`load_assets`）、材质按名字引用纹理、`e.material(id)`、mipmap | 手绘资产 |
| T3b | 图集 | UI、精灵、E7 的 2D |
| T3c | ETC2 压缩 | 纹理多的正式游戏 |
| T3d | 公告板粒子 | 火花、烟、魔法 |

每一步都在 main 上，只加不改；发布时（和其他库一起）写进 api.md。
