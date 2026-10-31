# alia-engine

Alia Pad 掌机的游戏引擎：一个轻量的 C++ 运行时，针对 UNO Q 的 Adreno 702（GLES 3.1、内存带宽约 2 GB/s）设计，同一份代码也在 PC 上运行（当作模拟器的渲染端）。

**现状：v0.2.0（2026-10-30），E0–E2、E3a、E4 完成**：Python 写 3D 掌机游戏的整套接口（[`docs/api.md`](docs/api.md)，从 §0 快速开始读起）、`alia_kit` 小件、插件（[`docs/plugins.md`](docs/plugins.md)，第一个是补间）、示例游戏 Wisp Keeper（`apps/sample/`）；游戏项目的布局和 `alia` 工具（运行、测试、打卡带）见 [`docs/project.md`](docs/project.md)；Megalith 是独立的游戏仓库 `NU-Hachimi/megalith`。下一步 E5 / E6；E3b 等屏幕和 IMU 硬件。 架构见 [`docs/architecture.md`](docs/architecture.md)。构建用 CMake，板上版本在 PC 上交叉编译（clang + 板子的 sysroot，§5.1）。

**做游戏从这里开始**：[`docs/getting-started.md`](docs/getting-started.md)（装好引擎、`alia new`、在 PC 上玩、打卡带上板子）。下面是改引擎本身的说明。

## 构建和运行（PC）

依赖：CMake ≥ 3.24、clang（或 gcc）、Ninja、SDL3、EGL、GLES 3（Arch：`sdl3`、`mesa`/`libglvnd`），Python 3.13（和板子同一版本；用 `-DALIA_PYTHON=...` 换，`-DALIA_WITH_PYTHON=OFF` 不编 Python 模块）。第三方库（stb、glm、nanobind）随仓库提供，静态编译。

```bash
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build
ctest --test-dir build --output-on-failure      # 渲染数学 + 黄金截图（无窗口 EGL）

build/alia-e0-demo                              # 窗口，动画测试场景；Esc 退出
build/alia-e0-demo --rotate 90                  # 竖屏输出，和 720x1280 面板上一样
build/alia-e0-demo --headless --out shot.png    # 无窗口，截一帧
build/alia-e0-demo --headless --frames 600      # 无窗口跑 600 帧，打印每帧耗时
build/test_golden --update                      # 改了渲染以后重新生成黄金图（先看 diff 再提交）
build/alia-bench                                # 合成负载的帧耗时表（E2；板上的数字见 docs/architecture.md §6.2）
(cd apps/sandbox && ../../tools/alia.py run)     # sandbox：每个引擎特性一个展台、自由相机、实时统计（视觉检验）
(cd apps/sample && ../../tools/alia.py run)      # 示例游戏 Wisp Keeper（捕光）：推荐的游戏组织方式（apps/sample/README.md）
PYTHONPATH=build python -c "import alia_engine"  # Python 模块（L7），游戏卡带 import 它
cmake --install build --prefix <卡带>/content     # python/alia_engine.abi3.so（稳定 ABI，Python ≥ 3.12）；lib/ 放共享库
```

## 板上运行

板上版本在 PC 上交叉编译：clang + lld + 板子的 sysroot（来自 Alia Pad 的 devkit，用 `ALIA_SYSROOT` 指定；`docs/architecture.md` §5.1）。

```bash
cmake -S . -B build-board -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-board.cmake
cmake --build build-board                         # 约 1–2 s；板上原生编译要 30 s
tools/board-deploy.sh [目标目录] [adb 设备]          # 把测试用的编译结果经 adb 推到板子上，在板上跑全部测试，打印汇总
```

