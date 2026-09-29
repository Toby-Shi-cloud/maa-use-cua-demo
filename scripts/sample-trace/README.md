# Touch Alternatives 接收端诊断

当前结论：不改接收端、仅让输入端在每一步投递期间短暂停止目标进程，
固定参数在 Sample 上连续 10 次到达相同落点；明日方舟同一指令的重复性尚未验证。
本页按调查顺序保留早期失败方案及其证据，最终输入端实验见文末
[“仅在输入端配对”](#仅在输入端配对逐步暂停目标进程)。

`ViewController.swift` 是 Apple TouchAlternativesSample 的诊断替换文件，保留原有
began 记录起点、changed 更新圆心、ended 不更新的逻辑。原始下载项目没有修改。
本次已复制到 `build/sample-trace-project` 并通过 Xcode 的 My Mac (Designed for iPad)
构建、运行。直接 open Debug-iphoneos 下的裸 app 会报 executable format 错误，需通过 Xcode Run。

## 记录内容

- UIPanGestureRecognizer 子类收到的 touch began/moved/ended/cancelled：位置、上一位置、
  precise location、touch/event 时间戳、接收 uptime、coalesced samples、phase。
- 原有 pan action 的状态、location、translation、velocity、起点及圆心更新前后。
- tap 归中及动画完成、scene 激活/失活、视图 bounds 和 screen.scale。
- 这是 UIKit/recognizer 入口的触摸，不是硬件原始数据，也不是 Touch Alternatives 内部日志。

手势期间在内存追加，手势结束后串行后台写 JSONL。每次启动创建新文件；Xcode console 的
`INPUT_TRACE_PATH` 给出路径（App 容器 Documents/input-trace-UUID.jsonl）。
日志仍会增加开销，不能把带日志的失败率等同于未插桩版本的失败率。

分析命令：

```sh
python3 scripts/sample-trace/analyze.py '/日志路径/input-trace-....jsonl'
```

分析器针对本次单指实验；每个 touch.began 分组，输出 pan 起点、最后 changed、ended、
触摸首尾、识别前位移、touch尾端与最后changed差、圆心位移。所有坐标为 UIKit points。
`max_delivery_lag_ms` 只是 UITouch.timestamp 至回调日志的时间差，不能测到进入 UIKit 前的全部延迟。
coalesced_count 含当前样本，不能直接当作被合并事件数。

重复回放前：用 `cua-shot --list --app TouchAlternativesSample` 获取当前 PID/window，
重新填入 replay 命令。旧 `scripts/test_10x.sh` 中的 PID/window 是历史值，本次未改该用户脚本。
每次归中后等待动画完成并截图确认；同时保存截图与对应日志。避免调试断点暂停。

## 本次实测发现

400ms / 输入(343,-351)，20步和40步各3次，另回放原 recording 10次。
本地原始数据在 build/trace-summary.jsonl、build/replay-trace-results.jsonl 和 build/move-study/。

1. **坐标缩放是较大距离差的一部分。** 窗口宽1024 points、原生图宽2048 pixels，
   UIKit view.bounds.width却是1330。换算应为每UIKit point约2048/1330=1.53985截图像素，
   而不是直接乘screen.scale=2。screen.scale并不能单独表示iPad兼容窗口的渲染比例。
2. 一次20步输入：字段X总和171.5（343/2）；touch首尾差162.925；
   pan began比touch起点晚17.15；touch ended比最后pan changed多8.575；
   圆心实际差137.2 UIKit points，乘2048/1330后为211.267 pixels，与截图211.258吻合。
   由此可分开识别前损失、尾部更新损失、渲染缩放，不能全归为事件丢失。
3. **同样40步，touch首尾本身不同。** 前两次水平128.625，第三次141.4875，
   相差12.8625 UIKit points（3个字段步长）。pan识别前损失三次均12.8625，
   尾部差均4.2875；最终圆心差111.475 vs124.3375。波动已发生在pan action之前。
4. Replay十次也观察到触摸首尾位移不一致。pan ended与最后changed的位置均一致，
   所以在这些回放中，仅让Sample在pan ended再赋值不会修复差异。
   这不证明底层具体丢了哪些事件：还需在发送端记录实际发送时间，并跟踪ScrollDrag内部
   接收/累计过程，区分调度、过滤、合并或转换。当前日志版在Xcode调试器下，不能与用户约10%失败率直接比较。

因此：`circle_final = circle_at_pan_began + last_pan_changed - pan_began` 确实成立。
但CGEvent的phase与坐标并不是UIPanGestureRecognizer的phase与坐标；把发送端changed终点
当成UIKit最后changed位置，是此前推理缺失的一步。

## 官方资料

- [UIPanGestureRecognizer](https://developer.apple.com/documentation/uikit/uipangesturerecognizer)：移动达到识别条件后才进入began。
- [Coalesced touches](https://developer.apple.com/documentation/uikit/getting-high-fidelity-input-with-coalesced-touches)：UIKit可合并触摸样本；这只是需要记录的机制，不是本次根因已被证明。
- [Touch Alternatives sample](https://developer.apple.com/documentation/apple-silicon/providing-touch-gesture-equivalents-using-touch-alternatives)：官方示例与输入替代背景。

## 发送端与AppKit队列对齐（后续调查）

诊断版另加入仅在本进程生效的 Objective-C method hooks，调用原实现并保持返回对象：
`NSApplication.nextEventMatchingMask:untilDate:inMode:dequeue:` 与
`UINSGameModuleScrollDrag.scrollWheel:`。记录队列取出的22/29事件、CG时间戳、118/119、phase，
以及ScrollDrag两项tracking/translation布尔状态。没有得到模块完整累计位移，不能声称已直接读到内部积分器。
这是私有实现的诊断代码，不能用于跨系统版本的产品实现。Hook本身也可能影响调度。

发送端设置环境变量 `CUA_GESTURE_TRACE=build/send.csv` 可记录每个步骤的调用前后mach纳秒时间、
phase和位移；只在整个动作结束后写CSV，不在发送循环内做磁盘IO。每次覆盖指定文件。

```sh
CUA_GESTURE_TRACE=build/send.csv ./build/cua-shot --app TouchAlternativesSample --gesture \
  --from-x 1024 --from-y 832 --to-x 1367 --to-y 481 --steps 40 --duration-ms 400
python3 scripts/sample-trace/align_queue.py '/容器路径/input-trace-....jsonl' build/send.csv
```

固定(343,-351)、40步、400ms，5次都从AppKit队列取出了42个subtype6事件
（begin + 40 changed + end），X增量总和都是171.499996，发送端为171.5，差值仅浮点精度。
本组没有证据表明translation增量在抵达AppKit队列前丢失。

|次序|touch首尾dx|识别前dx|touch尾端超出最后changed|圆心dx（UIKit）|截图dx（px）|
|---:|---:|---:|---:|---:|---:|
|1|162.925|17.150|4.2875|141.4875|217.871|
|2|171.500|17.150|4.2875|150.0625|231.049|
|3|162.925|12.8625|0|150.0625|231.049|
|4|167.2125|12.8625|4.2875|150.0625|231.049|
|5|167.2125|17.150|8.575|141.4875|217.871|

每次满足 `圆心dx = touch首尾dx - 识别前dx - 尾端差`。
相同translation总和产生不同touch起止和pan边界，不能再笼统称为“漏发了几个增量”。
具体边界如何由ScrollDrag/virtual digitizer/UI调度决定仍待验证。
第一组独立发送时间trace中最大步时延约2.39ms；尚不能据此排除其它组的发送抖动。

40步放慢到1600ms的3次结果仍为224.464、237.691、237.691px，没有消除波动。
下一步应分别验证：微小非零起步并等待touch建立、结束前确保最后位置已成为changed；
与继续增加总duration或直接乘位移倍率相比，这两项更直接针对本次观察到的边界变化。
本组未重测replay，不把translation全到的结论扩展为所有录制回放都没有队列前丢失。

本地结果：build/queue-aligned.jsonl、queue-state-*.csv、queue40-send.csv，截图在build/move-study/queue-*。

## 微小起步 + 等待的对照结果

新增可选 `--gesture-start-hold-ms`：began 从零位移改为沿运动方向1个字段单位
（总位移不足1时取整个距离），后续增量扣除这一单位；默认0保持旧行为。
主移动均400ms、40步、输入差(343,-351)，本次仍使用队列Hook版Sample。
四组交错各3次；仅起步等待组追加7次，共19次。截图水平位移如下：

|条件|总时长|次数|实际水平位移px|
|---|---:|---:|---|
|无等待|400ms|3|231.049 / 231.049 / 224.464|
|起步40ms|440ms|10|217.020×4 / 230.146×5 / 223.573×1|
|末尾40ms|440ms|3|217.871 / 237.691 / 237.691|
|起步40ms+末尾40ms|480ms|3|236.724 / 236.724 / 230.146|

结论：没有解决稳定性。起步组前三次一致，补到10次后出现三个落点。
“微小起步就能在等待期间建立touch”这个假设没有成立：首轮6个带seed的手势中，
began事件时间到touch.began的间隔为49.86～50.43ms，恰好在首个正式changed发出时
（40ms等待+10ms步间隔），而不是等待阶段。日志在build/seed-summary.jsonl及App容器中。
这支持下一轮单独检验预备阶段的changed/phase128，而不是继续增加同样的静默等待。
尚不能仅凭此确认是phase条件还是位移阈值触发了touch建立。

代码测试验证seed大小、总位移守恒、首尾坐标；CGEvent内部字段存在float精度截断，
总和测试使用1e-5字段单位容差。事件测试和构建通过。没有改默认输入行为。

## 零位移 changed / phase128 预备事件对照

诊断环境变量 `CUA_GESTURE_PRELUDE=control|changed|maybegin|both`（需start-hold>=30ms）。
不设置时完全沿用原路径。四组时间表相同：10ms发送带1单位seed的began，50ms首个正式changed，
440ms最后移动，480ms ended。maybegin组额外在0ms发送ScrollWheel phase8、普通Gesture、
translation phase128（后两项增量为0）；changed组额外在20ms发送零位移changed三事件组合。
总字段位移(171.5,-175.5)保持不变，40步，主移动400ms，起/尾各40ms。
这只复现预备phase，不等同于完整真实录制的所有字段、间隔和位置。

各3次交错测试结果（截图dx）：

|条件|第1次|第2次|第3次|
|---|---:|---:|---:|
|control|236.724|236.724|230.146|
|零位移changed|230.146|223.573|236.724|
|phase128|230.146|223.573|236.724|
|两者都有|236.724|236.724|230.146|

12次translation字段X接收总和均171.499996。没有一个方案稳定。
touch.began时间相对began约37～46ms，仍在正式移动启动附近；没有在20ms的预备changed附近建立touch。
因此本次零位移changed、phase128都没达到提前建立触摸的目标。不能据此证明所有预备事件无作用，
也不能直接区分非零增量要求和位移阈值。

本轮12次touch最终坐标均等于最后pan changed，尾端差为0；尾部40ms停留在本组覆盖了末步。
而pan识别前水平位移仍在约12.81～17.08 UIKit points变化，部分touch首尾总位移也不同。
后续应控制非零changed的增量及间隔，检验是否能在主移动前完成touch建立和pan识别。
发送trace在build/prelude-*.csv，截图build/move-study/prelude-*.png；分析结果build/prelude-summary.jsonl。
新增测试覆盖四种预备模式的事件数、phase8/128及字段总位移守恒。不要把诊断环境变量设为全局默认。

## 非零changed扫描与汇编（后续）

诊断变量 `CUA_GESTURE_PRIME=N` 仅配合changed/both预备模式：在20ms的预备changed发送沿
运动方向长度N的字段位移，并从正式移动中扣除。需N有限、非负且不超过seed之外剩余距离。
默认不设置，仍发送零位移changed；不改变产品默认动作。

N=1/4/8/16各3次（其余时间表和总位移与上一节相同）：

|N|截图dx三次|
|---:|---|
|1|229.149 / 229.149 / 229.149|
|4|239.320 / 232.846 / 219.900|
|8|228.933 / 228.933 / 222.578|
|16|215.057 / 202.782 / 215.057|

12次touch.began均提前到预备阶段（相对began约6.5～14.1ms），pan began仍在其后约56.7～70.2ms。
说明非零预备changed确实能提前建立touch，但这不等于提前完成pan识别；没有可靠解决落点波动。
N=1仅3次一致，不作稳定性保证。接收字段X总和均约171.5（float截断级误差）。
本地数据：build/prime-*.csv、prime-summary.jsonl、move-study/prime-*.png。

通过Xcode LLDB暂停并导出当前UIKitMacHelper原始方法（不是替换后的hook），之后已恢复运行。
本地dump为build/scroll-drag.asm、scroll-drag-block.asm、scroll-threshold.txt。
这只针对当前系统构建，不保证其它系统版本相同。关键证据：

- scrollWheel方法+680～+764：识别subtype6，读取118/119，向两个double的累计向量相加。
- +204～+224：读配置double，比较是否为0，非0时选择常量1.0，0时选择0.0作为后续比较门槛。
  该次暂停读到原配置值为5，但在此段指令经过fcsel后的有效比较门槛是1.0；不是直接拿5比较。
- +248～+288：虚拟手指尚未建立时，依次比较abs(accumX)、abs(accumY)。任一>=门槛进入创建路径，
  两者均小于门槛则跳出。因此是按轴比较，不是欧氏长度>=1。
- +292之后进入虚拟手指创建/更新路径；更新block +60～+76将起始位置与累计向量相加。

本次对角线单位seed的分量为(0.698909,-0.715210)，两个绝对值都<1，所以单独seed和零changed
不越过门槛；再发长度1的非零changed，累计分量约(1.397819,-1.430421)，即可触发。
这与日志提前建立touch吻合。阈值成立不代表已经找出所有调度波动的根因。

解释：越过ScrollDrag门槛时才创建touch，创建时的坐标已含累计位移；这一段不能当作touch建立后的
pan移动。下一步若要提前pan识别，应把预备过程拆成“跨门槛建touch”和“touch建立后再移动”两阶段，
并记录实际pan began，不宜继续把第一笔预备增量单纯放大。

## Pan 识别阈值的汇编核对

继续通过 Xcode LLDB 查询当前 UIKitCore，未修改 recognizer 参数：

- `+[UIPanGestureRecognizer _defaultHysteresis]` 只有返回 double 10 的指令。
- `-[UIPanGestureRecognizer _hysteresis]` 读取实例值并乘以 `_touchSloppinessFactor`，
  另有离散滚动缩放分支。因此默认常量不能独自代表所有输入环境的有效值。
- 对 Sample 当前 TracedPan 实例直接求值 `_hysteresis`，得到 **10**（本次为空闲 Possible 状态）。
- `_willScrollX` / `_willScrollY` 分别以 (1,0)/(0,1) 调用
  `_translationDistanceInSceneInSelfAxis:`，然后与 `_hysteresis` 比较，`b.ge` 即达到或超过。
  方法内部包含坐标转换，因此这里是 recognizer 的坐标距离，不能直接叫 CGEvent 字段单位或截图像素。
- `_shouldTryToBeginWithEvent:` 还检查触点数量并调用方向/委托判定；超过距离门槛不代表
  任意应用都会立刻触发 began，还可能受其它识别条件和手势竞争影响。

这确认了“创建 touch 的轴向 1”与“pan 的有效 hysteresis 10”是不同层的条件。
首次 pan 回调可能已经超过门槛；离散触摸更新只会在某次更新跨过门槛后触发，
不能把日志中第一次 began 的实际位移直接当作固定阈值。
下一轮两阶段预备应在 touch 建立后再跨过 pan 门槛，并用日志验证实际 began，
而不是把 touch 创建前的大增量也算进 pan 距离。

本地证据：build/pan-detail-*.asm、pan-gate-*.asm、pan-selector-*.txt、
pan-runtime-hysteresis.txt。以上仅对应当前系统构建和 Sample 配置。

## 两阶段预备：先建立 touch，再触发 pan

新增诊断变量 `CUA_GESTURE_PAN_PRIME=N`：需要 changed/both 模式、start-hold>=100ms，
在60ms发送第二个非零changed；从后续主移动扣除，保持总字段位移不变。
N为沿运动方向的欧氏长度（CGEvent字段单位），不是轴向长度或截图px。
不设置时没有此事件，默认行为不变。预备事件的CGEvent位置仍固定from，位移由118/119携带。

本次输入(343,-351)截图px，对应字段总和(171.5,-175.5)，40步主移动400ms。
时间表（相对发送器起点）：

- 10ms：began，长度1 seed，轴分量约(0.699,-0.715)。
- 20ms：第一段changed，长度1，累计超过touch门槛。
- 60ms：第二段changed，分别测试无此事件、长度10、长度15。
- 110～500ms：40个正式changed；540ms ended，包含40ms尾部停留。

先交错跑三组各5次，再对15组补5次。每次截图确认重置到中心。

|第二段长度|正式移动前pan began|截图水平位移px|
|---|---|---|
|无|0/5|170.247～248.828|
|10|0/5|238.585～244.886|
|15|10/10|215.057～245.801|

15组第二段轴分量约(10.484,-10.728)，10组仅(6.989,-7.152)，结果符合按轴门槛。
15组pan began相对接收began事件时间约42.98～59.26ms，全部早于首个正式移动事件。
这证明本组两阶段预备可以重复提前识别pan，不能据10次就声称永久可靠，也未解决最终落点波动。
15组所有发送/接收字段X总和约171.5，tail均0；circle水平位移每档约差3.990464 UIKit points，
恰与正式主移动单步X量相符。说明剩余问题不能只归因于pan开始边界；仍需定位主移动到touch坐标
之间发生的差异。尚未直接记录内部累计向量，不能据此断言具体丢弃发生在哪一层。

复现单次（先单点中心重置并等动画完成；此配置总时长540ms，不是总时长400ms）：

```sh
CUA_GESTURE_PRELUDE=changed CUA_GESTURE_PRIME=1 CUA_GESTURE_PAN_PRIME=15 \
  ./build/cua-shot --app TouchAlternativesSample --gesture \
  --from-x 1024 --from-y 832 --to-x 1367 --to-y 481 \
  --steps 40 --duration-ms 540 --gesture-easing linear \
  --gesture-start-hold-ms 100 --gesture-end-hold-ms 40
```

本地数据：build/twostage_trials.py、twostage_more.py、twostage-*.csv、
twostage-summary.jsonl、move-study/twostage-*.png。测试覆盖第二段事件数与总位移守恒；构建和gesture_smoke通过。

## 进一步对齐：丢失量与 ScrollDrag 外部 dequeue 完全对应

使用 `analyze_demux.py RECEIVER_JSONL SENDER_CSV...` 按 scrollDrag.before/exit 同步调用边界
划分 subtype6 的 queue.dequeue。对两阶段15组全部10次：

- pan began位置均完全相同：(676.816522264,486.542722809)。
- 最后pan changed与touch ended坐标相同，但不同试次之间不相同。
- 全部44个subtype6都被App取到，X总和均171.499997。
- 第1/2/4/5/9次所有非零translation均在ScrollDrag调用内部dequeue，pan末X=836.435062074。
- 第3/6/7/8/10次分别有2/3/4/1/5个主移动translation在ScrollDrag调用外部dequeue。
  它们的X增量和分别为7.980927/11.971390/15.961854/3.990463/19.952317，
  与相对于完整试次的末坐标短缺量一致（浮点精度内）。

具体第10次seq19543～19547：ScrollWheel取出→进入ScrollDrag→退出ScrollDrag→随后才取出
subtype6 changed（dx=3.990463495）。该增量不在本次ScrollDrag内部读取循环中。
汇编+536～+768的确是在scrollWheel内再次从App队列取22/29事件，只有在这个循环读到subtype6
才把118/119加到内部向量。由此强烈支持：ScrollWheel与translation独立投递存在时序窗口，
部分translation被外层事件循环取走，未进入此累计路径。日志尚未记录dequeue调用栈/模式，
也未直接记录内部向量，因此精确的外层消费者与调度原因仍待证实。

结论：Sample的begin与last-changed差值公式没有矛盾；发送端固定to点不保证接收端touch终点固定。
本组剩余差异可被上述内部/外部取队列边界解释，不能再只称为泛泛的步数/缓动不稳定。
下一步优先给nextEvent日志补mode和调用来源标记、给ScrollDrag补真实累计向量；随后做小规模
事件配对/投递顺序对照，验证能否让每个非零translation进入内部读取循环。不要先靠补大终点掩盖短缺。
本地摘要：build/twostage-demux-summary.jsonl。

## 投递顺序与预先准备事件的对照

调研 Apple 的 CGEvent/CGEventPost/CGEventTapPostEvent 文档以及本机 SDK CGEvent.h、
SkyLight.tbd：公开接口按单个CGEvent投递，未找到可直接使用的原子批量入队接口。
CGEventTapPostEvent 文档明确描述插入事件与回调返回事件的顺序，但没有批量原子性保证；
本轮没有实现event tap方案，也不能据此断言所有私有接口都没有批量能力。
参考：
- https://developer.apple.com/documentation/coregraphics/cgevent
- https://developer.apple.com/documentation/coregraphics/cgevent/tappostevent(_:)

新增仅诊断使用 `CUA_GESTURE_ORDER=SGT|STG|GST|TGS|ST`：
S=ScrollWheel，G=普通Gesture，T=subtype6位移Gesture。
所有事件及元数据先准备完毕，再按指定顺序连续调用现有SLEventPostToPid；非原子操作。
未设置时保留原始的创建/填写/发送交错顺序。
测试使用此前两阶段预备（prime1、pan-prime15、start100、end40、总540ms、40步），
总位移、时间表不变。每组先交错测试5次，再给ST补10次。

|方案|所有非零增量均进入ScrollDrag内部且有pan|结果|
|---|---|---|
|原始发送|4/5|一次漏1个主移动增量|
|预先准备SGT|4/5|一次漏1个|
|预先准备STG|4/5|一次漏2个|
|预先准备GST|4/5|一次漏3个|
|预先准备TGS|0/5|没有形成pan，截图位移为0|
|预先准备ST|14/15|第6次漏6个，其余14次同一落点|

完整落点截图位移为(245.801,-251.555)px；ST失败次为(208.893,-213.808)px。
这排除了“倒序投递就能解决”的简单方案；缩短间隔/删去普通Gesture仍未消除时序窗口。
小样本、非同时运行且带hook，不应用这些比例推断真实长期失败率。
完整性依据analyze_demux.py和截图双重核对；新增事件顺序/事件数/位移守恒测试通过。
本地资料：build/order_trials.py、order_more.py、order-summary.jsonl、order-trace-path.txt。

### 接收端等待2ms的诊断对照

仅在build复制的Sample中把`QueueTrace.preScrollDelayUS`设为2000，在原始scrollWheel IMP前usleep；
发送端使用原始顺序，时间表/位移与上表一致。10次中9次所有非零translation在内部消费，
第7次仍有1个主增量在外部取走；对应末点少3.990463 UIKit X单位。
10次pan began均相同，9次日志中的最后pan changed和circleAfter相同。
因此固定延时仍不能保证配对，不能视作已解决；这一干预也只适用于可修改的Sample。
截图有8次同一落点，第7次与漏增量一致，第9次截图偏差但日志circleAfter完全正确，
该截图异常尚未解释，不能据此再推断输入损失。其余判定以接收日志为准。
本地证据：build/wait2_trials.py、wait2-summary.jsonl、wait2-trace-path.txt。
诊断结束后源码和运行Sample均恢复preScrollDelayUS=0；保留显式诊断开关，不改变默认行为。

后续应优先研究有条件的接收端配对（或可靠的一次提交机制），而不是无限增加固定sleep。
截至上述顺序实验，尚未实现接收端缓存/配对器；下一节记录后续实现和验证。

## 有条件配对：先送 T，再送 S，由 Sample 缓存后交给 ScrollDrag

仅在复制的 Sample 中启用 `QueueTrace.pairEvents`，仓库里的诊断源码默认 `false`。
`cua-shot` 的合成事件带 `kCGEventSourceUserData` 标记；钩子只截取该标记的 subtype6 T，
在外层队列提前缓存，收到同组 S 后，在 ScrollDrag 内部的 `nextEvent` 调用中返回该 T。
用 phase 和 NSEvent.timestamp 校验配对（允许 T 比 S 早 0.5ms）；过期或重复 T 记为
`pair.stale` / `pair.orphan`，不回填至队列。若 S 先到，诊断器等待最多 50ms 寻找 T。
这会改变 Sample 的事件投递语义，仅用于验证机制；**没有移植到 cua-shot 或游戏中**。

初版只按“下一个 T”配对，产生错位：上次结束的 T 配给本次 began，后续 T 整体落后一帧。
增加时间/阶段校验后，另一缺陷是旧 T 重新放入延迟队列形成死循环；现已修正，并加入
0.5 秒定时落盘以便无 touch 时也能读到队列日志。上述失败试次不计入下表。

同一参数：prime=1、pan-prime=15、start-hold=100ms、end-hold=40ms、40 步、总 540ms，
目标截图位移 `(245.801,-251.555)` px。每次先点击中心复位并验证截图中心；启用配对时
使用普通前台点击复位，因为合成 gesture-click 也会受到配对钩子影响。

| 接收端配对 | 投递顺序 | 试次 | 到达目标 | 接收端证据 |
|---|---|---:|---:|---|
| 开 | T→G→S | 10 | 10 | 每次 44/44 个 S 配到 T；43 个非零 T 全在 ScrollDrag 内消费，外部 0；pan 起点、终点完全相同 |
| 开 | 原始 S→G→T | 5 | 0 | 每次观察窗口内均无 pan；S 先到时等待超时，T 随后才出现在外层队列；50ms 等待还拖慢整次处理 |
| 关 | T→G→S | 5 | 0 | 每次 43 个 T 在 ScrollDrag 外部消费，无 pan；4 次截图为中心，1 次截图轻微偏离但日志无 pan，原因未确定 |
| 关 | 原始 S→G→T | 3 | 2 | 失败次一个 changed T 在外部消费，末点短缺正好为其 `(3.990463,-4.083536)` UIKit 增量 |

启用配对的 10 次 pan 均从 `(676.816522,486.542723)` 到
`(836.435062,323.201296)` UIKit 点；接收端内部 T 总和
`(171.499997,-175.500003)`。截图位移与之对应。说明当前 Sample 中，
**T 先进入外层队列并在 S 处理期间定向交给 ScrollDrag** 可以消除本组试验的漏增量。
单独倒转投递顺序会让 T 在外层被消费，仍无法建立 pan。

这些数据只证明可修改 Sample、这组参数和此系统版本中的机制。配对器使用私有类与
方法替换，50ms 超时及简单 phase/时间戳条件没有并发手势隔离，也没有在 arknights.app 中
验证。总时长为 540ms，因此不代表 400ms 已稳定，更不能据此推断未修改游戏可做接收端配对。
本地证据：`build/paired_trials.py`、`build/paired-*.csv`、`build/unpaired-*.csv`、
`build/move-study/{paired,unpaired}-*.png`、对应 `input-trace-*.jsonl`；用
`analyze_demux.py` 对齐发送端 CSV 和接收端日志。

## 仅在输入端配对：逐步暂停目标进程

新增诊断变量 `CUA_GESTURE_GATE=step`。每一步在目标进程暂停期间连续投递
ScrollWheel、普通 Gesture 和 subtype6 位移 Gesture，然后立即恢复目标进程；
下一步仍按原时间表发送。此法不修改接收端，也不依赖 Sample 的配对钩子。
目前用 SIGSTOP/SIGCONT 和发送前 1ms 等待实现，属于实验方案；若发送进程遭到强制
终止，目标可能停留在暂停状态，需要向目标 PID 发送 SIGCONT。连接 Xcode 调试器
也可能拦截停止信号，测试时关闭了 scheme 的 Debug executable。

在未启用接收端配对的 Sample 中，用上述两阶段预备、40 步、总 540ms 参数测试：
10/10 次截图位移均为 `(245.801,-251.555)` px；pan began 和最后 changed
坐标每次相同。每次 43 个非零 subtype6 位移事件都在 ScrollDrag 调用内部取出，
外部取出数为 0，累计字段位移约为 `(171.5,-175.5)`。整次暂停后统一投递的
对照试验没有形成 pan；关键是每一步独立恢复，让接收端逐步处理。

```sh
CUA_GESTURE_GATE=step CUA_GESTURE_PRELUDE=changed \
CUA_GESTURE_PRIME=1 CUA_GESTURE_PAN_PRIME=15 \
./build/cua-shot --app TouchAlternativesSample --gesture \
  --from-x 1024 --from-y 832 --to-x 1367 --to-y 481 \
  --steps 40 --duration-ms 540 --gesture-easing linear \
  --gesture-start-hold-ms 100 --gesture-end-hold-ms 40
```

本地证据：`build/gated-step-*.csv`、`build/gated-base-*.csv`、
`build/gated_probe.py`、接收端 JSONL 和对应截图。10 次是本机、此 Sample、
此参数下的重复性证据，不能直接推出游戏中任意起终点都可重复。
