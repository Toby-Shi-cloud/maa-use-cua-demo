# cua-shot（C++20 / macOS）

直接链接 Cua Driver 0.28.2 的 `libcua_driver_sdk.dylib`，通过官方 C ABI 在当前进程内运行。
不启动 cua-driver 子进程，不使用 daemon、CLI、Python 或 Node。动态库来自官方发布包，已放在 `vendor/cua`。

## 编译

需要 CMake、C++20 编译器、nlohmann-json 和 standalone Asio（本机已安装）。缺少依赖时：

```sh
brew install cmake nlohmann-json asio
cmake -S . -B build
cmake --build build
```

## 使用

```sh
# 列出当前屏幕窗口，确认真实 App 名称
./build/cua-shot --list
./build/cua-shot --list --app Code

# 截取指定 App 最靠前的窗口
./build/cua-shot --app Code --output ./screenshots/code.png

# 标题子串过滤 / 精确指定窗口 ID（从 --list 获取）
./build/cua-shot --app Code --title trycua -o ./screenshots/project.png
./build/cua-shot --window-id 14593 -o ./screenshots/window.png

# 包含其他 Space、隐藏或最小化窗口；可枚举不代表可截图
./build/cua-shot --list --all
./build/cua-shot --check-permissions
```

App 名称区分大小写，使用枚举结果中的名称，例如 VS Code 通常为 `Code`。
多个匹配窗口选择最大 `z_index`；顺序全部未知则要求明确指定 ID。
默认只列当前屏幕窗口，指定 ID 时允许查找屏幕外窗口。
输出必须是 `.png` 文件路径；自动创建父目录，成功后原子替换已有文件，失败保留原文件。
默认原始像素分辨率（`--max-dimension 0`），不限制截图长边。
可用 `--max-dimension 1568` 显式缩小；截图与点击必须使用相同设置。
通过命名会话的 `set_config` 设置上限，避免持久修改全局默认值。
退出码：成功 0，参数、窗口、SDK 或截图错误 1。

## macOS 权限与实测状态

直接嵌入时权限归属于启动程序的宿主。请在你实际运行命令的终端中检查：

```sh
./build/cua-shot --check-permissions
```

如权限未授予，在「系统设置 → 隐私与安全性」中为对应宿主（例如 Terminal、iTerm 或 IDE）
启用「屏幕与系统音频录制 / 屏幕录制」和所需的「辅助功能」，完全退出并重新启动宿主后再试。
仅为 CuaDriver.app 授权不会自动赋予本进程权限。SDK 的权限检查是只读操作，不自动弹出授权窗口。

本环境已验证：编译链接成功，Asio 协程通过直接 C ABI 枚举 Code 窗口、查询权限、异步关闭，
并成功截图保存到 `screenshots/code.png`。
首次截图曾返回未产生内容，后续重试成功；诊断仍显示 `accessibility=false`、`screen_recording=true`。
截图专用路径不遍历 AX 树，因此这里不把辅助功能未授权当成截图失败的充分条件。

## 实现

- `src/driver.hpp`：`create_v1` 创建 runtime，`invoke_v1` 异步调用，通过 `asio::async_initiate` / `use_awaitable` 桥接 C 回调；
  回调通过 `asio::post` 回到调用协程的 executor，work guard 保持事件循环存活。
  释放 operation 和 ABI buffer，退出时先 `co_await shutdown()`，再 `destroy_v1`。
- `src/main.cpp`：窗口过滤、选择，调用 `get_window_state` 的 `screenshot_out_file` 保存 PNG。
- `vendor/cua/include/cua_driver_abi.h`：官方匹配版本头文件，ABI 1.1。
- `CUA_SDK_ROOT` CMake 变量可替换 SDK 目录（include/ 和 lib/ 布局）。当前附带 macOS 库。

可选安装（库会一起安装，使用相对 RPATH）：

```sh
cmake --install build --prefix "$PWD/dist"
./dist/bin/cua-shot --help
```

官方资料：
- https://github.com/trycua/cua/blob/cua-driver-rs-v0.28.2/libs/cua-driver/rust/include/cua_driver_abi.h
- https://cua.ai/docs/how-to-guides/driver/use-sdk-in-process

