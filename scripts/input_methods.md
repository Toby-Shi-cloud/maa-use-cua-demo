# 其它输入方式实验记录

这是从主 README 移出的历史状态快照。不同条目来自不同时期的实验，成功符号
只表示当时观察到应用响应，不表示落点精度、长期稳定性或后台条件均已验证。
当前 gesture+scroll 的接收端诊断见
[`sample-trace/README.md`](sample-trace/README.md)，游戏试放见
[`arknights_gesture.md`](arknights_gesture.md)。

## 离线检查

构建后可运行下列检查，不会向目标 App 发送输入：

```sh
ctest --test-dir build --output-on-failure
```

这只验证录制格式和合成事件的离线行为，不能代替游戏画面中的落点验证。

普通后台鼠标拖拽在 Finder 中有效，在 Arknights 中无效。另有独立的
`--gesture-click` 实验入口，它发送极小位移的 ScrollWheel + Gesture 开始／结束序列：

```sh
./build/cua-shot --app Arknights --gesture-click --x 2464 --y 1960 --dry-run
./build/cua-shot --app Arknights --gesture-click --x 2464 --y 1960
```

窗口可在后台，但不可隐藏或最小化；实际点击效果仍需截图确认。

测试机器当时为 Mac mini (M6)、macOS 27.0。

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
Arknights 后台水平／竖直位移 ✅


CG/SkyLight synthesized gesture click
        ↓
Arknights ✅


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
