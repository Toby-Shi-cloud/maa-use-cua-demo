## MAA Use cua Demo

尝试后台点击从 App Store 下载的 Arknights。

## 快速测试

首先编译项目

```sh
brew install cmake nlohmann-json asio
cmake -S . -B build
cmake --build build
```

尝试截屏，第一次使用的时候可能会失败（会提示需要权限）。启动本 cli 的终端需要「录屏与系统录音」权限。

```sh
./build/cua-shot --app Arknights --output ./screenshots/arknights.png
```

尝试点击，第一次使用的时候可能会失败（会提示需要权限）。启动本 cli 的终端需要「设备控制和数据访问」权限。

```sh
# 尝试从主界面点击「任务」
# 这里坐标需要换算，我本地的明日方舟窗口大小是 3942x2280
# 如果你的窗口大小是 (X, Y)，尝试点击 (0.625X, 0.86Y) 这样的位置
./build/cua-shot --app Arknights --click --x 2464 --y 1960
```

## 干员列表拖动：Gesture + ScrollWheel

合成手势使用窗口截图像素坐标，不需要录制文件或实时触控板输入。
先打开干员页面，再按当前截图调整起终点；可以加 `--dry-run` 先核对命令，
正式发送时去掉它：

```sh
# 打开干员页面，运行下面的指令（可以后台）
# 如果你的分辨率比较小，就调整一下数值
CUA_GESTURE_GATE=step ./build/cua-shot --app Arknights --gesture \
  --from-x 2200 --from-y 900 --to-x 1200 --to-y 900
```

`CUA_GESTURE_GATE=step` 是一种输入端配对方案，合法的手势需要同时有 Gesture 和 ScrollWheel 事件，并且两者有严格的时序和时间间隔要求。
由于 Mac 没有提供可用的批量投递 api，所以我们被迫采用了先向程序发送 SIGSTOP 暂停，然后发送单步的全部事件，再发送 SIGCONT 继续的做法保证时序和时差。
不使用这个宏在绝大多数普通场景中也没有影响。但是对于干员部署这种高精度要求的场景，则必须要开启才能提高部署成功概率。

## 实验记录和其他用法

完整的可复现实验命令、输入端实现限制及接收端证据见
[`scripts/sample-trace/README.md`](scripts/sample-trace/README.md)；
Arknights 试放见 [`scripts/arknights_gesture.md`](scripts/arknights_gesture.md)。
当前开关仍是诊断性质：发送进程若被强制终止，目标进程可能保持暂停，
需要向目标 PID 发送 SIGCONT。

其它输入方式的历史状态见 [`scripts/input_methods.md`](scripts/input_methods.md)；
步数、缓动、停留及旧版距离校准见
[`scripts/gesture_distance.md`](scripts/gesture_distance.md)。
[README_chatgpt.md](README_chatgpt.md) 保留较早的详细使用记录。
