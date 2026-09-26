# iPad App 在 Mac 上的触控板手势转发实验

## 目标与结果

iPad App 在 Mac 上运行时，普通后台鼠标拖拽无法稳定触发应用内的拖动。此实验从真实触控板手势的 CGEvent tap 接收事件，将副本投递到指定进程和窗口，不修改目标 App。

已观察到的结果：

| 输入方式 | Xcode Touch Alternatives sample | App Store 版 Arknights |
| --- | --- | --- |
| 只转发连续 ScrollWheel | 未产生目标操作 | 未产生目标操作 |
| 按回调顺序转发 ScrollWheel 和原始类型 29 | 成功操作 | 成功操作 |
| 录制后独立重放 ScrollWheel 和原始类型 29 | 成功操作 | 成功操作 |
| 从零合成 ScrollWheel 和类型 29 | 尚未验证 | 后台水平、竖直位移成功 |

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
