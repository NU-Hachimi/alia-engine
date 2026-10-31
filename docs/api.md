# alia-engine：游戏 API

> **v0.1.0**（2026-10-30，E4 定稿）。从 §0 快速开始读起；接口在 §1–§7，还没有的在 §8，定下来的决定在 §9。
> 从 v0.1.0 起接口保持兼容：只加不改；要改的先在这里标"将要变"，下一个次版本再变（`alia_engine.__version__`）。
> Python 模块 `alia_engine`（下面写作 `ae`），C++ 有同样的一套（`src/runtime/`、`src/world/`、`src/audio/`）。
> 分层和分工见 `architecture.md` §2、§2.1；资源格式见 `formats.md`；性能预算见 `architecture.md` §6.2；
> 示例：`apps/sample/`（示例游戏）、`apps/sandbox/`（每个特性一个展台）、Megalith（`NU-Hachimi/megalith`）。从零开始做一个游戏：`docs/getting-started.md`；项目怎么放文件、`alia` 的命令：`docs/project.md`。

## 0. 快速开始

一个完整的小游戏（`alia new hello` 生成的项目里，把它放进 `src/main.py`，再 `alia run`；`alia run --headless --frames 120 --screenshot shot.png` 不开窗口跑。项目布局和 `alia` 工具见 `docs/project.md`）：

```python
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "python"))   # 卡带里的引擎（content/python）
import alia_engine as ae
import alia_kit as kit

class Game:
    def init(self, e):
        w = e.world
        e.actions.load({"move": {"stick": "left", "dead_zone": 0.15}, "turn": {"stick": "right"}})
        kit.environment(e, "day")                                         # 天空、雾、太阳（§7.1）
        kit.Terrain(e, size=100, cell=4, height=kit.hills(seed=1))        # 看得见的山坡，也是能站的地面
        g = w.ground_height
        self.box = w.spawn(e.create_mesh(kit.box(2, 2, 2, kit.pal.RED)), position=(0, g(0, 6) + 1, 6))  # 红色的箱子
        self.box.collider = ae.capsule((0, -1, 0), (0, 1, 0), 1.0, layers=1)               # 挡人
        self.me = w.spawn(e.create_mesh(kit.box(0.8, 0.8, 0.8, kit.pal.BLUE, centre=(0, 0.4, 0))))  # 蓝色的"角色"，原点在脚下
        self.mover = kit.GroundMover(w, position=(0.0, 0.0), radius=0.4, layers=1)
        self.cam = kit.FollowCamera(w.camera)

    def fixed_update(self, e, dt):                                   # 60 Hz：改物体、读输入都在这里
        a = e.actions
        self.cam.turn(a.axis("turn")[0], dt)
        self.mover.move(kit.stick_to_world(a.axis("move"), self.cam.yaw), dt)
        self.me.position, self.me.yaw = self.mover.position, self.mover.yaw
        self.cam.follow(e.world, self.mover.position, dt=dt)
        self.box.yaw += dt

    def render(self, e, alpha):                                      # 每显示一帧：只画 HUD
        kit.say(e, 8, 8, "WASD / LEFT STICK", kit.WHITE, 2)

ae.run(Game(), argv=sys.argv[1:], title="Hello", app_id="com.example.hello")
```

形状、地面和光都是 `alia_kit` 现成的（§7.1）；自己做网格见 §2 和 §0.1 的"网格"一行。再往下看：示例游戏 `apps/sample/`（README 讲了推荐的组织方式）、sandbox `apps/sandbox/`（每个特性一个展台）。

### 0.1 约定

| 什么 | 约定 |
|---|---|
| 单位 | 米、秒、弧度（rig 的关键帧和部件槽的旋转是度，`formats.md`） |
| 坐标 | 右手系，Y 向上。相机 yaw 为 0 时看着 +Z，这时 +X 在画面**左边** |
| yaw / pitch / roll | 绕 +Y / +X / +Z，合成顺序 Ry · Rx · Rz；相机的 `pitch` 为正是抬头；物体的 `pitch` 为正时 +Z 那头朝下 |
| 摇杆 | `Input` 和 `Actions.axis` 都是 -1..1，+x 向右，**+y 向下**（往前推是负的）；`kit.stick_to_world` 换成地面上的方向 |
| 网格 | 位置 `array('f')`、索引 `array('H')` 的 bytes；三角形**从外面看逆时针**是正面；`create_mesh` 的 id 从 1 起，0 是"没有网格"（空物体） |
| 高度图 | `heights[j * n + i]` 是 (`origin_x + i·cell`, `origin_z + j·cell`) 处的高度：一行一行沿 z 排 |
| 颜色 | 网格、`tint`、粒子：调色板索引（`tint=0` 是"不换色"，所以索引 0 不能当 tint）；`e.frame` 的天空和雾：0xRRGGBB；HUD 和 `overlay`：0xRRGGBBAA |
| 两种像素 | HUD 用场景像素（`e.scene_size`，默认 640×360）；截图是输出尺寸（窗口或 `--size`） |

### 0.2 容易踩的坑

- **只在 `fixed_update` 里改物体**：引擎在两步之间插值。`render` 里改了位置的物体，要 `teleport()`，否则会被当成"上一步到这一步"的运动滑过去；对象池里挪了位置的物体也要 `teleport()`。
- **`set_ground` 不画东西**：它只给 `ground_height`、`ray_ground`、粒子落地用；地面的网格要另外建。`kit.Terrain` 两件一起做（快速开始里那样）。
- **`animator.play()` 从头开始放**：每步调它，动画永远停在第一帧；只在换动画时调，平时改 `animator.speed`。
- **`--demo` 之类的脚本输入**：引擎没有"往输入里塞按键"的接口（`Input` 只读）。做法是在动作之上包一层：游戏每步从一个"控制"对象拿 `move`、`turn`、`start`……，真玩时它读 `e.actions`，演示时它按脚本给（示例游戏的 `DemoInput`）。脚本跑的时候别写真正的存档。
- **头看目标会抖**：见 §4.1 的三条，用 `kit.Gaze`。
- **存档**：`ae.save.write(name, bytes)` 只收 bytes（JSON 先 `.encode()`）；卡带外面的存档目录按 `ae.run(app_id=…)` 分开（不给就和别的游戏共用 `pad.alia.engine`）。
- **调色板和灯有默认值，但都是"能看"的程度**：有画风的游戏自己 `set_palette`，自己设 `e.frame.sun.travel`。

## 1. 主循环和 Engine

### 1.1 主循环

```python
class Game:
    def init(self, e): ...                 # 可选
    def fixed_update(self, e, dt): ...     # 固定 60 Hz；e.input.pressed 每次按下只在一步里出现
    def render(self, e, alpha): ...        # 每显示一帧一次；alpha：0..1，到下一步走了多少
    def shutdown(self, e): ...             # 可选；SIGTERM、异常时也会调用

ae.run(Game(), argv=sys.argv[1:], title="Game", fixed_hz=60)
ae.strip_runtime_flags(args) -> list       # 去掉运行时的通用参数，剩下游戏自己的
```

