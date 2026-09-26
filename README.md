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

> `--drag` 这个后台拖拽对于 Finder 这样的 AppKit App 可以生效，但是对于 Arknights 不能生效。

实验性的合成手势入口在主程序中，从零创建连续 ScrollWheel 与类型 29 的 Gesture
事件序列，不需要录制文件或实时触控板输入。坐标仍使用窗口截图像素；先用
`--dry-run` 核对目标和坐标，再观察实际运行是否让 App 响应：

```sh
# 打开干员页面，运行下面的指令（可以后台）
# 如果你的分辨率比较小，就调整一下数值
./build/cua-shot --app Arknights --gesture \
  --from-x 2200 --from-y 900 --to-x 1200 --to-y 900
```

`--gesture` 要求每步至少 8 毫秒，例如 33 步需 `--duration-ms 264` 或更长；过短的序列
曾在 Arknights 中表现为一次点击，而非滑动。

可以将明日方舟窗口放在后台尝试点击（注意，请不要隐藏窗口、最小化窗口、或放在台前调度的后台）

我的测试环境：
```
Mac mini (M6)
macOS 27.0
```

## 其他

[README_chatgpt.md](README_chatgpt.md) 是 ChatGPT 写的详细的使用方案。

## 实验现状

```
CG/SkyLight background click
        ↓
Arknights ✅


CG/SkyLight background mouse drag
        ↓
Finder ✅
Arknights ❌


CG/SkyLight live-relay real trackpad scroll
        ↓
Finder ✅
Touch Alternatives Sample ❌
Arknights ❌


CG/SkyLight live-relay real trackpad scroll + gesture
        ↓
Touch Alternatives Sample ✅
Arknights ✅


CG/SkyLight recorded scroll + gesture replay
        ↓
Touch Alternatives Sample ✅
Arknights ✅


CG/SkyLight synthesized scroll + gesture
        ↓
Touch Alternatives Sample 待验证
Arknights 待验证


真实 Trackpad
pointer 在后台 Arknights 上
        ↓
Arknights ✅


foreground CUA mouse drag
        ↓
抢 cursor
        ↓
Arknights ✅


Karabiner Virtual HID Mouse
        ↓
真正 HID dx/dy
        ↓
抢 cursor ✅
```
