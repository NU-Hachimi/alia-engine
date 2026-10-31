# alia-engine 插件

插件是**游戏可以选用的一个 Python 包**，和 `alia_kit` 一样只用公开的接口（`docs/api.md`）。和 kit 的区别：kit 跟着引擎进每一张卡带，放的是每个游戏都会写的小件；插件只进用到它的卡带，放的是有一定规模、而且每个游戏用法不同的系统（补间、状态机、对话、寻路……）。第一方插件在引擎仓库的 `plugins/` 下，第三方的放在哪里都行，用法一样。

## 用一个插件

在项目的 `alia.toml` 里写上（`docs/project.md` §2）：

```toml
[engine]
plugins = ["tween", "my_dialogue"]   # 引擎自带的写名字；项目自己的放在 plugins/my_dialogue/
```

- `alia run`、`alia test`、`alia cart` 会检查每个插件支持的引擎版本，把它的包放进 `content/python/<包名>/`，和 `alia_engine`、`alia_kit` 在一起，游戏直接 `import`。
- 项目的 `plugins/` 放项目自己写的插件，或者带进来的第三方插件（复制进来，或者 git submodule），每个一个目录，格式和下面一样。
- 不改 `cart.toml`，系统侧也不知道插件的存在：对系统来说它就是游戏自己的文件。
- 不用 `alia` 的话，`tools/alia_plugin.py add <插件目录> <卡带目录>` 做同样的检查和复制，`list <卡带目录>` 看卡带里有哪些插件。

## 规矩

1. **插件是库，不是钩子**（`api.md` §9 第 8 条）：引擎不会去调插件，插件也不往主循环里挂东西。游戏持有插件的对象，在 `fixed_update` 里自己调它（`tweens.update(dt)`），和持有一个相机、一个 rig 一样。所以游戏永远看得见每一步在跑什么，板上的 Python 开销也看得见。
2. **只用公开接口**：`docs/api.md` 里写了的东西。不 monkeypatch 引擎和 kit，不碰以 `_` 开头的名字。api.md 的兼容承诺（从 v0.1.0 起只加不改）因此也保护插件。
3. **声明它支持的引擎版本**，并且在 import 时检查（见下面的模板）：版本不对时报清楚的错，不要跑到一半才坏。
4. **纯 Python**。C++ 插件要有稳定的 C++ 接口给它链接，现在没有；真有插件被 Python 的速度卡住时再定。
5. 包名不能是 `alia_engine`、`alia_kit`；第一方插件叫 `alia_<名字>`，第三方的建议带上自己的前缀，免得两个插件撞名。
6. 插件自己带测试（`tests/test_*.py`）；第一方插件的测试由 ctest 跑（`plugin_<名字>_test_*`）。

## 写一个插件

```
plugins/tween/
  plugin.toml            清单
  alia_tween/            包：整个目录被复制进卡带
    __init__.py
    ease.py
  tests/test_tween.py    不进卡带
```

`plugin.toml`：

```toml
[plugin]
name        = "tween"
package     = "alia_tween"          # 复制进 content/python/ 的目录
version     = "0.1.0"
engine      = ">=0.1, <1"           # 支持的 alia-engine 版本：逗号分开的 >= <= > < == != 条件，全都要满足
description = "..."
license     = "MIT"
```

`__init__.py` 开头检查版本（和 `engine` 写的一致）：

```python
import alia_engine as _ae

__version__ = "0.1.0"

if not (0, 1) <= tuple(int(p) for p in _ae.__version__.split(".")[:2]) < (1, 0):
    raise ImportError(f"alia_tween {__version__} needs alia-engine >=0.1, <1; this is {_ae.__version__}")
```

`tools/alia_plugin.py check <插件目录>` 检查清单；`add` 在复制前再按 `engine` 对一次卡带的引擎版本（默认是这个仓库的版本，`--engine` 指定别的）。

## 第一方插件

### tween（`alia_tween`）：补间

把任何属性在一段时间里缓动到一个值：物体的位置、缩放、朝向，`e.frame` 的天空和雾，游戏自己的数字。示例游戏用它让天色在一局里从黄昏变成夜晚（`apps/sample/src/field.py` 的 `nightfall`）。

```python
import alia_tween as tween
tw = tween.Tweens()                                          # 游戏持有一个（或者每个界面一个）

tw.to(door, "position", (0, 3, 0), 0.6, ease="out_back")     # 从现在的值开始
tw.to(node, "scale", 2.0, 0.3)                               # 一个数给 tuple 的每一项
tw.to(e.frame, "sky_top", 0x05070F, 60.0, colour="rgb")      # 打包的颜色：按通道补（"rgba" 是 0xRRGGBBAA）
tw.to(boss, "yaw", target, 0.4, angle=True)                  # 角度：走近的那一边
tw.to(hud, "score_shown", 120, 1.0)                          # int 还是 int
tw.to(lambda v: set_glow(v), None, 1.0, 0.5, start=0.0)     # 给一个函数：每步把值交给它（set_glow 是游戏自己的）
tw.to(x, "y", 1.0, 0.5, delay=0.2).loop(3, yoyo=True).then(on_done)   # 延迟、循环、来回、结束时回调
tw.call(1.5, spawn_wave)                                     # 定时器
tw.sequence(lambda: tw.to(cam, "fov_x", 0.9, 0.2), 0.5,      # 一步一步：开始补间的函数、等几秒、普通函数
            lambda: shake(), lambda: tw.to(cam, "fov_x", 1.25, 0.4)).then(on_done)
tw.stop(door)                    # 停掉 door 的补间（加 attr 只停一个属性；complete=True 先跳到终点并回调）
tw.busy(door), len(tw)           # 还有没有在跑的

def fixed_update(self, e, dt):
    tw.update(dt)                # 每步一次；暂停时不调它，补间就停在那里
```

- 缓动曲线：`linear`，`in_` / `out_` / `in_out_` 加 `quad`、`cubic`、`sine`、`expo`、`back`，还有 `out_elastic`、`out_bounce`、`in_bounce`（`alia_tween.ease.NAMES`）；也可以给一个函数 `t -> 份额`。默认 `out_quad`。
- 起点在补间**开始时**（延迟过后）读属性；`start=` 指定起点。
- 在 `fixed_update` 里调 `update`，引擎会在两步之间插值，补间动的物体画出来是平滑的。`then` 和 `sequence` 里开始的新补间从下一次 `update` 起动。
- `e.frame` 的颜色是打包的 int，**要给 `colour="rgb"`**，不然会把 0xRRGGBB 当一个数补，中间是乱的颜色。