```python
ae.run(Game(), argv=sys.argv[1:], title="Game", fixed_hz=60, app_id="com.example.mygame")
```

- `app_id`：卡带外面（PC、测试）用它给存档目录命名（§7）；在卡带里由清单的 `[cart] id` 和系统的 `ALIA_CART_ID` 决定。
- `argv` 里引擎认得的参数它自己用，**认不得的留给游戏**：游戏用 `ae.strip_runtime_flags(sys.argv[1:])` 拿到剩下的（比如自己的 `--demo`），交给 `argparse`。

| 参数 | 环境变量 | 作用 |
|---|---|---|
| `--headless` | `ALIA_HEADLESS=1` | 没有窗口（EGL surfaceless）；时钟是模拟的，每画一帧走一步，`alpha` 总是 1；不开声音设备（`e.audio.mix()` 手动渲染） |
| `--frames N` | `ALIA_FRAMES` | 画 N 帧就退出 |
| `--screenshot P` | `ALIA_SCREENSHOT` | 退出前把最后一帧存成 PNG（输出尺寸：窗口或 `--size` 的大小，比如 1280×720；HUD 坐标是场景像素，640×360） |
| `--stats` | `ALIA_STATS=1` | 每秒在标准输出打一行 `alia-stats {...}`（`stats-protocol.md`） |
| `--input-log` | `ALIA_INPUT_LOG=1` | 记录按键和手柄映射 |
| `--no-interpolate` | `ALIA_NO_INTERPOLATE=1` | 不在两步之间插值（看插值的效果） |
| `--size WxH` | `ALIA_DISPLAY_SIZE` | 窗口 / 输出尺寸 |
| `--render WxH` | `ALIA_RENDER` | 场景（内部）分辨率；默认来自卡带清单的 `[display] render`，再默认 640×360 |
| `--rotate 0/90/180/270` | `ALIA_DISPLAY_ROTATION` | 最终输出旋转（竖屏） |

PC 上键盘代替手柄：WASD 左摇杆，方向键右摇杆，空格 / 回车 East（A），J South（B），K North（X），L West（Y），Q / E L1 / R1，Tab Select，退格 Start，F L3，G R3，Esc 退出。按键是按位置的（`ae.SOUTH` 等），Alia Pad 的标签是任天堂式的：A = East，B = South，X = North，Y = West。

### 1.2 Engine（`e`）

| 接口 | 说明 |
|---|---|
| `e.input` | `Input`：`held`、`pressed`（按位置的按键位：`ae.SOUTH/EAST/WEST/NORTH/L1/R1/L2/R2/L3/R3/SELECT/START/UP/DOWN/LEFT/RIGHT`）、`lx ly rx ry`（-1..1，y 向下）、`lt rt`、`gamepad` |
| `e.create_mesh(ae.Mesh(positions, indices, tri_palette=…, vertex_rgb=…, double_sided=False, submeshes=[…]))` → id | 几何用 bytes（`array('f')`、`array('H')`）；`update_mesh(id, mesh)`、`destroy_mesh(id)`；分段和材质见 §2 |
| `e.create_material(ae.Material(...))` → id、`e.update_material(id, m)` | §2 |
| `e.load_pack(path)` → `({网格名: id}, {材质名: id})`、`e.load_meshes(path)` → `{网格名: id}` | 资源包 `.apak`；相对路径取卡带的 `content/` |
| `e.set_palette(rgb_bytes)` | 32 色调色板：最多 32 个 RGB 三元组（`bytes(r, g, b, r, g, b, …)`），索引 0 起。**不调也有一套默认的**（0 黑、1 白、2–5 灰、6 红、7 橙、8 黄、9 黄绿、10 绿、11 青、12 蓝、13 藏青、14 紫、15 品红、16 粉、17 棕、18 棕褐、19 米白、20 草绿、21 深绿、22 石灰、23 石板、24 天蓝、25 暮蓝、26–31 发光用的亮黄、珊瑚、亮青、亮黄绿、亮粉、近白）；有自己画风的游戏换掉它 |
| `e.frame` | 天空、雾、灯、叠色，帧与帧之间保留（下表）；相机字段跟着 `w.camera`（§3），用世界的相机就不用管 |
| `e.draw(mesh, model, tint=0, light=ae.Light.MATERIAL, fade=0, ground_fog=True)` | 本帧画一个实例；`model`：行主序 3×4（12 个数）；其余参数是实例覆盖（§2） |
| `e.hud_rect(x, y, w, h, color)`、`e.hud_text(x, y, text, color, scale, shadow)` | 场景像素坐标，颜色 0xRRGGBBAA |
| `e.screenshot(path)`、`e.scene_size`、`e.stats`、`e.describe()` | `scene_size`：场景（HUD）像素，默认 (640, 360) |

`e.frame` 的字段（颜色是 0xRRGGBB 的整数；距离是米；角度是弧度）：

| 字段 | 默认 | 说明 |
|---|---|---|
| `sky_top`、`sky_horizon`、`sky_span` | 0x4060A0、0xA0B0C0、0.55 | 天顶和地平线的颜色；地平线往上多少弧度到天顶色 |
| `fog_color`、`fog_near`、`fog_full`、`fog_steps` | 0xA0B0C0、40、110、16 | 距离雾：`fog_near` 米开始，`fog_full` 米全是雾色，分 `fog_steps` 档（0：关） |
| `ground_fog`、`ground_top`、`ground_ramp` | 0、9、30 | 贴地雾：强度 0..1、高度（米）、多远（米）到全强 |
| `sun`、`character` | `travel` (0.35, −0.82, 0.45)，`ambient` 0.5，`diffuse` 0.5 | 两盏平行光：`travel` 是**光走的方向**（从光源指向被照的东西，长度随意），可以直接改 `e.frame.sun.travel = (…)`；材质选用哪盏（§2） |
| `overlay` | 0 | 全屏叠色 0xRRGGBBAA（受伤闪红），A 为 0 不画 |
| `eye`、`yaw`、`pitch`、`fov_x`、`near`、`far` | | 相机；用了 `w.camera` 就由它决定（§3） |
| `e.world`、`e.actions`、`e.audio`、`e.rumble(...)` | 世界（§3）、输入动作和声音（§7）；常用的相机、角色控制器在 `alia_kit`（§7.1） |
| `e.particles(...)` | 粒子模板（§6） |

## 2. 材质

掌机的着色模型，不是 PBR（带宽，`architecture.md` §1）。字段名尽量和 StellarAlia 的 `MaterialData` 一致（`doubleSided`、`alphaMode`、`shadingModel`）；E6 导出时 StellarAlia 的 PBR 材质按规则转成这里的材质（底色 → 最近的调色板颜色或纹理，金属度、粗糙度丢掉）。

```python
glow = e.create_material(ae.Material(shading=ae.Shading.UNLIT, palette_index=9, emissive=1.0, fog=False))
glass = e.create_material(ae.Material(palette_index=3, alpha_mode=ae.AlphaMode.BLEND, opacity=0.5))
rune = e.create_mesh(ae.Mesh(positions, indices, tri_palette=colors,
                             submeshes=[(0, 10, 0), (10, 4, glow)]))   # (first_tri, tri_count, material id)
meshes, materials = e.load_pack("gen/props.apak")                     # 或者放进资源包（formats.md 类型 2）
e.update_material(glass, ae.Material(...))
```