## Asio 调用方式

```cpp
asio::awaitable<void> inspect(Driver& driver) {
    auto windows = co_await driver.call("list_windows", nlohmann::json::object());
    std::cout << windows.dump(2) << '\n';
    co_await driver.shutdown();
}
```

CLI 使用 `asio::io_context` + `co_spawn` 驱动协程。`call` / `shutdown` 不阻塞事件循环等待 C 回调；
初始化和本地文件读写仍是同步操作。Driver 必须存活到所有调用及 shutdown 完成，io_context 必须持续运行至完成。
目前未桥接 Asio cancellation slot，不提供超时或主动取消；不能通过提前销毁 Driver 来取消正在进行的调用。

## 窗口点击

```sh
# 先截图查看坐标；坐标是 PNG 像素，不是屏幕坐标或 macOS point
./build/cua-shot --app Code -o ./screenshots/before.png

# 左键点击，默认后台投递
./build/cua-shot --app Code --click --x 100 --y 100

# 双击 / 右键 / 显式前台投递
./build/cua-shot --window-id 14593 --click --x 100 --y 100 --count 2
./build/cua-shot --window-id 14593 --click --x 100 --y 100 --button right
./build/cua-shot --window-id 14593 --click --x 100 --y 100 --delivery foreground

# 只刷新截图、验证目标和坐标，不发送点击
./build/cua-shot --app Code --click --x 100 --y 100 --dry-run
```

`--button` 支持 left/right/middle；`--count` 支持 1/2。`--x`、`--y` 必须是非负有限数值。
点击前在同一 runtime 中调用 `get_window_state` 建立最新坐标映射，再调用 `click`。
点击必须唯一匹配一个可见窗口；有多个窗口时用 `--title` 或 `--window-id` 缩小范围。
坐标基于当次截图，窗口大小或 SDK 缩放配置改变后请重新确定坐标。
SDK 负责 Retina/缩放换算，CLI 不添加屏幕偏移。越界坐标在投递前拒绝。

点击时 `--output` 可选，指定后保存的是**点击前**的截图；未指定时临时截图会自动清理。
后台投递失败不会自动切换前台。`--delivery foreground` 会按 SDK 行为临时前置窗口再恢复原应用。
点击通常需要启动宿主的辅助功能权限，可用 `--check-permissions` 查询。
输出 `Click response` 代表 SDK 调用返回，实际 UI 效果仍应通过再次截图确认。

验证：编译通过，9 项无效参数检查通过，真实 Code 窗口 `--dry-run` 成功。
尚未向实际应用投递测试点击，也未验证 UI 点击效果。

### 原始分辨率与后台点击

默认 `--max-dimension 0` 解除了 SDK 截图长边上限，后台投递保持不变。
本机 Arknights 实测原始截图为 3942×2280，之前受限截图为 1568×907。
重新截图后使用新图的像素坐标，不能继续直接使用旧图坐标。

```sh
./build/cua-shot --app Arknights -o screenshots/arknights-native.png
./build/cua-shot --app Arknights --click --x 2464 --y 1960
```

上述坐标位于本次原始截图的「任务」区域；窗口大小和布局改变后需重新定位。
仅验证了原始截图尺寸和后台 dry-run，未发送测试点击。
CLI 仍对真实图片边界检查，原始图片范围内的坐标不再被旧的 1568 像素上限挡住。
0.28.2 后台左键可能返回 `route: accessibility`，CLI 显示该路由但不自动切换前台。

### 本地 SDK：坐标点击始终跳过 AXPress

项目使用 0.28.2 源码加本地补丁，删除 macOS 坐标点击的自动 AX 命中测试 / AXPress 分支。
CLI 的 `--click` 只传像素坐标，因此单击、双击都直接走鼠标事件路径，默认保持后台。
`--count 1` 是一个 down/up 对，不会伪装成双击。截图和其他 SDK 能力保持原样。

补丁位于 `vendor/cua/patches/0001-pixel-click-no-axpress.patch`。
安装 Rust 后运行 `./scripts/rebuild-cua.sh` 可从固定版本源码重建动态库。
重建产物针对当前机器架构；本次为 macOS arm64，不再包含原发布包的 x86_64 slice。
