# iPad App 在 Mac 上的触控板手势转发实验

## 目标与结果

iPad App 在 Mac 上运行时，普通后台鼠标拖拽无法稳定触发应用内的拖动。此实验从真实触控板手势的 CGEvent tap 接收事件，将副本投递到指定进程和窗口，不修改目标 App。

已观察到的结果：

| 输入方式 | Xcode Touch Alternatives sample | App Store 版 Arknights |
| --- | --- | --- |
| 只转发连续 ScrollWheel | 未产生目标操作 | 未产生目标操作 |
| 按回调顺序转发 ScrollWheel 和原始类型 29 | 成功操作 | 成功操作 |

第二行由用户在两个目标上实际验证。它仍以**真实触控板手势**为输入源；没有验证独立生成事件序列、最小化窗口、完全遮挡时的行为或重复成功率。

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

在 Xcode sample 中可以分别观察 `UINSGameModuleScrollDrag scrollWheel:`、`UINSVirtualDigitizer addVirtualFingerAtLoc:forKey:` 和 `modifyVirtualFingerForKey:withBlock:` 的命中情况，并以画面移动及后续操作正常作为最终判据。还需用不同窗口遮挡程度和多次手势测可靠性。若目标是无人值守自动化，下一项独立实验是记录成功的原始事件序列，再验证无实时触控板输入时能否重放；本工具当前不提供这种重放能力。