| 字段 | 取值 | 说明 |
|---|---|---|
| `shading` | `FLAT`（默认）/ `SMOOTH` / `UNLIT` | 面法线 / 顶点法线 / 不打光 |
| `color` | `PALETTE`（默认）/ `VERTEX` | 网格的每三角形调色板索引 / 每顶点 RGB。纹理等纹理做出来以后 |
| `palette_index` | 0–31 | 非 0：整段都用这个颜色（不需要网格的 `tri_palette`） |
| `light` | `SUN`（默认）/ `CHARACTER` | 用哪盏灯 |
| `ambient`、`diffuse` | 0..1，负数 = 用灯的默认值 | |
| `emissive` | 0..1 | 从受光的颜色混向不受光的颜色（发光的符文、火花）；1 等于不打光 |
| `alpha_mode`、`opacity` | `OPAQUE`（默认）/ `BLEND` | 半透明的段在所有不透明之后、按实例中心从远到近画，测深度不写深度（有填充率代价，`architecture.md` §6.2）。`cutout` 等纹理 |
| `double_sided` | bool | 不剔除背面 |
| `fog`、`ground_fog` | bool | 是否受距离雾、贴地雾影响 |

- **网格分段**：`submeshes` 把三角形分成若干段，每段一个材质；不给就是一段、用默认材质（id 0：有 `tri_palette` 就是平面着色的调色板颜色，否则是平滑着色的顶点颜色——和材质出现之前一样）。
- **什么时候生效**：`shading`、`color`、`palette_index` 在建网格时烘焙进顶点；其他字段每次画都读，所以 `update_material()` 对已有的网格立刻生效。
- **实例覆盖**（`e.draw()` 的参数，以后是世界物体的属性）：`tint`（把调色板颜色换成一个）、`fade`、`ground_fog=False`（压过材质）、`light`（默认 `MATERIAL` = 用材质的；`SUN` / `CHARACTER` / `UNLIT` 覆盖）。对应 StellarAlia 的 `MaterialOverrideComponent`。
- 每段一次 draw：`e.stats` 的 `draws` 是实际的 draw 次数（`architecture.md` §6.2 的预算按它算）。

## 3. 世界

常驻的物体：创建一次，以后只改变化的部分，引擎每帧画出来，并在上两个逻辑步之间插值。C++ 是 L5 的 `src/world/`（EnTT），组件和 StellarAlia 的 `Components.hpp` 同名：`TagComponent`、`HierarchyComponent`、`TransformComponent`（位置、四元数旋转、缩放）、`PrevTransformComponent`、`MeshRendererComponent`、`CameraComponent`。

```python
w = e.world
rock = w.spawn(meshes["rock_a"], position=(x, y, z), yaw=1.2, scale=2.0, tint=0)   # 常驻，直到 destroy()
# 全部参数和默认值：spawn(mesh=0, name="", parent=None, position=(0, 0, 0), yaw=0, pitch=0, roll=0, scale=1,
#                        tint=0, fade=0, light=ae.Light.MATERIAL, ground_fog=True, visible=True, active=True)
base = w.spawn(name="base")                                     # 没有网格：空物体，当父节点
arm = w.spawn(meshes["arm"], parent=base, position=(0, 1.3, 0)) # 位置、旋转、缩放都相对父节点

rock.position = (x, y, z)            # 属性：position、rotation（四元数 x y z w）、yaw / pitch / roll、
rock.yaw += 0.1                      #       set_rotation(yaw, pitch, roll)、scale（数或 (x, y, z)）、
rock.visible, rock.tint, rock.fade = False, 26, 0.5   # mesh、visible、tint、fade、light、ground_fog、name、parent
rock.world_position                  # 当前的世界位置（不插值）
rock.teleport()                      # 这一步不从上一步滑过去（瞬移以后调用）
rock.destroy()                       # 连同子物体；之后再用会报错（rock.alive 是 False）

cam = w.camera                       # 相机也是物体（可以有父节点）；用过一次以后就由它决定画面
cam.position, cam.yaw, cam.pitch = (0, 2, -8), 0.0, -0.1    # pitch 向上为正（和 e.frame 一样）
cam.fov_x, cam.near, cam.far = 1.3, 0.5, 120
w.environment                        # 天空、雾、灯、叠色：就是 e.frame（相机字段跟着 w.camera）
w.set_transforms(nodes, data)        # 批量：每个物体 10 个 float32（位置 3、旋转 4、缩放 3）
w.count, w.clear()
```

- **旋转**：`yaw` 绕 +Y、`pitch` 绕 +X、`roll` 绕 +Z，按 Ry · Rx · Rz 合成（和 Megalith 的物体一样：物体的 `pitch` 为正时 +Z 那头朝下）；相机的 `pitch` 为正是抬头。
- **插值**：每个逻辑步开始前，引擎记下所有物体的局部变换；显示时在"上一步之前"和"这一步之后"之间按 `alpha` 插值（位置、缩放线性，旋转球面插值），再按层级合成，所以子物体跟着父物体走、不会插歪。画面因此比逻辑晚不到一步。刚 `spawn` 的、`teleport()` 过的不插值。无窗口（模拟时钟）和 `--no-interpolate` 时 `alpha` 是 1，直接画当前状态。
- **只在 `fixed_update` 里改物体**：插值的前提是物体在逻辑步里移动；在 `render` 里改的物体要 `teleport()`，否则会被当成"上一步到这一步"的运动。
- **和 `e.draw()` 一起用**：世界在 `end_frame()` 时加进同一份绘制列表；`draw()` 画的东西不插值。同一个场景用世界画和用 `draw()` 画，截图逐像素相同（`tests/test_python.py`）。
- 每个物体的属性每次读写都跨一次 Python → C++（约 1–3 µs）；一帧改几百个用 `set_transforms`。

### 3.1 激活和渲染开关

两层，和 Unity 的 `activeSelf` / `activeInHierarchy` 一样：自己的开关 `active`，和"自己和祖先都开着"的 `active_in_hierarchy`；只管画不画的是另一个开关 `visible`。

```python
node.active = False         # 它和所有子物体离开世界：不画；Animator 不求值、不推进、不发事件；碰撞体不参与查询
node.active_in_hierarchy    # 自己和每个祖先都 active 才算
node.visible = False        # 只是不画这一个物体：不继承，碰撞、动画照常（"看不见但还在"）
node.fade = 1.0             # 完全淡进雾里：不画（fade 是朝雾色混合，0..1）
w.spawn(..., active=False)  # 生成时就关着（对象池）
```

- 关掉父物体**不改**子物体自己的 `active`；父物体再打开时，子物体回到它们自己的状态（打碎的部件仍然是关的）。
- 实际状态在每帧合成矩阵的那次遍历里顺便算出来（自己的 && 父物体的），查询时（碰撞、Animator）沿父节点往上看，树不深，代价可以忽略。
- 一个物体从"实际不在"变成"实际在"的那一步不插值（自动 `teleport()`），不会从关掉前的位置滑过来。
- `set_subtree` 批量改渲染属性（fade、tint、light、ground_fog）；藏整棵树用 `active`，不用 `set_subtree(visible=False)`（它会冲掉子物体各自的 `visible`）。

