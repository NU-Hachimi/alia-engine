# 开始做一个游戏

从零到一个能在 PC 上玩、能装到 Alia Pad 上的游戏。写游戏的接口看 `docs/api.md`，项目布局和 `alia` 的全部命令看 `docs/project.md`。

## 1. 准备（一次）

**PC**：Linux x86_64，**Python 3.13**（板子上卡带跑的就是 3.13，Alia Pad 平台 1.0 的规定；最低 3.12，但新于 3.13 的语法和标准库在板子上没有，`alia` 会提醒），系统里有 SDL3（3.2 或更新）和 EGL / GLES（Mesa 或显卡驱动）。Debian 13、Ubuntu 25.04、Fedora 42、Arch 这一代的发行版都行：

```sh
sudo apt install python3-venv libsdl3-0 libegl1 libgles2      # Debian / Ubuntu；Arch：pacman -S sdl3 mesa；Fedora：dnf install SDL3 mesa-libEGL
```

**alia**（引擎的命令行工具）：装进一个 Python 3.13 的环境，再 `alia setup`。系统自带的不是 3.13（比如 Arch 已经是 3.14）就用 conda：`conda create -n alia python=3.13 pip && conda activate alia`，再从 `pip install` 那行做起。

```sh
python3.13 -m venv ~/.alia && . ~/.alia/bin/activate            # 以后每次用之前 activate，或者把 ~/.alia/bin 加进 PATH
pip install https://github.com/NU-Hachimi/alia-engine/releases/download/v0.2.1/alia_engine-0.2.1-py3-none-any.whl
alia setup
```

`alia setup` 会下载这个版本编好的引擎（PC 用的；`alia setup --board` 连板子用的一起），并装上 Alia Pad devkit 的 `acart`（打包、安装、启动卡带的工具）。**不用编译任何东西，也不用碰 devkit。**

**板子**：刷好 Alia Pad 的开发镜像，开 adb，用 USB 连上电脑（`adb devices` 能看到它）。

## 2. 新项目

```sh
alia new mygame && cd mygame
python3.13 -m venv .venv && .venv/bin/pip install pytest   # 项目自己的测试环境，alia 会用它（用 conda 的话装进同一个环境，不建 .venv）
alia run                  # 开窗口玩（键盘：WASD 走，IJKL 转视角，X = A 键；键位和 Alia Pad 模拟器一样，docs/api.md §1）
alia test                 # 测试，在无头引擎里跑
git init && git add -A && git commit -m "mygame"
```

`alia new` 生成推荐的布局（`docs/project.md` §1）和一个能跑的小游戏，`src/main.py` 就是 `docs/api.md` §0 的快速开始，从这里改起。

- 改了 `src/` 的代码直接 `alia run`：开发时游戏是链接进去的，不用重新打包。
- 资产（`.apak` 资源包、`.wav`、`.json`）放 `assets/`，游戏从 `content/assets/` 读（`docs/project.md` §1）。资源包可以在代码里生成（`alia_pack`），也可以由项目的 `pipeline/` 做好提交进来。
- 用插件：在 `alia.toml` 的 `[engine] plugins` 里写上（`docs/plugins.md`）。

## 3. 上板子

```sh
alia cart                 # build/cart/ 和 build/mygame.acart（板子用的引擎第一次用时下载）
alia install              # 装到板子上
alia launch               # 在板子上启动（会停掉前台正在跑的卡带）
alia logs                 # 看它的输出；alia stop 停掉
```

- `acart` 打包要 `mksquashfs`（`sudo apt install squashfs-tools`）。
- 卡带清单 `cart/cart.toml` 的格式（标题、操作、分辨率、图标和截图、存档配额）见 Alia Pad devkit 的卡带文档。
- `alia run --sim` 在 PC 上的 Alia Pad 模拟器里运行（devkit 的 `acart run`：和掌机一样的系统环境、沙箱和手柄，要 Docker），比直接 `alia run` 慢一点，但和板子上一模一样。`alia cart --pc` 只打出 PC 版的卡带（`build/cart-pc/`）。

## 4. 引擎的版本

`alia` 和它用的引擎是同一个版本（`alia --version`）。Python 的版本跟着 Alia Pad 的平台走：平台 1.x 是 3.13，以后换了 Python 平台会升到 2.0。换版本就装另一个版本的 wheel（`pip install` 那个版本的 URL），`alia setup` 再下载对应的引擎。项目的 `alia.toml` 写它支持哪些版本（`[engine] version = ">=0.2, <1"`），不对时 `alia` 会直接说。每个版本改了什么在 GitHub 的 Releases 页面。

## 5. 改引擎、写原生代码的人

不用上面的 wheel，直接用引擎的源码：

```sh
git clone https://github.com/NU-Hachimi/alia-engine.git && cd alia-engine
python3 tools/alia.py setup       # 在 ~/.local/bin 放一个指向这个源码的 alia；之后项目里的 alia run / test 都从源码编译
```

- 编译要 CMake ≥ 3.24、clang、Ninja、SDL3、EGL 和 GLES 的开发包、Python 3.13（README 的"构建和运行"）。
- 板子的交叉编译要板子的 sysroot：devkit 的 `sysroot/fetch.sh` 生成（默认 `sysroot/out/aarch64`，约 580 MB），然后 `export ALIA_SYSROOT=<那个目录>`；引擎用自己的 `cmake/aarch64-board.cmake`，C/C++ 的卡带可以用 devkit 的 `sysroot/toolchain-aarch64.cmake`。
- 在板子上跑引擎自己的测试：`tools/board-deploy.sh`（`docs/architecture.md` §5.1）。
- 发布一个版本：`tools/release.sh`（两种平台的模块在 Debian 13 容器里编，`--publish` 建 GitHub release）。
