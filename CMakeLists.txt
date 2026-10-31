cmake_minimum_required(VERSION 3.24)
project(alia_engine VERSION 0.2.1 LANGUAGES C CXX)

# Layers (docs/architecture.md §2): each is a static library; dependencies only point down.
#   alia_core (L0) <- alia_platform (L1) <- alia_rhi (L2) <- alia_render (L3)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Engine libraries also link into the Python module (a shared object).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(ALIA_BUILD_TESTS "Build tests" ON)
option(ALIA_BUILD_APPS "Build demo apps" ON)
option(ALIA_WITH_SDL "Window support via SDL3 (OFF: headless only)" ON)
option(ALIA_WITH_PYTHON "Python module alia_engine (L7, nanobind)" ON)

# Our own code builds warning-clean; third-party sources get their own flags.
set(ALIA_WARNINGS -Wall -Wextra -Wpedantic -Werror)

# Board system libraries (the platform baseline, §5.1): linked dynamically.
find_package(PkgConfig REQUIRED)
if(ALIA_WITH_SDL)
  pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
endif()
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLES REQUIRED IMPORTED_TARGET glesv2)

# ---- third party (bundled, static) ----
add_library(alia_stb STATIC third_party/stb_impl.c)
target_include_directories(alia_stb SYSTEM PUBLIC third_party/stb)
set_target_properties(alia_stb PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(alia_glm INTERFACE)
target_include_directories(alia_glm SYSTEM INTERFACE third_party/glm)
target_compile_definitions(alia_glm INTERFACE GLM_FORCE_XYZW_ONLY GLM_ENABLE_EXPERIMENTAL)

# Built against Python 3.13 (the board's version, §5.1) but with the stable ABI, so the module
# (alia_engine.abi3.so) keeps working on any Python >= 3.12 when Debian moves on. The
# interpreter is only used at configure time; a cross build takes headers and the extension
# suffix from the sysroot.
if(ALIA_WITH_PYTHON)
  set(ALIA_PYTHON "" CACHE FILEPATH "Host Python 3.13 (empty: found on PATH)")
  if(ALIA_PYTHON)
    set(Python_EXECUTABLE "${ALIA_PYTHON}")
  endif()
  if(CMAKE_CROSSCOMPILING)
    set(Python_INCLUDE_DIR "${CMAKE_SYSROOT}/usr/include/python3.13")
    set(SKBUILD_SOABI "cpython-313-aarch64-linux-gnu")  # FindPython would ask the host interpreter
    set(SKBUILD_SOSABI "abi3")
  endif()
  find_package(Python 3.13 EXACT REQUIRED COMPONENTS Interpreter Development.Module Development.SABIModule)
  set(nanobind_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanobind/cmake")
  find_package(nanobind CONFIG REQUIRED)
  if(NOT TARGET Python::SABIModule)
    message(FATAL_ERROR "Python stable ABI (SABIModule) not found; nanobind would build a version-locked module")
  endif()
endif()

# ---- L0 core ----
add_library(alia_core STATIC
  src/core/log.cpp
  src/core/image.cpp)
target_include_directories(alia_core PUBLIC src)
target_compile_options(alia_core PRIVATE ${ALIA_WARNINGS})
target_link_libraries(alia_core PRIVATE alia_stb)

# ---- L1 platform ----
if(ALIA_WITH_SDL)
  add_library(alia_platform STATIC src/platform/window_sdl.cpp src/platform/keymap.cpp src/platform/headless_egl.cpp)
  target_link_libraries(alia_platform PUBLIC alia_core PRIVATE PkgConfig::SDL3 PkgConfig::EGL)
else()
  add_library(alia_platform STATIC src/platform/window_none.cpp src/platform/headless_egl.cpp)
  target_link_libraries(alia_platform PUBLIC alia_core PRIVATE PkgConfig::EGL)
endif()
target_compile_options(alia_platform PRIVATE ${ALIA_WARNINGS})

# ---- audio (beside L1): the mixer, and one SDL3 output stream ----
if(ALIA_WITH_SDL)
  add_library(alia_audio STATIC src/audio/mixer.cpp src/audio/device_sdl.cpp)
  target_link_libraries(alia_audio PRIVATE PkgConfig::SDL3)
else()
  add_library(alia_audio STATIC src/audio/mixer.cpp src/audio/device_none.cpp)
endif()
target_include_directories(alia_audio PUBLIC src)
target_compile_options(alia_audio PRIVATE ${ALIA_WARNINGS})

# ---- L2 rhi ----
add_library(alia_rhi STATIC
  src/rhi/gles/device_gles.cpp)
target_compile_options(alia_rhi PRIVATE ${ALIA_WARNINGS})
target_link_libraries(alia_rhi PUBLIC alia_core PRIVATE PkgConfig::GLES)

# ---- L3 render ----
add_library(alia_render STATIC
  src/render/final_pass.cpp
  src/render/handheld.cpp
  src/render/test_scene.cpp)
target_compile_options(alia_render PRIVATE ${ALIA_WARNINGS})
target_include_directories(alia_render PRIVATE third_party/glcdfont)
target_link_libraries(alia_render PUBLIC alia_rhi PRIVATE alia_glm)

# ---- L4 asset ----
add_library(alia_asset STATIC
  src/asset/pack.cpp)
target_include_directories(alia_asset PUBLIC src)
target_compile_options(alia_asset PRIVATE ${ALIA_WARNINGS})

# ---- L5 world ----
add_library(alia_entt INTERFACE)
target_include_directories(alia_entt SYSTEM INTERFACE third_party/entt)
add_library(alia_world STATIC
  src/world/particles.cpp
  src/world/rig.cpp
  src/world/world.cpp)
target_compile_options(alia_world PRIVATE ${ALIA_WARNINGS})
target_link_libraries(alia_world PUBLIC alia_render alia_asset alia_entt alia_glm)

# ---- L6 runtime ----
add_library(alia_runtime STATIC
  src/runtime/actions.cpp
  src/runtime/app.cpp
  src/runtime/engine.cpp
  src/runtime/locale.cpp
  src/runtime/saves.cpp)
target_compile_options(alia_runtime PRIVATE ${ALIA_WARNINGS})
# The Alia Pad cart SDK's headers (save.h, MIT), vendored in third_party/alia-sdk. With ALIA_SDK_DIR (the SDK's
# include/alia), warn when the copy differs from it.
target_include_directories(alia_runtime PRIVATE third_party/alia-sdk)
set(ALIA_SDK_DIR "" CACHE PATH "the Alia Pad cart SDK's include/alia, to check the vendored copy against (optional)")
if(ALIA_SDK_DIR AND EXISTS "${ALIA_SDK_DIR}/save.h")
  file(SHA256 "${ALIA_SDK_DIR}/save.h" _upstream)
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/third_party/alia-sdk/alia/save.h" _copy)
  if(NOT _upstream STREQUAL _copy)
    message(WARNING "third_party/alia-sdk/alia/save.h differs from ${ALIA_SDK_DIR}/save.h: copy it again")
  endif()
endif()
target_link_libraries(alia_runtime PUBLIC alia_platform alia_render alia_world alia_audio PRIVATE alia_asset)

# ---- L7 bindings ----
if(ALIA_WITH_PYTHON)
  # NB_STATIC: nanobind's runtime is linked into the module, nothing extra to ship.
  nanobind_add_module(alia_engine STABLE_ABI NB_STATIC src/bindings/module.cpp)
  target_compile_options(alia_engine PRIVATE ${ALIA_WARNINGS})
  target_link_libraries(alia_engine PRIVATE alia_runtime)
  target_compile_definitions(alia_engine PRIVATE ALIA_VERSION="${PROJECT_VERSION}")
  # alia_engine.pyi: the module's types and docstrings for editors (autocompletion, type checks), generated from the
  # built module by nanobind's stubgen; it ships beside the module. A cross build can't import its module: the PC
  # build's stub is the same, and tools/release.sh puts it in both archives.
  if(NOT CMAKE_CROSSCOMPILING)
    nanobind_add_stub(alia_engine_stub MODULE alia_engine OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/alia_engine.pyi
                      PYTHON_PATH $<TARGET_FILE_DIR:alia_engine> DEPENDS alia_engine)
  endif()
  # alia_kit (python/alia_kit.py): pure-Python helpers that ship beside the module (api.md §7.1); copied next
  # to it whenever the file changes, so PYTHONPATH=build sees both.
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/alia_kit.py
                     COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/python/alia_kit.py ${CMAKE_CURRENT_BINARY_DIR}/alia_kit.py
                     DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/python/alia_kit.py)
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/alia_pack.py
                     COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/tools/alia_pack.py ${CMAKE_CURRENT_BINARY_DIR}/alia_pack.py
                     DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/alia_pack.py)
  add_custom_target(alia_kit ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/alia_kit.py ${CMAKE_CURRENT_BINARY_DIR}/alia_pack.py)
  add_dependencies(alia_engine alia_kit)
  # First-party plugins (plugins/<name>/, docs/plugins.md): copied next to the module too, so PYTHONPATH=build
  # imports them while developing; carts take them with tools/alia_plugin.py add, not with the install.
  file(GLOB ALIA_PLUGIN_MANIFESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/plugins/*/plugin.toml)
  foreach(manifest ${ALIA_PLUGIN_MANIFESTS})
    get_filename_component(plugin_dir ${manifest} DIRECTORY)
    file(STRINGS ${manifest} package_line REGEX "^package *=")
    string(REGEX REPLACE "^package *= *\"([A-Za-z0-9_]+)\".*" "\\1" package "${package_line}")
    file(GLOB_RECURSE plugin_sources CONFIGURE_DEPENDS ${plugin_dir}/${package}/*.py)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${package}/__init__.py
                       COMMAND ${CMAKE_COMMAND} -E copy_directory ${plugin_dir}/${package} ${CMAKE_CURRENT_BINARY_DIR}/${package}
                       DEPENDS ${plugin_sources})
    add_custom_target(plugin_${package} ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${package}/__init__.py)
  endforeach()
endif()

if(ALIA_BUILD_APPS)
  add_executable(alia-e0-demo apps/e0-demo/main.cpp)
  target_compile_options(alia-e0-demo PRIVATE ${ALIA_WARNINGS})
  target_link_libraries(alia-e0-demo PRIVATE alia_platform alia_render)

  add_executable(alia-bench apps/bench/main.cpp)
  target_compile_options(alia-bench PRIVATE ${ALIA_WARNINGS})
  target_link_libraries(alia-bench PRIVATE alia_runtime)
endif()

if(ALIA_BUILD_TESTS)
  enable_testing()
  add_executable(test_render tests/test_render.cpp)
  target_compile_options(test_render PRIVATE ${ALIA_WARNINGS})
  target_link_libraries(test_render PRIVATE alia_render)
  add_test(NAME render_math COMMAND test_render)

  # Golden image test: renders headless (surfaceless EGL) and compares to tests/golden/.
  add_executable(test_golden tests/test_golden.cpp)
  target_compile_options(test_golden PRIVATE ${ALIA_WARNINGS})
  target_link_libraries(test_golden PRIVATE alia_platform alia_render)
  target_compile_definitions(test_golden PRIVATE ALIA_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
  add_test(NAME golden COMMAND test_golden)

  add_executable(test_handheld tests/test_handheld.cpp)
  target_compile_options(test_handheld PRIVATE ${ALIA_WARNINGS})
  target_link_libraries(test_handheld PRIVATE alia_platform alia_render)
  target_compile_definitions(test_handheld PRIVATE ALIA_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
  add_test(NAME handheld COMMAND test_handheld)

  add_executable(test_asset tests/test_asset.cpp)
  target_compile_options(test_asset PRIVATE ${ALIA_WARNINGS})
  target_link_libraries(test_asset PRIVATE alia_asset)
  add_test(NAME asset COMMAND test_asset)

  add_executable(test_world tests/test_world.cpp)
  target_compile_options(test_world PRIVATE ${ALIA_WARNINGS})
  target_link_libraries(test_world PRIVATE alia_world)
  add_test(NAME world COMMAND test_world)
  add_executable(test_audio tests/test_audio.cpp)
  target_compile_options(test_audio PRIVATE ${ALIA_WARNINGS})
  target_link_libraries(test_audio PRIVATE alia_audio)
  add_test(NAME audio COMMAND test_audio)

  if(ALIA_WITH_SDL)
    add_executable(test_keymap tests/test_keymap.cpp)
    target_compile_options(test_keymap PRIVATE ${ALIA_WARNINGS})
    target_link_libraries(test_keymap PRIVATE alia_platform PkgConfig::SDL3)
    add_test(NAME keymap COMMAND test_keymap)
  endif()
  add_executable(test_runtime tests/test_runtime.cpp)
  target_compile_options(test_runtime PRIVATE ${ALIA_WARNINGS})
  target_link_libraries(test_runtime PRIVATE alia_runtime)
  add_test(NAME runtime COMMAND test_runtime)
  set_tests_properties(golden handheld runtime PROPERTIES SKIP_RETURN_CODE 77)

  if(ALIA_WITH_PYTHON AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME python COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python.py)
    set_tests_properties(python PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:alia_engine>")
    # The sandbox keeps running: 30 frames headless through every exhibit.
    add_test(NAME sandbox COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/apps/sandbox/src/main.py --headless --frames 30)
    set_tests_properties(sandbox PROPERTIES ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:alia_engine>")
    add_test(NAME kit COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_kit.py)
    set_tests_properties(kit PROPERTIES ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:alia_engine>")
    add_test(NAME plugin_tool COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_plugin_tool.py)
    add_test(NAME alia_tool COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_alia_tool.py)
    set_tests_properties(alia_tool PROPERTIES ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:alia_engine>")
    foreach(manifest ${ALIA_PLUGIN_MANIFESTS})  # each plugin's tests/test_*.py, against the build
      get_filename_component(plugin_dir ${manifest} DIRECTORY)
      get_filename_component(plugin_name ${plugin_dir} NAME)
      file(GLOB plugin_tests ${plugin_dir}/tests/test_*.py)
      foreach(t ${plugin_tests})
        get_filename_component(tname ${t} NAME_WE)
        add_test(NAME plugin_${plugin_name}_${tname} COMMAND ${Python_EXECUTABLE} ${t})
        set_tests_properties(plugin_${plugin_name}_${tname} PROPERTIES ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:alia_engine>")
      endforeach()
    endforeach()
    # The sample game plays itself for 10 s (--demo: walks, casts, catches), headless.
    add_test(NAME sample COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/apps/sample/src/main.py --demo --headless --frames 600)
    set_tests_properties(sample PROPERTIES ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:alia_engine>")
  endif()
endif()

# ---- install ----
# The engine ships inside each cart (the Alia Pad cart docs): --prefix <cart>/content.
#   python/  Python modules (PYTHONPATH)
#   lib/     shared libraries (LD_LIBRARY_PATH); empty while everything links statically
set(ALIA_INSTALL_PYTHONDIR python CACHE STRING "Python modules, relative to the prefix")
set(ALIA_INSTALL_LIBDIR lib CACHE STRING "Shared libraries, relative to the prefix")
if(ALIA_WITH_PYTHON)
  install(TARGETS alia_engine LIBRARY DESTINATION ${ALIA_INSTALL_PYTHONDIR})
  if(NOT CMAKE_CROSSCOMPILING)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/alia_engine.pyi DESTINATION ${ALIA_INSTALL_PYTHONDIR})
  endif()
  # alia_kit, and alia_pack for games that build packs (rigs, meshes) in code: e.load_rigs(PackWriter().bytes())
  install(FILES python/alia_kit.py tools/alia_pack.py DESTINATION ${ALIA_INSTALL_PYTHONDIR})
endif()