### 3.2 挂载：换父节点时保持世界变换

挂到挂点上、保持世界位置脱下来（Unity `SetParent(p, worldPositionStays)`，Unreal 的 KeepWorld 规则，Godot `reparent(keep_global_transform)`），对任何物体都能用，不只是 rig（Megalith 的骑手骑在马鞍上、钩锁拉走、落回马鞍）：

```python
rider.set_parent(horse.attach_point("saddle"))                     # 保持局部变换（和 node.parent = … 一样）
rider.set_parent(None, keep_world=True)                            # 脱下来：原地不动，挂到世界根
rider.set_parent(horse.attach_point("saddle"), keep_world=True)    # 挂上去：原地不动，以后跟着挂点走
rider.position, rider.rotation = (0, 0, 0), (0, 0, 0, 1)           # 要"吸附"到挂点就自己清零（落回马鞍）
rigs["pc"].attach_position("hand", anim, phase)                    # 不生成物体：某个姿态下挂点在 rig 空间的位置
```

- `keep_world`：新的局部变换 = 新父节点世界矩阵的逆 × 原来的世界矩阵，再分解成位置、旋转、缩放。旋转和统一缩放是精确的；带旋转的非均匀缩放父节点会产生切变，分解不出来，只能近似（Unity 也一样）。
- 上一步的变换也按同样的方法换算到新父节点下，所以换父节点那一步照常插值，不会顿一下。
- 不能挂到自己的子孙下面（报错）。

## 4. 动画

刚体 rig：骨骼（父子层级、相对父骨骼的偏移、网格）、每块骨骼的关键帧欧拉角、根骨骼的上下起伏、部件槽、挂点、动画事件。姿态在 C++ 里算（`src/world/rig.cpp`），算法和 Megalith 的 `rig.py` 完全一样（三个 rig、所有动画对照，差在 10⁻⁶ 以内）。

```python
e.load_pack("gen/meshes.apak")                 # 先加载 rig 用到的网格（按名字找）
rigs = e.load_rigs("gen/rigs.apak")             # 或者 e.load_rigs(PackWriter().bytes())：代码里生成的 rig
boss = w.spawn_rig(rigs["moose"], position=(0, 0, 0), yaw=0.0)   # 根物体；每块骨骼、每个部件槽是子物体

boss.animator.play("walk", speed=64)           # 引擎自己推进：每秒多少相位单位（循环 0..256）
boss.animator.anim, boss.animator.phase = 2, 128   # 或者游戏每步自己设（Megalith 的用法）
for ev in boss.animator.fired: ...             # 这一步跨过的事件：{"type", "phase", "anim", 事件自己的键}

boss.bone("head")                              # 骨骼的物体（名字或下标）：可以改 mesh、tint、visible
boss.slot(3).visible = False                   # 部件槽的物体
boss.slot_variant(3, 1)                        # 装上 rig 自己的第 1 个变体（按网格名字）
boss.attach_point("hand").world_position       # 挂点：一个物体，可以读位置，也可以当父节点
boss.set_subtree(fade=0.5, light=ae.Light.CHARACTER)   # 整个 rig 一起改（任何物体都可以用）
rigs["moose"].pose("walk", 128)                # rig 空间的每块骨骼变换（测试，或者游戏暂时自己算碰撞体）
```

- 相位：循环动画 0..256（自动回绕），不循环的 0..255（停在最后一帧），和 Megalith 一样。
- 动画在逻辑步里求值（运行时在 `fixed_update` 之后），骨骼是普通的物体，所以**每块骨骼的局部旋转**都插值，比插值合成后的矩阵更自然。不经过运行时（工具、`init` 里）改了相位，画的时候也会重新求值。
- 读 `world_position`（例如挂点）之前，改过相位或动画的 rig 会先重新求姿态，所以同一步里"设相位、读手的位置"读到的是新姿态。
- 事件在跨过它的那一步出现在 `animator.fired` 里（`(上次的相位, 这次的相位]`，循环时回绕）；换了动画时从开头算。
- rig 在资源包里是类型 3（`formats.md`），`tools/alia_pack.py` 的 `add_rig()` 接受 Megalith rig JSON 的结构。

### 4.1 刚体 IK（两种解析约束）

不做通用 IK 系统（没有 FABRIK、全身求解）。刚体 rig 没有蒙皮，骨骼长度就是子骨骼的偏移，两种解析约束覆盖掌机上几乎所有需要：

```python
look = boss.look_at("head", player_node, axis=(0, 0, 1), max_angle=math.radians(60), weight=1.0)
reach = walker.two_bone("arm_r", "forearm_r", "hand_r", ball, pole=(0, 0, -1), weight=1.0)
look.weight = 0.5; look.target = (x, y, z); look.enabled = False; look.max_angle; reach.pole; look.remove()
```

- **look_at**：在动画姿态之上，把骨骼的 `axis`（骨骼空间）按最短弧转向目标（所以滚转保持动画的），超过 `max_angle` 就停在边界；`weight` 在动画姿态和约束结果之间插值。
- **two_bone**：上段、下段（必须是上段的子骨骼）两块骨骼，`end` 是末端：下段上的挂点、挂在下段上的骨骼，或者下段坐标系里的一个点。余弦定理求关节位置，`pole`（rig 坐标系）定关节往哪边弯；够不到就伸直指向目标。
- 在逻辑步里、动画求姿态之后、合成矩阵之前，按创建顺序依次应用；结果写进骨骼的局部旋转，所以和动画一样插值。目标是物体时每步自动跟随，是点时游戏每步设。
- 引擎每步在动画求姿态之后解一次；同一步里目标再动，要到下一步才跟上。
- sandbox 的小人是它的展台（头看相机、右手够一个上下浮动的球，权重来回变）。
- **头抖的三个原因，都在游戏怎么用它**（示例游戏踩过，2026-10-30）：look_at 每步把骨骼直接转向目标，所以目标怎么动，头就怎么动。① 目标在几个之间来回换（"最近的那个"，两个差不多远时每步换一次）：选目标要有回差（离开的距离比进入的远、换成另一个要近得多）。② 平滑目标点的位置：目标绕着头转的时候，平滑的点会从中间横穿过去、穿过头本身，方向一两步就翻过去；要平滑的是方向。③ 目标到了身后：最短弧在 180° 附近翻边，加了 `max_angle` 时头从一边的极限跳到另一边的极限。`alia_kit.Gaze`（§7.1）把 ② ③ 做好了：按方向平滑、限转速，并且不越过身体朝向两边的限角；游戏自己只要做 ①（示例的 `Game._watched`）。

## 5. 碰撞

球和胶囊挂在物体上（物体的局部坐标），跟着物体当前的世界变换走（不插值；半径乘物体最大的轴向缩放）。射线和球、胶囊的求交算法和 Megalith 的 `collide.py` 一样。

