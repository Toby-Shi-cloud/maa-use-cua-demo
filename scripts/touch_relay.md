# iPad App 在 Mac 上的触控板手势转发实验

## 目标与结果

iPad App 在 Mac 上运行时，普通后台鼠标拖拽无法稳定触发应用内的拖动。此实验从真实触控板手势的 CGEvent tap 接收事件，将副本投递到指定进程和窗口，不修改目标 App。

已观察到的结果：

| 输入方式 | Xcode Touch Alternatives sample | App Store 版 Arknights |
| --- | --- | --- |
| 只转发连续 ScrollWheel | 未产生目标操作 | 未产生目标操作 |
| 按回调顺序转发 ScrollWheel 和原始类型 29 | 成功操作 | 成功操作 |
| 录制后独立重放 ScrollWheel 和原始类型 29 | 成功操作 | 成功操作 |
| 从零合成 ScrollWheel 和类型 29 | -- | 后台水平、竖直位移成功 |
| 从零合成短促 Gesture 点击 | -- | 成功操作 |

第二、三行均由用户在两个目标上实际验证。第二行仍以**实时触控板手势**为输入源；第三行重放时不依赖实时手势。第四行由用户在 Arknights 后台验证，无需录制文件或实时手势。合成事件在 sample、最小化窗口、完全遮挡时的行为及重复成功率尚未验证。

## 构建和运行

```sh
cmake -S . -B build
cmake --build build --target live-relay
```

先观察一次真实双指手势，原事件会照常传递：

```sh
./build/live-relay --observe 2> /tmp/relay-events.log
```

观察模式不要求 PID 或窗口号，会记录 session tap 能见到的原始 CGEvent 类型、时间戳、位置与部分字段。然后用当次运行的目标 PID、CGWindowID 做对照：

```sh
./build/live-relay --scroll-only PID WINDOW_ID [LOCAL_X LOCAL_Y]
./build/live-relay --sequence PID WINDOW_ID [LOCAL_X LOCAL_Y]
```

不带模式参数等同于 `--scroll-only`。坐标是窗口本地的 **point**，省略时取窗口中心。运行时将真实指针放在目标窗口之外，再做双指手势。程序拦截选中的原事件，给副本写入目标 PID、窗口号和位置，并通过 SkyLight 投递；输出的 `relay-attempt` 仅表示已调用投递函数，实际效果以目标行为为准。

`--sequence` 额外订阅原始 CGEvent 类型 29，按 tap 回调顺序转发。代码保留事件副本的时间戳、ScrollWheel phase 和 118/119 字段，不凭空合成 Gesture。该类型在当前 CoreGraphics 公开头文件中没有命名常量；在其他系统版本应先用 `--observe` 核对事件流。

## 调试证据与解释

真实双指手势和旧 scroll-only relay 都能进入 sample 的 `UINSInputView.scrollWheel:`，所以旧方案的失败不发生在窗口路由入口之前。真实手势触发的 `UINSGameModuleScrollDrag.scrollWheel:` 还从 AppKit 事件队列取 ScrollWheel 与 Gesture，检查 Gesture subtype，并经 `NSEvent.CGEvent` 读取私有 double 字段 118/119，累加到模块的位移状态；达到阈值后会创建或更新 virtual finger。

旧 relay 只订阅并转发 ScrollWheel，缺少这部分事件序列。加入类型 29 的实时转发后，sample 和 Arknights 均出现预期操作。这支持“完整事件序列是旧方案缺失条件”的解释。当前证据没有逐项证明每个字段都是必需的；单纯改变 ScrollWheel 的 `MayBegin` phase 也没有被验证为可行替代。

代码中的 `field7` 是原始 CGEvent 字段 7，不能直接当作 NSEvent 的 subtype。观察日志若没有类型 29，不能推断目标 App 内也没有 Gesture：session tap 可能无法观察 AppKit 后续生成的事件。

