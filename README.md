# 本地 MaaFramework macOS 测试

本分支的 cua-shot 通过公开 C API 调用本地已安装的 MaaFramework。
截图使用 ScreenCaptureKit，输入可选 PostToPid（后台）或 GlobalEvent（前台）。
不链接 Cua SDK，不调用旧的 macos_drag / macos_gesture 实现。
旧源码和实验记录保留作参考，不参与当前构建。

## 构建

需要 CMake 3.20+、Xcode Command Line Tools 和已经编译安装的 MaaFramework。
运行要求 macOS 14+。

~~~sh
cmake -S . -B build-maafw --fresh -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMaaFramework_DIR="<path-to-maaframework>/install/share/cmake/MaaFramework" \
  -DCMAKE_PREFIX_PATH="<path-to-maaframework>/source/MaaUtils/MaaDeps/vcpkg/installed/maa-arm64-osx"
cmake --build build-maafw -j 4
ctest --test-dir build-maafw --output-on-failure
~~~

CMake 使用 find_package(MaaFramework CONFIG REQUIRED)，链接 MaaFramework::MaaFramework。
trycua 不编译 MaaFramework 源码，也不需要单独查找 OpenCV、Boost 或 meojson。
修改 MaaFramework 后在它的仓库重新编译、安装；trycua 使用 install 中的动态库。
保留 install/bin 中的配套库，框架会从该目录加载 macOS ControlUnit。
这是本地开发构建，不是可分发安装包。--fresh 需要 CMake 3.24+；
使用较旧 CMake 时请换一个空构建目录并省略 --fresh。

## 先截图确定坐标

~~~sh
./build-maafw/cua-shot --check-permissions
./build-maafw/cua-shot --list --app Arknights
./build-maafw/cua-shot --app Arknights --output screenshots/maafw.png
~~~

终端需要录屏权限；实际发送输入还需辅助功能权限。权限检查只读，不主动弹窗。
多个窗口匹配时，用 --window-id 或 --title 缩小范围。

坐标基于本分支生成的截图，不能直接复用旧 Cua 的 Retina 截图坐标。
控制单元按窗口 points 设置截图输出尺寸，通常与旧图不同。
不裁剪标题栏，不强制 16:9。
如果使用 --max-dimension，输入命令也要使用相同值；程序将缩图坐标映射回原图。
每次运行会显示窗口 ID、库版本、截图尺寸和转换后的输入坐标。

## 点击和拖动

以下是语法示例；先根据截图调整坐标。--dry-run 只截图和校验，不发送输入。

~~~sh
./build-maafw/cua-shot --app Arknights --click --x 500 --y 400 \
  --delivery background --dry-run

./build-maafw/cua-shot --app Arknights --drag \
  --from-x 900 --from-y 400 --to-x 500 --to-y 400 \
  --duration-ms 500 --delivery background --dry-run
~~~

去掉 --dry-run 才实际发送。用相同坐标分别测试：

- --delivery background：MaaFramework PostToPid。
- --delivery foreground：MaaFramework GlobalEvent，会激活应用并使用真实鼠标，抬起后恢复光标位置。

点击和拖动调用 MaaControllerPostClickV2 / MaaControllerPostSwipeV2，插值及底层事件调度由框架负责。\n--steps 不再支持，请使用 --duration-ms 调整时长。
contact 对应鼠标左/右/中键，不是真实多指触控。--button 可选 left/right/middle。
--count 2 发送两次点击，不保证目标应用将其识别为系统双击。
--output 保存的是输入前截图；发送成功只表示接口返回成功，请再次截图确认游戏响应。
--gesture、--gesture-click 和全部 --gesture-* 参数均报错；CUA_GESTURE_* 环境变量不参与执行。

旧的 scripts/test_10x.sh 等实验脚本未迁移到此后端，不能作为本分支测试入口。
离线检查覆盖 CLI 帮助和无效参数拒绝；游戏实际响应需手动验证。

## 启动和停止游戏

先在 MaaFramework 仓库编译并安装包含 macOS 启停实现的版本。
两种操作都接收应用的 .app 路径，不是应用显示名或 Bundle ID。

~~~sh
./build-maafw/cua-shot --start-game "<path-to-game>/Arknights.app" --dry-run
./build-maafw/cua-shot --start-game "<path-to-game>/Arknights.app"
./build-maafw/cua-shot --stop-game "<path-to-game>/Arknights.app"
~~~

相对路径会转换为绝对路径；路径包含空格时请加引号。
dry-run 仅检查应用包并显示路径及 Bundle ID，不执行启停。
启停不要求已有可见窗口，不截图，不需要录屏权限，也不接受 --app 等窗口筛选参数。
停止由 MaaFramework 读取 Bundle ID，再向匹配进程发送 Command-Q；运行中的目标需要辅助功能权限。
不存在匹配的运行进程时，框架返回停止成功。命令等待框架返回结果，以退出码报告成功或失败。
