# 游戏项目：目录布局和 `alia` 工具

一个用 alia-engine 做的游戏是一个独立的项目（自己的仓库），按下面的布局放文件，用引擎带的 `alia` 工具运行、测试、打卡带。游戏不需要 Makefile，也不需要知道引擎怎么编译。引擎仓库里的 `apps/sample/`（示例游戏）和 `apps/sandbox/` 就是这样的项目。

## 1. 布局

```
mygame/
  alia.toml        项目文件：引擎版本、用哪些插件（§2）
  cart/            cart.toml（卡带清单：标题、操作、分辨率，格式见 Alia Pad 的卡带文档（devkit）），以及要跟着清单走的固定文件
  src/             游戏代码：main.py 在顶层，其余是它的模块。只放手写的代码    → 卡带的 content/
  assets/          做好的资产：游戏运行时读的资源包（.apak）、音频（.wav）、数据（.json）  → content/assets/
  plugins/         项目自己的插件，或者带进来的第三方插件（每个一个目录，有 plugin.toml）；
                   用不用由 alia.toml 决定                                       → content/python/
  tests/           测试（pytest），在无头引擎里跑
  tools/           游戏自己的开发工具：回归快照、卡带美术……（不进卡带）
  pipeline/        资产生产线：源文件（建模、rig 的 JSON、MIDI……）和把它们变成 assets/ 的脚本（不进卡带）
  docs/            设计文档
  pyrightconfig.json  编辑器的配置（补全、类型检查：引擎的 .pyi 在 build/dev/content/python）；alia new 生成，alia setup --editor 补上
```

- **代码和资产分开**：`src/` 只有手写的代码，生成的东西一律不放进来。游戏在 `content/assets/` 找它的资产（`Path(__file__).parent / "assets"`），开发时和卡带里是同一个位置（§3）。
- **资产生产线是独立的**：`pipeline/` 有自己的依赖（Blender、fluidsynth、numpy……），自己的说明；它把产物写进 `assets/` 并提交。发布时资产已经定好了，所以卡带、游戏代码、引擎都不依赖它，打卡带也不跑它。只改代码的人不用装它的任何东西。
- 只有代码、没有资产和插件的小游戏，只要 `alia.toml`、`cart/cart.toml`、`src/main.py`。

## 2. `alia.toml`

```toml
[game]
name = "megalith"          # 卡带文件名（build/<name>.acart）；默认是目录名
# src = "src"  assets = "assets"  cart = "cart"  tests = "tests"     # 默认值，一般不用写
art = "tools/cart_art.py"  # 可选：打卡带前运行，参数是卡带目录（画图标、封面、截图）

[engine]
version = ">=0.2, <1"      # 支持的引擎版本，写法同插件（docs/plugins.md）
plugins = ["tween"]        # 引擎自带的插件写名字；项目的插件写 plugins/ 下的目录名（或者相对路径）
```

## 3. `alia` 工具

`tools/alia.py`，在项目目录（或者它下面的任何目录）里运行：

```sh
alia new mygame            # 新项目：上面的布局，加一个能跑的小游戏和一个测试
alia run [--demo ...]      # PC 上开窗口玩；后面的参数交给游戏（--headless、--frames N、--screenshot、--stats……）
alia run --sim [...]       # 在 Alia Pad 模拟器里运行（devkit 的 acart run：和掌机一样的环境）
alia test [-k name]        # 测试（pytest），在无头引擎里
alia exec tools/x.py ...   # 运行项目自己的工具，游戏的模块和引擎都能 import（同 test 的 PYTHONPATH）
alia cart [--pc]           # 卡带：build/cart/（板子）并打包成 build/<name>.acart；--pc 是给模拟器的 build/cart-pc/
alia install | launch | stop | logs    # 板子上的卡带（用 Alia Pad devkit 的工具）
alia engine [pc|board]     # 只编译引擎模块（run、test、cart 需要时会自己做）
alia plugin check DIR      # 检查插件的 plugin.toml
```

- **引擎从哪来**：用 pip 装的 `alia`（`docs/getting-started.md`）用同版本编好的引擎，第一次用时从 GitHub release 下载到 `~/.cache/alia-engine/<版本>/`，不编译；从引擎源码运行的 `alia`（`tools/alia.py`）用那份源码编译，结果在源码的 `build/`（PC）和 `build-board/`（板子，交叉编译要板子的 sysroot：`ALIA_SYSROOT`），所有项目共用。`alia --version` 说是哪一种。
- **Python**：项目有 `.venv/` 就用它（`python3 -m venv .venv && .venv/bin/pip install pytest`），否则 `python3`；`PYTHON=` 可以指定。
- **卡带的内容**（`alia cart`）：

  ```
  cart.toml + cart/ 里的其他文件
  content/             src/ 的内容（main.py 在这里）
  content/assets/      assets/
  content/python/      alia_engine（板子或 PC 的）、alia_kit、alia_pack、alia.toml 里的插件
  ```

  `main.py` 要把 `content/python` 放进 `sys.path`（`alia new` 生成的就有这一行）。
- **`run` 和 `test` 用同样的布局**：`build/dev/content/` 里，代码和资产是指向 `src/`、`assets/` 的链接（改了马上生效），引擎和插件是复制的。测试的 `PYTHONPATH` 是 `content` 和 `content/python`，所以测试直接 `import` 游戏的模块。
- `art`：用 PC 的引擎无头运行，`PYTHONPATH` 同测试；它把图写进卡带目录，`cart.toml` 的 `[art]` 引用它们。