## 后续验证

在 Xcode sample 中可以分别观察 `UINSGameModuleScrollDrag scrollWheel:`、`UINSVirtualDigitizer addVirtualFingerAtLoc:forKey:` 和 `modifyVirtualFingerForKey:withBlock:` 的命中情况，并以画面移动及后续操作正常作为最终判据。独立重放已在 sample 和 Arknights 上成功；合成手势已在 Arknights 后台成功。sample 的合成手势、不同窗口遮挡程度和多次手势的可靠性仍需分别验证。

## 记录与独立重放实验

运行记录模式后做**一次**真实双指手势，手势结束后按 Ctrl-C。文件记录连续 ScrollWheel
及原始类型 29 的扁平化 CGEvent、类型与相对时间。记录模式不会拦截原手势，也不会投递。

```sh
./build/live-relay --record gesture.cgevents
./build/live-relay --inspect gesture.cgevents
./build/live-relay --replay gesture.cgevents PID WINDOW_ID [LOCAL_X LOCAL_Y]
```

`--record` 拒绝覆盖已有文件；`.cgevents` 已被 Git 忽略，因为原始事件可能含设备、
进程与坐标信息。`--inspect` 离线验证文件并显示事件数、顺序和间隔，不需要输入权限。
`--replay` 在投递前验证所有已保存记录，把事件时间戳平移到当前运行时，并按原间隔发送到
目标窗口；这次运行不需要实时触控板输入。

旧版记录使用 CGEvent 自带时间戳；实测 ScrollWheel 与 Gesture 在 tap 中按顺序到达时，
时间戳仍可能倒退数百微秒。读取旧文件会保持回调顺序，并将倒退的时间点调整到上一事件的
时间点。新版记录改用回调到达时的单调时钟计算间隔。

本机测试发现 `CGEventCreateData` 会丢失人工构造的 Gesture 字段 118/119，因此格式
额外保存这两个 double 值，并在重放时恢复。记录时会立即重新解码，检查类型、
ScrollWheel phase 和这两个字段能否恢复；失败则停录并标记文件无效。
这不保证其他私有字段也被保留。用户已确认录制文件的独立重放能操作 sample 和 Arknights；
`replay-attempt` 日志本身仍只表示投递尝试，实际效果以目标行为为准。格式只用于当前实验，
不保证跨系统版本。

## 从零合成手势

`cua-shot --gesture` 提供了不依赖录制文件的实验入口。它按指定时长创建连续
ScrollWheel，并在每步发送两个类型 29 的 Gesture 事件。离线对照发现，新建的类型 29
事件默认在 AppKit 中呈现为 subtype 0；设置私有字段 110 为 6 后，AppKit 呈现的 subtype
与录制的位移事件一致。生成器还会设置位移字段 118/119 和目标窗口路由信息。

首轮 Arknights 实测看起来只出现类似轻触的反应，没有预期的水平滑动；进一步测试确认，
旧版命令实际触发了竖直位移。对照录制文件发现首版生成器把水平参数写进了字段 119，
并且没有设置 Gesture 阶段。录制的水平位移主要位于字段 118；
其位移事件字段 132 依次为 1（开始）、2（变化）、4（结束），字段 135 为 1。
修正后，用户已在 Arknights 后台验证水平和竖直位移均可成功。录制事件还包含新建
CGEvent 没有的附加数据；目前的成功说明这些差异没有阻止所测操作，但不证明它们在
其他手势或系统版本中无关。
另一次用 33 步、总时长 33 毫秒的旧版实验甚至触发了点击。新入口要求每步至少 8 毫秒，
防止用远快于所录制节奏的参数测试。

`--from-x/--from-y` 与 `--to-x/--to-y` 使用截图像素坐标。首版测试支持字段 119
控制竖直位移，修正版测试支持字段 118 对应水平位移。先用 `--dry-run` 检查截图坐标；
其他距离与步速、sample 以及遮挡状态仍需分别实测。不要把 `Synthetic gesture posted`
当作成功判据。