**sandbox**（`apps/sandbox/`，Python，只用公开 API）是引擎的开发卡带 `pad.alia.engine.sandbox`：每个引擎特性一个展台（材质、雾、灯光、HUD；E4 每做完一步就加新的展台），自由相机（左摇杆移动、右摇杆转视角、L1/R1 下降上升），按键切换雾、贴地雾、太阳转动、调色板、受伤叠色，顶上一行实时统计。卡带用 `tools/alia.py cart` 打，用 `alia install` / `alia launch` 装到板子上并启动（`docs/project.md`）。E0 的测试图（`alia-e0-demo`）仍然在，用于黄金截图和基准，不做成卡带。

## E1、E3a：通用的掌机渲染、Python API 和主循环

| 层 | 内容 |
|---|---|
| L1 `platform/input.hpp` | 手柄（SDL3，按位置命名：Alia Pad 上 A = EAST、B = SOUTH、X = NORTH）；PC 上键盘代替手柄（WASD / 方向键 / 空格 / J K L …）。忽略窗口焦点（板上的无头 sway 没有键盘，窗口永远拿不到焦点；谁收手柄由系统决定）。`ALIA_INPUT_LOG=1` 打印映射和按键 |
| L3 `render/handheld.hpp` | 一个场景 pass：天空渐变 → 网格（32 色调色板或逐顶点 RGB；太阳光 / 角色光 / 不打光；淡出到雾色）→ 分级距离雾（N 级色带）和贴地雾 → 全屏叠色 → HUD（矩形和 5×7 字体）。包围球视锥剔除 |
| L4 `asset/pack.hpp` | 资源包 `.apak`（`docs/formats.md`）：PC 上用 `tools/alia_pack.py` 写，`engine.load_meshes(path)` 读，返回 {名字: 网格 id} |
| L6 `runtime/engine.hpp`、`runtime/app.hpp` | `Engine`：窗口或无窗口、设备、管线、最终 pass；每帧一份绘制列表。`run()`：主循环（固定步长、按键每步只算一次、`SIGTERM`）、卡带清单、运行选项、统计 |
| L7 `alia_engine` | Python 模块（稳定 ABI）：`run`、`Engine`、`Mesh`、`Frame`、`Light`、按键常量；几何用 bytes 传（`array('f').tobytes()`） |

```python
import alia_engine as ae

class Game:                                    # alia_engine.run() owns the loop and the display
    def init(self, e):
        self.cube = e.create_mesh(ae.Mesh(positions, indices, tri_palette=colors))
        e.set_palette(rgb_bytes)
    def fixed_update(self, e, dt):             # 60 Hz; e.input.pressed holds each press once
        self.x += e.input.lx * dt
    def render(self, e, alpha):
        f = e.frame; f.eye, f.yaw, f.pitch = (0, 2, -8), 0.0, -0.1
        e.draw(self.cube, (1, 0, 0, self.x,  0, 1, 0, 0,  0, 0, 1, 0), light=ae.Light.SUN)
        e.hud_text(8, 8, "HELLO", 0xFFFFFFFF, scale=2, shadow=0x000000FF)

ae.run(Game(), argv=sys.argv[1:], title="Game")  # --headless --frames N --screenshot P --stats ...
```

内部分辨率来自卡带清单的 `[display] render`，窗口尺寸和节奏来自合成器；运行选项（无窗口、跑 N 帧、截图、统计、输入日志，以及调试用的 `--size` / `--render` / `--rotate`）对所有游戏都一样，见 `src/runtime/app.hpp`。统计行的格式：`docs/stats-protocol.md`。

验证：`test_handheld`（黄金图，NVIDIA / Intel / llvmpipe 一致）；Megalith（基于本引擎的游戏）在 PC 和板上都能跑，画面和原来 MCU 渲染器的截图逐项对得上；板上 Python + 提交约 16 ms/帧（60 Hz）。

## E0 做了什么

| 层 | 内容 |
|---|---|
| L0 `src/core` | 日志；RGBA 图像和 PNG 读写、图像比对 |
| L1 `src/platform` | `Surface`：SDL3 窗口（GLES 3.0 上下文）和无窗口 EGL（surfaceless，不需要显示器） |
| L2 `src/rhi` | 薄 RHI：buffer / texture / render target / pipeline / pass（显式 load/store，按 tile GPU 设计）；GLES 3.0 后端 |
| L3 `src/render` | 最终 pass：640×360 整数倍放大、居中加黑边、0/90/180/270 旋转；E0 测试场景 |