```python
node.collider = ae.sphere((0, 0, 0), 1.0, layers=1, user=7)        # sphere(centre, r, layers=1, user=0, priority=0)
node.collider = ae.capsule((0, 0, 0), (0, 2, 0), 0.5, layers=1)     # capsule(a, b, r, layers=1, user=0, priority=0)；None 去掉
node.collider_enabled = False                                       # 暂时关掉
hit = w.ray_cast(origin, direction, max_distance=80, layers=0b101, exclude=[node])
hit.node, hit.distance, hit.point, hit.layers, hit.user            # 没打中是 None
w.overlap_sphere(centre, r, layers)                                 # [Hit]（distance：球心到表面，在里面是 0）
w.colliders(layers)                                                 # 世界空间的形状（瞄准辅助之类自己算的东西）
w.set_ground(heights, n, cell, (origin_x, origin_z))                # 高度图：n x n，z 方向一行一行
w.ground_height(x, z), w.ray_ground(origin, direction, max_distance)
rig_node.colliders("neck")                                          # rig 骨骼上的碰撞体（物体）
rig_node.slot_variant(3, 1)                                         # 部件槽的碰撞体：装上变体就开，变体 0 就关
```

- **`set_ground` 只是数据**：给 `ground_height`、`ray_ground`、粒子落地、`kit.GroundMover` 用，不画东西；地面的网格要自己建（§0 的例子、示例游戏的 `field.py`）。
- `layers` 是位掩码：查询的掩码和碰撞体的掩码有共同的位才算。`user` 是给游戏的整数（Megalith 用它放部件的槽号）。`priority`：两个命中相差 1 mm 以内时，优先级高的赢（腿上的装甲挡在腿前面）。
- 射线从形状内部出发不算命中它。`exclude` 里的物体和它们的子物体上的碰撞体都不算。
- rig 的碰撞体写在资源包里（骨骼上的列表、部件槽上的一个），`spawn_rig` 自动生成：骨骼的碰撞体是骨骼的子物体，部件槽的碰撞体在槽物体上，默认关闭。
- 读之前会先给改过相位的 rig 求姿态（和 `world_position` 一样），所以同一步里"设相位、打射线"打的是新姿态。

## 6. 粒子

模拟在 C++ 里做（固定步长），同一个模板的粒子一次 instanced draw 画完；有玩法意义的东西（会伤人的尘环、要判定的子弹）仍然是游戏逻辑里的物体，纯视觉的（火花、碎片、尘土、拖尾）用粒子。模块参考 Unity 的粒子系统（Shuriken），只取对低多边形掌机有用的部分：点、球、半球、锥、圆环五种形状；寿命、速度、大小、旋转的范围；重力、阻力；大小、淡出、颜色随寿命变化；落地停住、弹起或消失；本地或世界空间。

```python
sparks = e.particles(meshes["spark"], lifetime=(0.2, 0.3), speed=(2, 5), size=1.0, gravity=1.0,
                     shape="cone", angle=0.6,               # point / sphere / hemisphere / cone / ring（radius=）
                     size_over_life=[(0, 1.0), (1, 3.0)], fade_over_life=[(0.7, 0), (1, 1)],
                     tint=0, tint_over_life=[(0.5, 9)], spin=(-6, 6), rotation=(0, 6.3), drag=0.0,
                     velocity=(0, 2, 0), ground="bounce", bounce=0.4,   # through / stop / bounce / die
                     max_particles=256, local=False, light=ae.Light.MATERIAL, ground_fog=True)   # 模板
fx = w.spawn_emitter(sparks, parent=boss.attach_point("brow"), rate=0, seed=1)   # 发射器是物体
fx.burst(24)                                                  # 现在发 24 个
fx.burst(6, at=(x, y, z), tint=palette.STONE)                 # 从某个点、换个颜色（碎片跟着打碎的部件）
fx.rate = 40                                                  # 每秒
fx.time_scale = 0.25                                          # 慢动作；0 停住（打击停顿）
fx.count, fx.clear(), fx.active = False                       # 关掉：不模拟也不画
```

- **默认值**：`particles(mesh, lifetime=1, speed=0, velocity=(0, 0, 0), size=1, rotation=0, spin=0, gravity=0, drag=0, max_particles=64, local=False, shape="point", radius=0, angle=0.5, size_over_life=None, fade_over_life=None, tint=0, tint_over_life=None, ground="through", bounce=0.4, light=MATERIAL, ground_fog=True)`；`spawn_emitter(particles, parent=None, position=(0, 0, 0), yaw=0, pitch=0, rate=0, seed=0)`。上面例子里的值不是默认值。
- **单位**：`lifetime` 秒；`speed`、`velocity` 米/秒（`velocity` 在发射器的坐标系里）；`gravity` 是 9.8 m/s² 的倍数（1 就是地球重力）；`drag` 每秒的减速比例；`size` 乘在网格本身的大小上（单位球的网格 `size=0.1` 就是 10 cm）；`spin`、`rotation`、`angle` 弧度；`fade` 0 是原色、1 是完全融进雾色（不画）；`tint` 是调色板索引，0 是"不换色"。
- 范围参数是一个数或 `(lo, hi)`；曲线是 `[(t, 值)]`，t 是寿命的 0..1，分段线性、两头保持；`tint_over_life` 是阶梯（调色板索引不插值）。
- 每步在动画之后模拟（`World::update_particles`），粒子记着上一步的位置，画的时候和别的物体一样插值。满了（`max_particles`）就挤掉最老的：新的效果更要紧（Megalith 原来的 `MAX_SPARKS` 也是这样）。
- 粒子画成普通的网格实例，同一个模板的粒子被渲染器合成一次 instanced draw（`architecture.md` §6.2）。`fade` 到 1 的不画。
- 落地只对世界空间的粒子（`local=False`）生效，用 §5 的高度图。

- **确定性**：每个发射器有自己的随机种子，模拟在固定步长里做，所以无窗口截图和测试可以复现。
- **预算**：板上实测见 `architecture.md` §6.2（1000–2000 个网格粒子约 1–2 ms）。

## 7. 系统：输入动作、声音、存档、语言、振动

- **输入动作映射**（已实现，`src/runtime/actions.hpp`）：游戏读"它要什么"，不读按键；按哪个键是一张表，运行时能改（改键菜单），能存。

  ```python
  act = e.actions
  act.bind("shoot", ae.SOUTH)                     # 按钮动作：这些键里任何一个（ae.SOUTH 或 "SOUTH"）
  act.bind("hook", ae.NORTH, ae.R1)
  act.bind_axis("move", "left", dead_zone=0.15)   # 轴：左摇杆、右摇杆或十字键（"dpad"），径向死区，过了死区重新拉满到 1
  act.held("shoot"), act.pressed("shoot")         # pressed：这一步按下的（比一步还短的点按也算）
  act.axis("move")                                # (x, y)，-1..1，+x 右，+y 下（和 Input 一样）
  table = act.bindings()                          # {"shoot": ["SOUTH"], "move": {"stick": "left", "dead_zone": 0.15}}
  act.load(table)                                 # 整张换掉，比如从存档读回来的；act.unbind(name)、act.clear()
  ```
  - 再 `bind` 同一个名字就是替换。按钮是位置的（South、East……）：Alia Pad 的标签是任天堂式的（A = East，B = South）。
  - 以后可以从资源包读表（StellarAlia 的 `.sainputmap`）；现在游戏在 `init` 里 `load` 一个 dict 就行（Megalith 的 `main.py`）。