## 手势点击实验

`cua-shot --gesture-click --x X --y Y` 使用与合成滑动相同的窗口路由和截图像素坐标，
只发送一个极小位移的 ScrollWheel + Gesture 开始／结束序列，默认保持 80 毫秒。
它不调用 CUA 的 `click`，也不依赖录制文件；`--dry-run` 只验证窗口和坐标。

## ScrollWheel 必要性对照

已有实验中，单独转发 ScrollWheel 失败，ScrollWheel + Gesture 转发和合成输入成功。
在进行只发 Gesture 的对照实验时，未产生预期操作。实验仅省略
ScrollWheel，保留 Gesture 的 subtype、阶段、位移与步间时序。这表明当前目标和生成序列
仍需要 ScrollWheel 参与；不能据此推断所有 iPad App 或系统版本的输入要求。

sample 的调用栈显示 `UINSGameModuleScrollDrag scrollWheel:` 会处理真实触控板输入，
当前 `cua-shot` 不提供 `--gesture-only`，仍使用 ScrollWheel + Gesture。


## Option / Trackpad Capture 实验结论（已归档）

本轮 Option replay 实验代码、测试和 LLDB 跟踪脚本已移入 Git stash；下文保留脱敏后的
机制分析和实验结论，不代表当前工作区提供这些实验参数。未包含用户目录、真实 PID、
窗口 ID、设备 SenderID、内存地址或原始触点轨迹。原始录制、截图和调试输出不纳入此文档。

### 最终结果与适用范围

- 已确认：显式 Option 状态切换、恢复设备字段后，跨进程重放可在 Apple
  Touch Alternatives Sample **前台**移动小球。
- 已确认：**切到后台立即失去捕获**，小球随即回到中间。因此前台捕获后切后台
  也无法维持当前方案。回中的具体机制未确认，不能直接认定为点击、取消或框架自动复位。
- 当前依赖 Option 捕获的实现不适合作为持续后台控制方案，因此暂停这一实现方向。
  这不证明所有后台触点注入都不可能；已有普通 ScrollWheel + Gesture 的后台结果仍有效。
- 无实体触控板、任意设备 ID、虚拟设备、跨设备/重启后的录制，以及最终位移精度均未验证。

### 已确认的输入路径

普通双指滚动使用 `UINSGameModuleScrollDrag`；按住 Option 使用独立的
`UINSGameModuleTrackpadCapture`。LLDB 实际命中的入口链为：

```
NSApplication sendEvent:
  NSWindow sendEvent:
    UINSInputView flagsChanged:
      UINSGameEventTranslator flagsChanged:
        UINSGameModuleTrackpadCapture flagsChanged:
```

本机反汇编和 selector 解析显示（私有实现，非跨版本 API 保证）：

- `flagsChanged:` 检查 Option 位，调用 `_debouncedCaptureTrackpad` 或 `_releaseTrackpad`。
- `_captureTrackpad` 请求独占输入模块、取消既有输入、设置触摸类型，并隐藏/移动光标，
  调用 `SLAssociateMouseAndMouseCursorPosition` 与 `SLWarpMouseCursorPosition`。
- 触点入口是 `touchesBegan/Moved/Ended/CancelledWithEvent:`，随后进入
  `_handleTouchEvent:cancel:`。
- 转换支持 indirect IOHID digitizer 和 synthetic NSTouch 两条分支；真实 Option 单指
  操作命中了前者，未命中后者。

一次真实操作的累计断点计数如下。计数包含跟踪启用后的收尾事件，并非逐事件时间线，
不能将额外一次转换擅自归因于特定操作。Xcode 未显示预期的自动继续断点输出，证据来自
暂停后读取的 `breakpoint list -v`。

