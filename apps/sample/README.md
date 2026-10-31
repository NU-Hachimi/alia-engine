# Wisp Keeper（捕光）：alia-engine 的示例游戏

黄昏的草地上飘着光点，躲在一圈立石后面；60 秒内用光束尽量多地捉住它们，连着捉到的分数更高，最好成绩会存下来。

```sh
cd apps/sample
../../tools/alia.py run                                          # PC 窗口（键盘或手柄）
../../tools/alia.py run --demo --headless --frames 600 --screenshot shot.png
../../tools/alia.py cart && ../../tools/alia.py install && ../../tools/alia.py launch   # 作为卡带在板子上跑
```

操作：左摇杆走路，右摇杆转视角，B（South）放光束，A（East）开始，Start 暂停。键盘用 Alia Pad 平台的键位（devkit 的 `docs/tools.md` §2「键盘」）。

## 它是怎么组织的

项目按引擎推荐的布局放（`docs/project.md`）：`alia.toml`（引擎版本、用 tween 插件）、`cart/cart.toml`、`src/`（代码）。它不带资产文件：网格和声音都在代码里做。

这是引擎推荐的写法（`docs/api.md` §9 第 5、7 条）：**引擎里没有给游戏继承的基类**，游戏就是普通的 Python 类，每个类持有自己的引擎物体；一个 `Game` 类在固定步长里调度它们。引擎负责画、插值、动画、粒子模拟、混音；游戏负责"谁想去哪儿、发生了什么"。

| 文件 | 类 | 持有的引擎物体 | 用到的接口 |
|---|---|---|---|
| `src/main.py` | `Game` | 粒子模板、发射器、声音、补间 | 主循环（§1.1）、输入动作（§7）、存档（§7）、HUD、插件 `alia_tween`（`docs/plugins.md`） |
| `src/field.py` | `Field` | `kit.Terrain` 的地面、立石（`kit.box`，带胶囊碰撞体） | 高度图地面（§5）、`kit.environment` 的黄昏再调一点（`e.frame`，§1.2） |
| `src/keeper.py` | `Keeper` | 一个 rig、它的挂点、光束；`alia_kit` 的 `GroundMover`、`FollowCamera`、`Gaze` | 代码里做的 rig 模板（§4）、`look_at` IK（§4.1）、射线（§5）、`alia_kit`（§7.1） |
| `src/wisps.py` | `Wisp` | 一个发光的球（带球形碰撞体）、挂在它下面的拖尾发射器 | 碰撞层（§5）、粒子（§6）、发光材质（§2） |
| `src/sounds.py` | | | 代码里合成声音；网格用 `kit.box`、`kit.sphere`。示例不带资源文件（真正的游戏用资源包和 WAV） |

几条值得照抄的做法：

- **只在 `fixed_update` 里改物体**，`render` 里只画 HUD：引擎在两步之间插值，画面比逻辑步平滑（§3）。
- **多次用到的东西做成模板**：keeper 的 rig（`keeper_template`，一份数据，`spawn_rig` 一次）、粒子模板（每种颜色一个拖尾，一个爆发）、光点的球网格；实例化时给参数（位置、颜色）。
- **碰撞层说清楚"什么挡什么"**：立石是 `SOLID`，光点是 `WISP`，光束的射线两层都查；`hit.user` 是光点的编号，直接找到被捉的那个。
- **对象池**：捉到的光点不销毁，换个远处的位置再出现（`Wisp.place`，`teleport()` 免得从原地滑过去）。
- **`animator.play()` 会从头开始放**：只在换动画时调，平时只改 `speed`（`Keeper.update`）。
- **走路和相机用 `alia_kit`**：`GroundMover` 贴地、挡在立石外面（它们的胶囊碰撞体在 `SOLID` 层），`FollowCamera` 从右肩后面跟着（准星不压在角色身上）。
- **头看光点不抖**：选目标有回差（`Game._watched`：离开 16 m、换一个要近 25%、只看身前 90° 以内的），头沿 `kit.Gaze` 平滑过的方向看、不越过身体两边 70°（`docs/api.md` §4.1 写了头抖的三个原因）。
- **输入读动作，不读按键**（`ACTIONS` 表），以后做改键菜单只要换表、存进存档。
- **天色用补间插件**：一局开始时 `Field.nightfall` 让天空、雾和太阳在 60 秒里缓动到夜晚（`alia_tween`，`alia.toml` 的 `plugins` 让卡带带上它）；暂停时不调 `tweens.update`，夜色也停住。
- **声音只告诉混音器放什么**：音乐 `play_music` 一次，音效每次 `play`，音调随连击升高。
- `--demo` 用脚本输入自己玩，测试（`ctest -R sample`）和截图都用它；脚本跑的时候不写存档。

文字只有英文：HUD 的字体只有 ASCII（`docs/api.md` §8）。卡带的中文名「捕光」写在 `cart/cart.toml` 的 `[locale.zh]` 里，Shell 自己显示。