- **声音**（已实现，`src/audio/`；方案 2026-10-30 用户确认）：

  ```python
  au = e.audio
  hit = au.load("sfx/hit.wav")                    # 16 位 PCM 的 WAV（路径相对卡带的 content/，或 bytes），单声道或立体声，任意采样率
  v = au.play(hit, volume=0.8, pan=-0.3, pitch=1.1)   # -> Voice；loop=True、loop_start=秒、end=秒、fade_in=秒、bus="effects"
  v.playing, v.stop(fade=0.2), v.set_volume(0.5, fade=0.1), v.set_pan(0.4)
  au.play_music(theme, volume=0.6, loop=True, loop_start=19.2, end=153.6, fade=0.4)   # 每步都调也行（见下）
  au.play_music(None)                             # 安静；au.stop_music(fade)
  au.music_volume, au.sfx_volume = 0.8, 1.0       # 两条总线（设置菜单的两个滑条）
  au.voices, au.device                            # 正在响的声音数；设备给了什么（None：没有设备）
  au.mix(frames)                                  # 无头：手动渲染接下来的 frames 帧（48 kHz 立体声 float32 的 bytes）
  ```
  - 声音保持自己的采样率和声道（22 kHz 单声道的两分钟音乐约 5 MB），混音时线性插值重采样到 48 kHz。32 个声音槽，满了就让最小声的音效让位（音乐不让）。音量、淡入淡出都是渐变，不会爆音；声像对单声道是等功率（居中两边各 0.707），对立体声是平衡。
  - 音乐只有一条：同一个声音再调只改音量（按 `fade` 渐变），换一个就交叉淡化；放完的一次性曲子（胜负的短曲）在还被要求的时候保持安静，不会重放。
  - 设备只在窗口模式打开：无头的测试、工具、adb 上的跑分都不出声，可以 `mix()`。`alia-stats` 多了 `voices` 和 `audio_late`（回调比上一次晚了 1.5 个缓冲以上的次数：很可能断音，`stats-protocol.md`）。
  - 和系统侧的约定见 Alia Pad 的卡带文档（devkit）里"声音"一节：
  - 后端 SDL3 audio：一个 48 kHz 立体声的 `SDL_AudioStream`，所有声音（流式音乐 + 载入的音效）在引擎里用 C++ 混好再送进去；不直接用系统的音频库。
  - 申请 512 帧的 quantum（约 10.7 ms），音效目标延迟 20–30 ms。混音在 SDL 的音频线程里，普通优先级；板上满负载实测没有断音（`architecture.md` §6.2），不需要实时优先级。
  - 只有播放、只有立体声声像；不录音、不枚举设备（喇叭和耳机的切换游戏察觉不到）。
  - 游戏看不到也改不了系统音量和静音；引擎只提供混音内部的比例。**音乐和音效分开**（2026-10-30 定）：两条总线各有音量（`e.audio.music_volume`、`e.audio.sfx_volume`），游戏可以做成设置菜单里的两个滑条，存进存档。
  - 挂起恢复后接着放缓冲里剩下的几十毫秒，不清空，不通知游戏（§9 第 6 条）。
  - 系统的通知音播放时，系统把游戏这条流的音量渐变压低（现在约 −8 dB），播完渐变恢复；调音量的"嘀"这类短反馈音直接叠加，不压低。全是系统做的，游戏和引擎什么都不用做。具体数字以 Alia Pad 的卡带文档为准。
- **存档**（已实现，`src/runtime/saves.hpp`）：就是 Alia Pad 卡带 SDK 的 `save.h`（MIT，复制在 `third_party/alia-sdk/`），和 SDK 的 Python 版 `alia.save` 读写同样的文件（`tests/test_python.py` 对照过）。

  ```python
  ae.save.write("slot1", data)        # 原子写入；前两个版本留着
  ae.save.read("slot1")               # 最新的可读版本；没有是 None；read("slot1", 1) 读上一个版本
  ae.save.names(), ae.save.versions("slot1"), ae.save.rollback("slot1", 1), ae.save.delete("slot1")
  ae.save.dir(), ae.save.set_dir(path)  # 工具、测试
  ```
  - 在卡带里写进 `ALIA_SAVE_DIR`（受卡带配额 `ALIA_SAVE_QUOTA_KB` 限制，满了 `OSError` ENOSPC）；别的地方（PC、模拟器、测试）引擎打开时用 `~/.local/share/alia-engine/saves/<app id>`，所以游戏在哪儿都一样存档。**app id 在卡带外面是 `ae.run(app_id=…)` 给的**，不给就是 `pad.alia.engine`，和别的没给的游戏共用一个目录。
  - `write(name, data)` 的 `data` 是 **bytes**（JSON 先 `json.dumps(...).encode()`）；`set_dir(path)` 会把目录建出来。名字 `[A-Za-z0-9][A-Za-z0-9._-]{0,63}`，不合法的 `OSError` EINVAL；出错的消息里写着存档目录。
- **语言**（已实现，`src/runtime/locale.hpp`）：系统语言，到处同一个办法拿。

  ```python
  ae.language()                                   # BCP 47："zh-CN"、"en"……卡带里是 ALIA_LANG（系统的设置），别处是 POSIX 的区域设置
  ae.choose_language(["en", "zh"])                # 游戏有的语言里用哪个：完全相同的标签 → 只看语言（zh-CN → zh、zh-Hans）→ 英语 → 第一个
  ae.localized({"en": "PRESS START", "zh": "按开始"})   # 直接取文字
  ```
  - `ALIA_LANG` 是启动时读的：游戏运行时用户改了语言，下次启动才生效（Alia Pad 的卡带文档）。卡带的标题和简介的翻译写在 `cart.toml` 的 `[locale.<标签>]` 里，系统自己挑，游戏不用管；引擎读清单时也按同样的规则挑窗口标题（`Manifest::title_for`，旧写法 `title_zh` 算 `[locale.zh]`）。引擎自己的卡带（sandbox、E0）用的是 `[locale.zh]`。
- **振动**（已实现）：`e.rumble(low, high, ms)` 就是 `SDL_RumbleGamepad`。**马达硬件到货之前一直不支持**（没有"只记日志"的阶段）：`SDL_RumbleGamepad` 返回不支持，`e.rumble` 返回 False，不报错，游戏照常调用就行。

### 7.1 alia_kit：游戏常用的小件

`python/alia_kit.py`，纯 Python，跟着引擎模块装进卡带（`content/python/`），`import alia_kit as kit`。都是游戏持有、每步调用的东西，**没有要继承的基类**（§9 第 5、7 条）；收进来的是 sandbox、示例游戏、Megalith 各自写过的那些：相机、角色控制器、HUD 文字、对象池，还有现成的形状、地面和光，常用的数值、角度、向量计算和噪声。

