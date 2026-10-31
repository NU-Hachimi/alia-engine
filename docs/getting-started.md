# 开始做一个游戏

从零到一个能在 PC 上玩、能打成卡带的游戏项目。写游戏的接口看 `docs/api.md`，项目布局和 `alia` 的全部命令看 `docs/project.md`。

## 1. 准备（PC，一次）

- **编译引擎要的东西**：CMake ≥ 3.24、clang、Ninja、SDL3、EGL 和 GLES 3（Mesa 或显卡驱动）、Python 3.13。Arch：`pacman -S cmake clang ninja sdl3 mesa python`；Debian / Ubuntu：`cmake clang ninja-build libsdl3-dev libegl-dev libgles-dev python3.13-dev`。引擎是 C++ 写的 Python 模块，第一次用时在你的机器上编译（一两分钟）。
- **引擎**：

  ```sh
  git clone https://github.com/NU-Hachimi/alia-engine.git ~/alia-engine
  python3 ~/alia-engine/tools/alia.py setup        # 在 ~/.local/bin 放一个 alia 命令（这个目录要在 PATH 里）
  ```

- **测试要的 pytest**：每个项目用自己的 `.venv`（下面第 2 步），`alia` 自己会找到它。

## 2. 新项目

```sh
alia new mygame && cd mygame
python3 -m venv .venv && .venv/bin/pip install pytest
alia run                  # 开窗口玩（键盘：WASD 走，方向键转视角）；第一次会先编译引擎
alia test                 # 测试，无头引擎里跑
git init && git add -A && git commit -m "mygame"
```

`alia new` 生成的是推荐的布局（`docs/project.md` §1）和一个能跑的小游戏：`src/main.py` 就是 `docs/api.md` §0 的快速开始。从这里改起。

- 改了 `src/` 里的代码，直接 `alia run`：开发时游戏是链接进去的，不用重新打包。
- 资产（`.apak` 资源包、`.wav`、`.json`）放 `assets/`，游戏从 `content/assets/` 读（`docs/project.md` §1）。资源包可以在代码里生成（`alia_pack`），也可以由项目的 `pipeline/` 生成好提交进来。
- 用插件就在 `alia.toml` 的 `[engine] plugins` 里写上（`docs/plugins.md`）。

## 3. 引擎的版本

`alia` 用的是它所在的那个引擎仓库，所有项目共用一份。`alia.toml` 的 `[engine] version` 写游戏支持哪些版本，不对时 `alia` 会直接说。要换版本，在引擎仓库里切到对应的标签：

```sh
git -C ~/alia-engine fetch --tags && git -C ~/alia-engine checkout v0.2.0
```

以后引擎的每个版本会附上编好的模块，`alia` 按 `alia.toml` 取，就不用在本机编译 C++ 了。

## 4. 上板子（Alia Pad）

需要 **Alia Pad devkit** 里的两样东西：板子的 **sysroot**（交叉编译用的头文件和库）和 **`acart`**（打包、安装、启动卡带的工具），还有一台开了 adb 的板子。

```sh
export ALIA_SYSROOT=/path/to/devkit/sysroot      # 也可以写进 shell 的配置
alia cart                 # build/cart/ 和 build/mygame.acart（第一次会交叉编译板子的引擎模块）
alia install              # 装到板子上
alia launch               # 在板子上启动（会停掉前台正在跑的卡带）
alia logs                 # 看它的输出；alia stop 停掉
```

- `alia cart --pc` 打一个 PC 版的卡带（`build/cart-pc/`），给模拟器用。
- 卡带清单 `cart/cart.toml` 的格式（标题、操作、分辨率、图标和截图、存档配额）见 Alia Pad 的卡带文档（devkit）。
- 板子上跑引擎自己的测试（改引擎的人用）：`tools/board-deploy.sh`（`docs/architecture.md` §5.1）。