| 入口 | 命中次数 |
| --- | ---: |
| `flagsChanged:` | 3 |
| `_debouncedCaptureTrackpad` / `_captureTrackpad` | 1 / 1 |
| `touchesBeganWithEvent:` / `touchesMovedWithEvent:` / `touchesEndedWithEvent:` | 1 / 97 / 1 |
| `touchesCancelledWithEvent:` | 0 |
| `_handleTouchEvent:cancel:` | 100 |
| `_createDirectDigitizerIOHIDEventFromIndirectEvent:` | 100 |
| `_createDirectDigitizerEventFromSyntheticTouches:` | 0 |
| `_releaseTrackpad` | 2 |

### 重放所需信息及修复

仅给 ScrollWheel/Gesture 添加 Option modifier flags 不等于开始捕获。
实验版改为向相同目标发送左 Option 的 `flagsChanged`（keycode 58），等待捕获，重放，
再发送释放；使用原始事件间隔，并处理异常、SIGINT/SIGTERM 下的释放尝试。
这不是全局硬件按键，崩溃或 SIGKILL 时也不保证清理。

**更正早期推断：CGEvent 录制中确实可能保留 IOHID payload。** 两份本机录制解码后，
分别有 167/167、290/290 个事件带 digitizer；检查还发现 Finger 子事件、变化坐标和
抬起状态。不能仅凭录制器筛选了 ScrollWheel/Gesture 就认定触点数据缺失。

但 payload 存在不代表 AppKit 可以生成触点：所测录制的 CGEvent 私有字段 **87** 为零。
`_initMTTouchesFromIOHidEvent:` 用该字段查询设备 registry ID，再找到 touch device。
实验版仅在字段缺失且带 digitizer 时，从 `IOHIDEventGetSenderID` 恢复字段 87。

- 离线对照：恢复前 `allTouches` 为空；恢复后可生成 Began/Moved/Ended。
- 目标进程单步：恢复后实际读到 Began 触点，关联视图为 `UINSInputView`。
- 用户最终验证：新版本前台可移动小球。该验证未单独隔离字段 87 的因果作用。
- 早期自动对照只 Raise 窗口，没有确认应用激活和 key-window，不能作为前台失败证据。
  单步调试也会改变时序，不能代替完整重放测试。

恢复现有设备编号不等于创建虚拟设备。任意编号是否可用未经验证；当前解码路径会查找
设备对象，因此不能承诺无触控板环境可用。IOHID 旧时间戳虽存在，目标 NSTouch 使用了
更新后的 CGEvent 时间；没有证据表明旧 IOHID 时间是本次失败原因，未据此修改它。

### 坐标映射与可复用发现

indirect 转换复制 IOHID 事件，遍历 type 11 子事件，读取字段 `0xb0000`、`0xb0001`，
分别乘以 `convertSizeToScene:` 返回的场景宽、高，再写回副本。
synthetic 分支读取 `phase`、`_index`、`normalizedPosition` 并构建 digitizer finger。
这说明转换中的坐标缩放是乘法，但不证明最终 UIPan 位移没有识别阈值或其他处理。

本机符号探测发现：`CGEventCopyIOHIDEvent`、`IOHIDEventCreateDigitizerEvent`、
`IOHIDEventCreateDigitizerFingerEvent`、`IOHIDEventCreateData`、`IOHIDEventCreateWithData`；
未找到 `CGEventSetIOHIDEvent`。AppKit 有 `+[NSEvent _eventWithTouches:]`、
`-[NSEvent _setTouches:]`。符号存在仅表示可继续研究，不等于注入功能已验证。

实验版构建及两项自动测试通过，覆盖录制解码、Option 首尾顺序、目标字段、中断释放和
普通 replay 不增加 Option 事件；投递使用内存替身，不等于窗口操作测试。
若未来继续，优先研究无需前台捕获的触点分发，或校准已有后台 gesture+scroll 的位移；
不再单纯调大 Option 捕获等待时间。