```python
cam = kit.FollowCamera(w.camera, distance=6.5, height=3.0, shoulder=0.9, pitch=-0.22, turn_speed=2.2, recenter=0.0)
cam.turn(act.axis("turn")[0], dt); cam.follow(w, mover.position, mover.yaw, dt); origin, d = cam.ray(lift=0.1)
fly = kit.FlyCamera(w.camera, level=True); fly.update(move, look, rise, dt, floor=0.3)       # 自由相机（sandbox）
fps = kit.FirstPersonCamera(w.camera, eye_height=1.6); fps.update(position, look, dt)       # 第一人称
mover = kit.GroundMover(w, position=(x, z), radius=0.4, speed=4.5, layers=SOLID, bounds=(x0, z0, x1, z1))
mover.move(kit.stick_to_world(act.axis("move"), cam.yaw), dt)   # 走在地面上，碰到 layers 上的碰撞体就推出来
mover.position, mover.yaw, mover.moving                         # moving：这一步用了多少速度（0..1，给走路动画）
gaze = kit.Gaze(rate=5, speed=4); look.target = gaze.update(head_pos, target, dt, forward=body_yaw, limit=1.2)
kit.Smoothed(start, rate).update(target, dt)                    # 平滑一个点（相机的目标、标记）
kit.world_to_screen(e, point)                                   # HUD 像素，身后是 None
kit.text_width(s, scale), kit.say(e, x, y, s), kit.centred(e, y, s, color, scale)
pool = kit.Pool(make, on_get=..., on_release=...); pool.get(), pool.release(obj)
```

现成的形状、地面和光（返回 `ae.Mesh` 的交给 `e.create_mesh`，一份网格可以 spawn 很多次）：

```python
kit.box(sx, sy, sz, colour, centre=(0, 0, 0))             # 箱子；colour 一个索引，或 6 个（-Z、+Z、-Y、+Y、+X、-X 面）
kit.sphere(radius, colour, rings=6, segments=10)          # 低多边形球，100 个三角形
kit.cylinder(radius, height, colour, segments=10, cap=None)  # 立着的圆柱，y 从 0 到 height（柱子、树干）
kit.plane(width, depth, colour, divisions=1, checker=None)   # y=0 的平地，checker 给了就是棋盘格
height = kit.hills(seed=1, amplitude=2.0, scale=0.06, octaves=3)   # 函数 (x, z) -> y：起伏的山坡，同一个 seed 一样
t = kit.Terrain(e, size=64, cell=4, height=height, colours=(21, 20), centre=(0, 0))  # 画地面 + set_ground
kit.environment(e, "dusk")                                # day / dusk / night / overcast：天空、雾、太阳、角色光
```

数值、角度、向量和噪声（向量就是 tuple：引擎收的、给的都是 tuple，所以没有 Vec3 类）：

```python
kit.clamp(v, lo=0, hi=1), kit.lerp(a, b, t), kit.inverse_lerp(a, b, v), kit.remap(v, a, b, c, d), kit.smoothstep(a, b, v)
kit.approach(v, target, step)          # 匀速靠近（step = 速度 × dt），不会冲过头
kit.damp(v, target, rate, dt)          # 指数靠近，和步长无关（数字版的 Smoothed）
kit.wrap_angle(a)                      # 换到 -π..π
kit.angle_between(a, b)                # 从 a 转到 b 最近的那一边，带符号（正是向左，yaw 增大）
kit.turn_towards(yaw, target, step)    # 每次最多转 step
kit.yaw_to(origin, target)             # 站在 origin 面朝 target 的 yaw；(x, z) 和 (x, y, z) 都行
kit.forward(yaw, pitch=0), kit.right(yaw)   # 相机看的方向（单位向量，aim(d, camera=True) 的反过来）；地面上的屏幕右方
kit.aim(direction)                     # (yaw, pitch)：node.set_rotation(*kit.aim(d), 0) 让 +Z 指向 d；相机用 aim(d, camera=True)
kit.add, kit.sub, kit.scale, kit.dot, kit.cross, kit.length, kit.normalize, kit.distance, kit.distance_xz, kit.lerp3
kit.noise(x, z=0, seed=0)              # 平滑的值噪声，-1..1，大约每单位一个起伏；只给 x 就是一维（抖动、闪烁、飘）
kit.fbm(x, z=0, seed=0, octaves=3)     # 几层噪声叠起来，还在 -1..1；hills() 就是它
kit.WHITE, kit.BLACK, kit.SHADOW       # HUD 颜色（0xRRGGBBAA）
kit.pal.RED, kit.pal.GRASS, ...        # 默认调色板的索引（网格、tint、粒子），名字见下面
```

- `aim` 区分物体和相机，因为两者的 pitch 方向相反（§0.1）：物体的 pitch 为正时 +Z 朝下，相机的 pitch 为正是抬头。
- `kit.pal` 只对默认调色板有意义：`set_palette` 换的是颜色，名字还是这些编号。名字按编号（`src/render/handheld.hpp` 的 `kDefaultPalette`）：`BLACK WHITE LIGHT_GREY GREY DARK_GREY CHARCOAL`（0–5）、`RED ORANGE YELLOW LIME GREEN TEAL`（6–11）、`BLUE NAVY VIOLET MAGENTA PINK BROWN`（12–17）、`TAN CREAM GRASS FOREST STONE SLATE`（18–23）、`SKY DUSK_BLUE GLOW_YELLOW CORAL GLOW_CYAN GLOW_LIME`（24–29）、`GLOW_PINK NEAR_WHITE`（30–31）。
- 随机数用 Python 的 `random.Random(seed)`，kit 不另外包。

- 形状都可以给 `material=`（一份 `create_material` 的 id）：整个网格用这个材质，颜色是材质的 `palette_index`。默认材质是平直着色（掌机的棱角感）；要圆滑的球给 `Shading.SMOOTH`，发光的给 `UNLIT` 或自发光（§2）。
- `box` 的 `centre` 决定原点在哪：`centre=(0, h / 2, 0)` 原点在底面（放在地上），`(0, -l / 2, 0)` 原点在顶上（四肢挂在关节上）。
- `Terrain` 的 `colours` 是 (低处, 高处) 两个索引，按中间高度分；或者一个函数 `(x, y, z) -> 索引`，每格一次。`t.node` 是它的物体，`t.height(x, z)` 就是 `world.ground_height`。`height` 可以是任何函数：`kit.hills`、自己写的，或者 `lambda x, z: 0.0`（平地，也是默认）。
- `environment` 只改 `e.frame` 里的这些：`sky_top`、`sky_horizon`、`fog_color`、`fog_near`、`fog_full`、`ground_fog`、`sun.travel`/`ambient`/`diffuse`、`character.ambient`/`diffuse`；之后再改哪个字段都行。预设的值在 `kit.ENVIRONMENTS`。**引擎没有点光源**：只有太阳和角色光两盏方向光；灯笼、火把这类用自发光材质，看起来亮（§2）。

