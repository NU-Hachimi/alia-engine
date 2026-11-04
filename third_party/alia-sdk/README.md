The Alia Pad cart SDK's C headers and shaders (MIT, published with the Alia Pad devkit), vendored so the engine builds on its
own. With `-DALIA_SDK_DIR=<the SDK's include/alia>`, CMake warns when this copy differs from the SDK's.

- `save.h`: cart saves (atomic writes, two previous versions, a quota): the same files and rules as the SDK's
  Python `alia.save`.
- `camera.h`, `camera_proto.h`: the camera client (protocol v2): frames, parameters, photos; the engine's camera
  (src/runtime/camera.cpp, docs/camera.md) is built on it.
- `camera/alia_camera.glsl`: the SDK's reference shader for developing the camera's raw frames; the engine compiles it
  into its developing pass, so games look like the system camera.
