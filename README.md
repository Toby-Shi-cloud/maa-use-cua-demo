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
尺寸遵循该版本 SDK 的截图配置，不保证原始像素分辨率。
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