- 全部参数：`FollowCamera(camera, distance=6.5, height=3.0, shoulder=0.9, pitch=-0.22, turn_speed=2.2, clearance=1.0, recenter=0.0, recenter_after=0.8)`（`recenter`：松开摇杆 `recenter_after` 秒后，每秒转回目标朝向的比例，0 不转回；`.yaw` 是它现在的朝向）；`GroundMover(world, position=(x, z), radius=0.4, height=1.8, speed=4.5, turn_rate=10.0, layers=1, bounds=None)`：**给的位置是地面上的 (x, z)，读回来的 `.position` 是 (x, y, z)**（y 贴着地），`.yaw` 是它面朝的方向，`.moving` 是这一步用了多少速度（0..1）；`Gaze(rate=6.0, speed=4.0, reach=10.0)`；`FlyCamera(camera, speed=8.0, look_speed=1.6, min_pitch=-1.4, max_pitch=1.4, level=False)`；`FirstPersonCamera(camera, eye_height=1.6, look_speed=2.0, min_pitch=-1.2, max_pitch=1.2)`。
- `GroundMover` 是运动学的角色控制器（没有物理）：按高度图贴地；球和胶囊投到地面上当圆和线段，够到它的高度才挡（`overlap` 不用游戏自己写）；推两遍，免得推出一个又进了另一个。
- 相机的 `turn()` 是摇杆的 x：往右推向右转。`stick_to_world()` 让摇杆向上就是"离开相机往前走"。
- 测试在 `tests/test_kit.py`。玩法特有的东西（状态机、AI、武器）不放这里：每个游戏不一样。

## 8. 还没有的（以后）

- **纹理**：材质里留了 `color = texture` 和 `alpha_mode = cutout` 的位置；ETC2、图集等 2D（E7）或者有游戏要用时再做（§9 第 3 条）。
- **公告板粒子、光晕粒子**：老掌机（PS1、N64、PSP）的做法是**加法混合的公告板**：朝相机的小方块，贴一张中心亮、边缘透明的径向渐变，颜色直接加在背后的像素上。比半透明的壳便宜得多：两个三角形；加法和顺序无关，所以不用排序、一个模板仍然一次 instanced draw；开销只在填充率（估计 1000 个、每个约 20×20 像素，约 1.7 个全屏层，板上约 0.6 ms）。要做的三件事：材质加 `AlphaMode.ADD`（加法的部分也走实例化、不排序）；粒子系统里让方块朝相机；软边的贴图（等纹理），或者不用纹理的替代：中心亮、边缘为 0 的小圆盘，靠顶点颜色带一个亮度。现在的"发光"只是材质的 emissive + 不受雾影响（sandbox 的发光球）。
- **粒子的其他部分**：子发射器（死亡、落地时触发另一个发射器）、从资源包读模板、粒子和碰撞体的碰撞。
- **字体**：HUD 只有一套内置的 5×7 像素字体，只有 ASCII。要显示中文，得先做字体：资源包里的位图字体（一张字形图集 + 字符表），打包时只烘焙游戏用到的汉字（掌机上整套 CJK 太大），像素风的 CJK 字体（比如 12 px，要挑许可合适的）。和 E7 的 2D 一起做（`architecture.md` §6.3）。
- **通用模板**：rig 是现在唯一的模板（§9 第 7 条）；一般的物体树模板（资源包里或者代码拼出来，网格、材质、碰撞体、发射器）以后做。
- **插件**：有了（`docs/plugins.md`），第一个是补间 `alia_tween`。还没有的是插件的索引和在线安装（插件多起来再说）、C++ 插件（要先有稳定的 C++ 接口）。
- **从资源包读的输入表**（StellarAlia 的 `.sainputmap`）、**流式的长音乐**（现在整首载入）。

## 9. 决定（2026-10-29 起）

1. 世界接口用"物体 + 属性"（§3），不用 Unity 式的 `GameObject` + `transform`。
2. E4 的顺序：1 资源 ✅ → 2 材质 → 3 世界 → 4 动画 → 5 碰撞 → 6 粒子（同时给 L2 加实例化）→ 7 系统 → 8 收尾（Megalith 完全迁到场景 API、3D 示例卡带、API 文档）。材质在世界之前，世界的 `MeshRenderer` 就不用改两次。
3. 纹理：材质里先留 `color = texture` 和 `alpha_mode = cutout` 的位置，真正的纹理（ETC2、图集）等 2D（E7）或者有游戏要用时再做；公告板粒子也等它。
4. 在途帧和每帧动态数据的环形缓冲：现在交给驱动（GLES 隐式同步，结果正确），**出现性能瓶颈时再做**（2–3 帧深的环形缓冲加 fence）；将来的 Vulkan 后端在后端内部实现在途帧，游戏和上层不用改。
5. **不做"游戏对象"层**（2026-10-29，方案 B）：引擎只提供积木（`Node`，以后的 Animator、Collider、发射器），游戏用自己的类组织逻辑（例如 Megalith 的 `Player`、`Boss` 持有物体，由 `Game` 统一调度）。不做 Unity 式的"物体上挂脚本、引擎逐个回调 `update()`"：板上 Python 是瓶颈（`architecture.md` §6.2），每个物体每步一次回调撑不住；也不另做一层可选的 `Actor` / `Scene`。示例卡带和文档会演示推荐的组织方式。
6. **游戏不响应挂起**（2026-10-29）：和其他掌机一样，挂起时系统冻结卡带（SIGSTOP），恢复后系统在冻结的画面上盖"已暂停 · 按任意键继续"。卡带清单用默认的 `pause = "system"`，引擎不装 SIGUSR1/2 的处理，也没有 `Game.suspend()/resume()`。运行时只保证恢复后不追帧：两帧之间超过 0.25 s 的时间直接丢掉，游戏从停下的地方继续（`app.hpp`）。游戏自己的暂停菜单（比如 Megalith 的 Start 菜单）照常是游戏的事。
7. **复用靠模板，不靠继承**（2026-10-30）：引擎里没有可以继承的基类（没有 `Actor`、`Component` 让游戏派生），游戏的类也不必为了复用而叠继承层次。复用的单位是**模板**：一份描述（一棵物体树：网格、材质、碰撞体、挂点、动画，以后还有发射器），实例化多少次都行，每次得到一组普通的物体，参数（位置、tint、部件变体……）在实例化时给。rig 就是第一种模板（资源包里一份，`spawn_rig` 一次一个实例，Megalith 的 BOSS 和 sandbox 的小人都是这样来的）。以后把它推广成一般的模板（资源包类型或者代码里拼出来的都行），游戏逻辑仍然是普通的类持有这些实例，靠组合不靠继承。
8. **插件是库，不是钩子**（2026-10-30）：kit 之外的可选系统（补间、状态机、对话……）做成插件（`docs/plugins.md`）：一个 Python 包加 `plugin.toml`，用 `tools/alia_plugin.py add` 放进用到它的卡带，不进别的卡带，也不改 `cart.toml`。引擎不回调插件，插件不往主循环里挂东西；游戏持有插件的对象、每步自己调（和第 5 条一样：每一步在跑什么，游戏看得见）。插件只用 api.md 写了的接口、声明支持的引擎版本并在 import 时检查、纯 Python。

