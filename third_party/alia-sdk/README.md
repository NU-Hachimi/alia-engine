The Alia Pad cart SDK's C headers (MIT, published with the Alia Pad devkit), vendored so the engine builds on its
own. With `-DALIA_SDK_DIR=<the SDK's include/alia>`, CMake warns when this copy differs from the SDK's.

- `save.h`: cart saves (atomic writes, two previous versions, a quota): the same files and rules as the SDK's
  Python `alia.save`.