验证：
- 黄金截图在 NVIDIA、Intel（Mesa）、llvmpipe 和**板上的 FD702**（Mesa 26.1，GLES 3.1）上一致，最大差 1 级。
- SDL 窗口：PC 上用户实测正常。
- 板上（无窗口，板上原生编译，当时 glmark2 同时占着 GPU，数字偏保守）：

  | 内部分辨率 → 输出 | 每帧 |
  |---|---|
  | 640×360 → 1280×720 | 2.95 ms |
  | 854×480 → 1280×720 | 3.29 ms |
  | 1280×720 → 1280×720 | 4.26 ms |
  | 640×360 → 720×1280（旋转 90°） | 2.98 ms |

- 内部分辨率可调（`--internal WxH`）；输出是整数倍时最近邻放大，否则锐利双线性（`--scaling`）。
- `-DALIA_WITH_SDL=OFF`：只有无窗口模式，不需要 SDL3（板上基准、CI）。

- 交叉编译（板子的 sysroot）出的版本在板上通过黄金截图，性能和板上原生编译相同；只依赖板上已有的库（libstdc++ 符号最高用到 GLIBCXX_3.4.31，板上有 3.4.33）。
- **板上窗口模式**：作为 Wayland 客户端跑在板上的无头 sway 里，FD702 渲染，用 `grim` 截取板子屏幕输出确认。

## 和其他部分的关系

| 部分 | 关系 |
|---|---|
| Alia Pad 系统 | 掌机的系统部分（不开源）。引擎编译时**不依赖**它；运行时通过标准的 SDL 手柄读输入，读系统给的环境变量，卡带格式见 Alia Pad 的卡带文档（devkit）。系统在沙箱里运行卡带，引擎不管沙箱。详见 `docs/architecture.md` §4 |
| StellarAlia | PC 上的引擎和编辑器。只借用它的非渲染代码（EnTT 场景、输入动作映射、Jolt 物理、BVH）和数据格式，不修改它；掌机导出目标放在后期 |
| Megalith | 基于本引擎的游戏（独立的私有仓库）。第一个里程碑是让它的 Python 玩法代码不改，通过本引擎在 PC 上跑起来；借用了它的 g3d 协议解析、rig 求值和资源构建脚本 |

## 计划

详见 `docs/architecture.md` §6。硬件（屏幕、IMU）到货之前，先做不依赖硬件的部分：E3a → E4 → E5 → E6 → E7。

| 阶段 | 内容 | 状态 |
|---|---|---|
| E0 | 骨架：SDL3 窗口和无窗口 EGL、GLES 后端、最终 pass、黄金截图测试 | ✅ |
| E1 | **第一个里程碑**：Megalith 通过本引擎跑起来 | ✅ |
| E2 | 板上离屏 GPU 基准，实测预算（§6.2） | ✅ |
| E3a | 运行时接管主循环；渲染表面的参数来自卡带清单和系统，游戏不管 | ✅ |
| E3b | DSI 屏、旋转和 direct scanout、IMU | 等硬件 |
| E4 | 通用场景 API：资源、材质、世界（EnTT）、动画和 IK、碰撞、粒子和实例化、输入动作、混音器、存档、语言；`alia_kit`；Megalith 迁过去；示例游戏 | ✅ v0.1.0 |
| E5–E7 | 嵌进宿主应用（以后）、StellarAlia 掌机导出、2D 和比赛游戏移植（后期） | |

## 目录（计划）

```
docs/       架构和接口文档
src/        引擎各层（core / platform / rhi / render / asset / world / runtime / bindings）
tools/      PC 端资源烘焙工具
tests/      单元测试和黄金截图测试
```
