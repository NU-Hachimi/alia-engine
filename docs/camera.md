# 相机

游戏用掌机的相机：相机画面当背景（AR 的第一级），在 HUD 里画取景框，拍照。相机的帧和参数来自 Alia Pad 卡带 SDK 的相机接口
（`alia/camera.h`，协议 v2，见 Alia Pad 的卡带文档；引擎带了一份在 `third_party/alia-sdk/`）；引擎负责把原始帧变成画面，游戏只看到下面这些。

## 接口

```python
cam = e.camera                      # Engine.camera（DeviceCamera）；不是 e.world.camera（那是看场景的视角）
cam.open()                          # 要相机：默认是场景尺寸（e.scene_size）、全视野
cam.open(1280, 720, full_fov=False) # 或者自己给尺寸；可以中心裁切（视野窄，更省）
cam.close()                         # 不用了就关（传感器断电）

cam.state       # "closed" "starting" "pending"（在问用户） "running" "paused"
                # 用不了时是原因："denied" "revoked" "busy" "unavailable" "suspended" "failed"
cam.message     # 出错时的说明文字（给开发者看，不是给玩家的）
cam.size        # 画面尺寸（转向之后），还没有画面时是 None
cam.frame       # 最新一帧：sequence、timestamp_ns（单调时钟，曝光结束）、exposure_us、gain、wb_red、wb_blue、colour_temperature
cam.mode        # 拿到的传感器模式：width、height、fps、turn、order；内参 fx fy cx cy k1 k2 p1 p2 k3（原始帧像素，转向之前）、measured
cam.zero_copy   # True：GPU 直接读相机的缓冲区（掌机上）；False：每帧复制一次（有的模拟环境）

e.frame.camera_background = "cover"   # 天空的位置画相机画面，填满、多出的裁掉；"fit"：完整显示，留黑边；"off"（默认）
e.hud_camera(x, y, w, h)              # HUD 里画相机画面（取景框），场景像素：裁掉多出的部分填满这个框
e.hud_camera(x, y, w, h, fit=True, color=0xFFFFFF80)   # 完整显示、在框里居中；color 乘在画面上（这里半透明）

shot = cam.photo()      # 拍照，异步：shot.done，然后 shot.jpeg（bytes）或 shot.error
# 之后某一步：if shot.done and shot.jpeg: ae.save.write("photo-1.jpg", shot.jpeg)
```

`hud_camera` 和 `hud_rect`、`hud_text` 一样按调用顺序画，可以在上面再画框线、文字、准星。背景画在 3D 场景后面，3D 物体照常画在上面（相机画面上叠游戏物体）。

## 行为

- **显影**：每来一帧新的，引擎按 SDK 的参考着色器（和系统相机同一套步骤：去马赛克、黑电平、白平衡、暗角、颜色矩阵、色调曲线）在 GPU 上显影一次，
  缩放到画面尺寸，画进引擎自己的一张纹理；背景和取景框都用这一张。没有新帧时用上一张。
  - **画面尺寸**：相机自己的长宽比（转向之后），刚好盖住 `open` 给的尺寸，但不超过原始帧（显影只会丢细节，不会多出细节）。
  - **缓冲区**：掌机上 GPU 直接读相机的缓冲区（不复制），显影完（GPU 栅栏）马上还回去；读不了的环境退回映射内存、每帧上传一次。
- **更新**：每个显示帧读一次相机（不阻塞，不开线程），在 `fixed_update` 之前更新 `state`、`frame`。
- **权限**：卡带的清单（`cart.toml`）要声明 `[permissions] camera = true`。第一次 `open` 时系统问用户，`state` 先是 `"pending"`；
  问的时候系统把游戏暂停（和打开系统菜单一样），回答后接着跑，之后变成 `"running"`，或者 `"denied"`。用户的选择系统会记住（设置里能改）。用户在设置里关掉是 `"revoked"`，画面立刻没有。
- **暂停**：拍照的那一下、卡带不在前台、系统相机临时拿走时是 `"paused"`，画面停在最后一张；恢复后引擎自己重新接上，游戏不用管。
- **拍照**：只在相机开着时能拍（不然 `shot.error`），一次一张（前一张还没好时再拍是 `shot.error`）。结果是正立的 JPEG，视野和当前画面一样
  （裁切模式就是裁切后的视野），全分辨率。引擎不替游戏存：游戏自己用 `ae.save.write` 存进自己的存档；引擎只留最近 8 张的结果。
- **用不了**：`"unavailable"`（机器上没有相机，或者没有相机服务：比如电脑上的模拟器，相机模拟还没做）、`"busy"`（别人在用）、
  `"suspended"`（卡带不在前台）、`"failed"`（系统那边出错，`message` 说原因）。这时背景和取景框画成黑色；要重试就 `close()` 再 `open()`。
- **转向**：相机的安装方向引擎自己处理，`cam.size` 和画面都是转好的；内参是原始帧的（AR 标记识别用到时再给换算好的）。
- **别的相机服务**：环境变量 `ALIA_CAMERA_SOCKET` 让引擎连另一个相机服务（测试、模拟器）。

## 代价

板上显影 640×480 约 4.6 ms（GPU 在最低频率时实测），每来一帧新画面做一次（相机 30 或 60 帧）。选尺寸时按游戏真的要画多大来给：
背景就是场景尺寸（默认），只要取景框就给小一点。

## 第一版不做

- 相机画面当材质的纹理（引擎还没有纹理，等纹理一起做）。
- 标记 AR（识别打印的标记、把物体放在上面）：要内参实测和标记识别的库。
- 存进相册：等 SDK 的接口；到时一个调用，相机照片和游戏截图都能存。
- 录像。

## 测试

`tests/test_camera.py`：一个假的相机服务（同一个协议，测试图案），引擎无头跑：问用户、显影的颜色和方向（含 90° 安装）、背景（cover / fit）、
取景框、缓冲区归还、拍照（暂停、新模式、恢复）、拒绝、中途关掉权限、没有服务。电脑上帧是复制的；不复制的导入在掌机上验证。
