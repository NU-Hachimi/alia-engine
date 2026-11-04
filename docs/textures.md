# 纹理

> **规划定于 2026-11-04**（用户确认：按 T1 → T2 → T3 的顺序，默认 `nearest` 过滤）。**T1、T2 已经做了**（main 上，还没发布）；T3 是计划，
> 做的时候按这里来，改了计划就改这份文档。

引擎原来没有纹理：材质只有调色板色和顶点色，网格没有 UV，HUD 只能画矩形、文字和相机画面。下面把用到纹理的几件事放在一起规划：
图片（相册、照片、截图）、带纹理的材质、相机画面当纹理、HUD 里的图片和以后的精灵（E7）、公告板粒子。它们共用同一个"纹理"对象。

## 1. 纹理对象

```python
tex = e.load_texture("art/wood.png")             # 卡带 content/ 里的文件，或者 bytes（照片：ae.save.read(...)）
tex = e.load_texture(jpeg_bytes, max_size=1024, filter="linear")  # 大图按比例缩到最长边不超过 max_size（3280×2464 → 1024×769）
tex = e.load_texture_async(jpeg_bytes, max_size=1024)  # 在另一个线程解码：几帧以后 tex.done，然后 tex.ready 或 tex.error
tex = e.create_texture(w, h, rgba_bytes)         # 代码生成的（程序纹理、小图标）；tex.update(rgba_bytes) 换内容
tex.width, tex.height, tex.ready, tex.done, tex.error
tex.destroy()                                    # 不用了就放掉（显存）；之后再画它什么也不画
e.lens.texture                                 # 相机画面也是一个纹理（每来一帧更新），可以用在下面任何地方
e.stats["texture_kb"]                            # 所有纹理在 GPU 上占多少
```

- **格式**：PNG、JPEG（运行时用 stb_image 解码；引擎已经带了它，现在只开了 PNG）。GPU 上是 RGBA8。
- **过滤**：默认 `nearest`（掌机的像素感，和现在的画风一致），可选 `linear`（照片、相机画面）。
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
  `update_material` 换纹理对已经建好的网格也生效；纹理还没加载好或者已经 `destroy()`：画成白色。
- **平铺**：纹理的 `wrap="repeat"`（`load_texture`、`create_texture`），UV 超过 0..1 就重复（地面、墙）；默认 `"clamp"`。
- **半透明**：`BLEND` 时纹理的 alpha 乘 `opacity`。
- **镂空**：`alpha_mode=CUTOUT`（纹理的 alpha 低于一半就不画）：树叶、栅栏、头发。不排序、照常合批，比 `BLEND` 便宜。
- **合批**：同一个网格、同一个材质（所以同一张纹理）的实例照常一次 instanced draw；纹理多了 draw 就多，图集（第三步）把小图合成一张。
- **相机画面贴在物体上**：材质的 `texture=e.lens.texture`（电视屏幕、魔镜）；照片模式里换成那张全分辨率照片。
- **资源包**：材质可以是 `colour 2`（texture）、`alpha mode 2`（cutout），但纹理本身还不在包里：游戏加载后设 `texture`（T3 按名字引用）。
- 例子：sandbox 的 TEXTURES 展台（箱子、染色、镂空的树丛、平铺的地面、每帧更新的屏幕）。测试：`tests/test_textured_materials.py`。

## 4. 资源包里的纹理、压缩、图集（T3，计划）

- 资源包加纹理条目；打包工具（`alia_pack`，在运行时镜像里跑）把 PNG 放进去，材质按名字引用。
- **ETC2 压缩**（GLES 3.0 必有，板子不支持 BC）：显存和带宽是 RGBA8 的 1/4–1/8。打包时压缩，要一个编码器进镜像；先用 RGBA8，游戏的纹理多起来再做。
- **图集**：打包时把小图拼成一张，`src` 区域给精灵和 UI 用；和 E7 的 2D、精灵、九宫格一起做。

## 5. 也会用到纹理的

- **公告板粒子**（`api.md` §8）：朝相机的方块 + 一张软边的圆，加法混合。
- **字体**：中文字体是一张字形图集（E7）。
- **StellarAlia 导出**（E6）：底色贴图 → 纹理材质。

## 6. 顺序

| 步 | 内容 | 用它的 |
|---|---|---|
| T1 ✅ | 纹理对象（PNG / JPEG、`create_texture`、异步加载）、`hud_image`、`e.lens.texture` | 相机测试卡带的相册、照片预览、UI 图标 |
| T2 ✅ | 网格 UV、`TEXTURE` 材质、`CUTOUT`、平铺、相机画面贴在物体上 | 3D 游戏的贴图、树叶、电视屏幕 |
| T3 | 资源包里的纹理、ETC2、图集；公告板粒子 | 正式游戏的资源、E7 的 2D |

每一步都在 main 上，只加不改；发布时（和其他库一起）写进 api.md。
