## MAA Use cua Demo

我正在尝试 cua 能否正常后台点击从 App Store 下载的 Arknights。

## 快速测试

首先编译项目

```sh
brew install cmake nlohmann-json asio
cmake -S . -B build
cmake --build build
```

尝试截屏，第一次使用的时候可能会失败（会提示需要权限）。由于这是个 cli，所以需要启动这个 cli 的父进程有权限（如 Terminal.app 或者 VSCode.app）。(截图权限叫做「录屏与系统录音」)

```sh
./build/cua-shot --app Arknights --output ./screenshots/arknights.png
```

尝试点击，第一次使用的时候可能会失败（会提示需要权限）。由于这是个 cli，所以需要启动这个 cli 的父进程有权限（如 Terminal.app 或者 VSCode.app）。（辅助操作的权限是「设备控制和数据访问」）

```sh
# 尝试从主界面点击「任务」
# 这里坐标需要换算，我本地的明日方舟窗口大小是 3942x2280
# 如果你的窗口大小是 (X, Y)，尝试点击 (0.625X, 0.86Y) 这样的位置
./build/cua-shot --app Arknights --click --x 2464 --y 1960
```

可以将明日方舟窗口放在后台尝试点击（注意，请不要隐藏窗口、最小化窗口、或放在台前调度的后台）

我的测试环境：
```
Mac mini (M6)
macOS 27.0
```

## 其他

[README_chatgpt.md](README_chatgpt.md) 是 ChatGPT 写的详细的使用方案。
