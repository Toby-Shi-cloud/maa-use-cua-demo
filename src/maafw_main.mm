#include <AppKit/AppKit.h>
#include <ApplicationServices/ApplicationServices.h>
#include <MaaFramework/MaaAPI.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

MaaStatus wait_controller(MaaController *ctrl, MaaCtrlId id) {
  // AppKit refreshes NSRunningApplication state on the main run loop.
  // Keep it running while MaaFramework performs the operation on its worker.
  for (;;) {
    const auto status = MaaControllerStatus(ctrl, id);
    if (status != MaaStatus_Pending && status != MaaStatus_Running)
      return status;
    if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false) ==
        kCFRunLoopRunFinished)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
struct Options {
  std::string app, title, output, delivery = "background", button = "left";
  std::optional<std::string> start_game, stop_game;
  uint32_t window = 0;
  int pid = 0, max_dimension = 0, duration = 500, count = 1;
  std::optional<double> x, y, fx, fy, tx, ty;
  bool list = false, all = false, permissions = false, click = false,
       drag = false, dry = false;
};
int integer(const std::string &value, int maximum, bool zero = false) {
  int n = 0;
  auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), n);
  if (error != std::errc{} || end != value.data() + value.size() ||
      n < (zero ? 0 : 1) || n > maximum)
    throw std::runtime_error("Invalid integer: " + value);
  return n;
}
double coordinate(const std::string &value) {
  double n = 0;
  auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), n);
  if (error != std::errc{} || end != value.data() + value.size() ||
      !std::isfinite(n) || n < 0)
    throw std::runtime_error("Invalid coordinate: " + value);
  return n;
}
std::string string_value(NSDictionary *dict, NSString *key) {
  NSString *value = dict[key];
  const char *utf8 = value ? value.UTF8String : nullptr;
  return utf8 ? std::string(utf8) : "";
}
struct Window {
  uint32_t id;
  int pid;
  bool visible;
  std::string app, title;
};
std::vector<Window> windows(const Options &o) {
  const auto flags =
      (o.all ? kCGWindowListOptionAll : kCGWindowListOptionOnScreenOnly) |
      kCGWindowListExcludeDesktopElements;
  CFArrayRef array = CGWindowListCopyWindowInfo(flags, kCGNullWindowID);
  if (!array)
    throw std::runtime_error("Cannot enumerate windows");
  std::vector<Window> result;
  for (NSDictionary *info in (NSArray *)array) {
    Window w{[info[(id)kCGWindowNumber] unsignedIntValue],
             [info[(id)kCGWindowOwnerPID] intValue],
             [info[(id)kCGWindowIsOnscreen] boolValue],
             string_value(info, (id)kCGWindowOwnerName),
             string_value(info, (id)kCGWindowName)};
    if (o.window && o.window != w.id)
      continue;
    if (o.pid && o.pid != w.pid)
      continue;
    if (!o.app.empty() && o.app != w.app)
      continue;
    if (!o.title.empty() && w.title.find(o.title) == std::string::npos)
      continue;
    if (!o.window && [info[(id)kCGWindowLayer] intValue] != 0)
      continue;
    result.push_back(std::move(w));
  }
  CFRelease(array);
  return result;
}
void save_png(const MaaImageBuffer *image, const std::string &output) {
  const auto path = fs::absolute(output);
  fs::create_directories(path.parent_path());
  std::string pattern = (path.parent_path() / ".cua-shot-XXXXXX.png").string();
  std::vector<char> temporary(pattern.begin(), pattern.end());
  temporary.push_back('\0');
  int fd = mkstemps(temporary.data(), 4);
  if (fd < 0)
    throw std::runtime_error("Cannot create screenshot temporary file");
  close(fd);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code e;
      fs::remove(path, e);
    }
  } cleanup{temporary.data()};
  const auto *bytes = MaaImageBufferGetEncoded(image);
  const auto size = MaaImageBufferGetEncodedSize(image);
  if (!bytes || !size)
    throw std::runtime_error("Cannot encode screenshot");
  std::ofstream file(cleanup.path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(bytes),
             static_cast<std::streamsize>(size));
  file.close();
  if (!file)
    throw std::runtime_error("Cannot write screenshot");
  fs::rename(cleanup.path, path);
}
int run(int argc, char **argv) {
  Options o;
  bool input_option = false, duration_option = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::cout
          << "Installed MaaFramework C API runner (no Cua SDK)\n"
             "cua-shot --start-game PATH.app [--dry-run]\n"
             "cua-shot --stop-game PATH.app [--dry-run]\n"
             "cua-shot --list [--app NAME] [--all]\n"
             "cua-shot --app NAME --output image.png [--max-dimension N]\n"
             "cua-shot --app NAME --click --x X --y Y [--delivery "
             "background|foreground|gesture]\n"
             "cua-shot --app NAME --drag --from-x X --from-y Y --to-x X --to-y "
             "Y\n"
             "  --delivery background: PostToPid; foreground: GlobalEvent; "
             "gesture: Gesture\n"
             "  --duration-ms N  Drag duration, 0..10000 (default 500)\n"
             "  Drag interpolation is handled by MaaFramework.\n"
             "  --button left|right|middle; --count 1|2 (click only)\n"
             "  --title TEXT --window-id ID --pid PID  Narrow window "
             "selection\n"
             "  --output/-o FILE.png  Save screenshot BEFORE input\n"
             "  --max-dimension N    Resize output; coordinates use that "
             "resized image\n"
             "  --dry-run            Capture and map coordinates, send no "
             "input\n"
             "  --check-permissions  Read permission status, no prompts\n"
             "Legacy --gesture/--gesture-click and gesture tuning flags are "
             "rejected.\n";
      return 0;
    }
    if (arg == "--list") {
      o.list = true;
      continue;
    }
    if (arg == "--all") {
      o.all = true;
      continue;
    }
    if (arg == "--check-permissions") {
      o.permissions = true;
      continue;
    }
    if (arg == "--click") {
      o.click = true;
      continue;
    }
    if (arg == "--drag") {
      o.drag = true;
      continue;
    }
    if (arg == "--dry-run") {
      o.dry = true;
      continue;
    }
    if (arg == "--start-game" || arg == "--stop-game") {
      if (++i == argc || !argv[i][0] || std::string(argv[i]).starts_with("--"))
        throw std::runtime_error("Missing .app path for " + arg);
      auto &target = arg == "--start-game" ? o.start_game : o.stop_game;
      if (target)
        throw std::runtime_error("Duplicate lifecycle action");
      target = argv[i];
      continue;
    }
    if (arg == "--steps")
      throw std::runtime_error(
          "--steps is controlled by MaaFramework; use --duration-ms");
    if (arg.starts_with("--gesture"))
      throw std::runtime_error("Legacy gestures are disabled; use --click or "
                               "--drag with MaaFramework");
    const std::vector<std::string> valued{
        "--app",           "--title",       "--output",    "-o",
        "--delivery",      "--button",      "--window-id", "--pid",
        "--max-dimension", "--duration-ms",

        "--count",         "--x",           "--y",         "--from-x",
        "--from-y",        "--to-x",        "--to-y"};
    if (std::find(valued.begin(), valued.end(), arg) == valued.end())
      throw std::runtime_error("Unknown option: " + arg);
    if (++i == argc)
      throw std::runtime_error("Missing value for " + arg);
    std::string v = argv[i];
    if (arg == "--app")
      o.app = v;
    else if (arg == "--title")
      o.title = v;
    else if (arg == "--output" || arg == "-o")
      o.output = v;
    else if (arg == "--window-id")
      o.window = integer(v, std::numeric_limits<int>::max());
    else if (arg == "--pid")
      o.pid = integer(v, std::numeric_limits<int>::max());
    else if (arg == "--max-dimension")
      o.max_dimension = integer(v, 32768, true);
    else {
      input_option = true;
      if (arg == "--delivery")
        o.delivery = v;
      else if (arg == "--button")
        o.button = v;
      else if (arg == "--duration-ms") {
        o.duration = integer(v, 10000, true);
        duration_option = true;
      } else if (arg == "--count")
        o.count = integer(v, 2);
      else if (arg == "--x")
        o.x = coordinate(v);
      else if (arg == "--y")
        o.y = coordinate(v);
      else if (arg == "--from-x")
        o.fx = coordinate(v);
      else if (arg == "--from-y")
        o.fy = coordinate(v);
      else if (arg == "--to-x")
        o.tx = coordinate(v);
      else if (arg == "--to-y")
        o.ty = coordinate(v);
    }
  }
  const bool lifecycle = o.start_game.has_value() || o.stop_game.has_value();
  if (lifecycle) {
    if (o.start_game && o.stop_game)
      throw std::runtime_error("Choose either --start-game or --stop-game");
    if (o.click || o.drag || o.list || o.permissions || input_option ||
        !o.app.empty() || !o.title.empty() || o.window || o.pid || o.all ||
        !o.output.empty() || o.max_dimension)
      throw std::runtime_error(
          "Game lifecycle actions accept only a .app path and --dry-run");
    const auto path =
        fs::absolute(fs::path(o.start_game ? *o.start_game : *o.stop_game))
            .lexically_normal();
    if (path.extension() != ".app" || !fs::is_directory(path))
      throw std::runtime_error("Expected an existing .app directory");
    const std::string intent = path.string();
    NSBundle *bundle = [NSBundle
        bundleWithPath:[NSString stringWithUTF8String:intent.c_str()]];
    if (!bundle || !bundle.bundleIdentifier)
      throw std::runtime_error("Application bundle has no Bundle ID");
    std::cout << (o.start_game ? "start_game" : "stop_game")
              << " path=" << intent
              << " bundle_id=" << bundle.bundleIdentifier.UTF8String << '\n';
    if (o.dry) {
      std::cout << "dry_run: no application started or stopped\n";
      return 0;
    }
    std::unique_ptr<MaaController, decltype(&MaaControllerDestroy)> controller(
        MaaMacOSControllerCreate(0, MaaMacOSScreencapMethod_None,
                                 MaaMacOSInputMethod_None),
        MaaControllerDestroy);
    if (!controller)
      throw std::runtime_error("MaaFramework controller creation failed");
    auto *ctrl = controller.get();
    if (wait_controller(ctrl, MaaControllerPostConnection(ctrl)) !=
        MaaStatus_Succeeded)
      throw std::runtime_error("MaaFramework connection failed");
    const auto id = o.start_game
                        ? MaaControllerPostStartApp(ctrl, intent.c_str())
                        : MaaControllerPostStopApp(ctrl, intent.c_str());
    if (wait_controller(ctrl, id) != MaaStatus_Succeeded)
      throw std::runtime_error("MaaFramework game lifecycle operation failed; "
                               "check installed library and logs");
    std::cout << (o.start_game ? "Application launch succeeded.\n"
                               : "Application stop succeeded.\n");
    return 0;
  }
  const bool input = o.click || o.drag;
  if (int(o.click) + int(o.drag) + int(o.list) + int(o.permissions) > 1)
    throw std::runtime_error("Choose one action");
  if (!input && (input_option || o.dry))
    throw std::runtime_error("Input options require --click or --drag");
  if (o.click &&
      (!o.x || !o.y || o.fx || o.fy || o.tx || o.ty || duration_option))
    throw std::runtime_error("Click requires only --x and --y coordinates");
  if (o.drag &&
      (!o.fx || !o.fy || !o.tx || !o.ty || o.x || o.y || o.count != 1))
    throw std::runtime_error(
        "Drag requires start/end coordinates and no click count");
  if (o.delivery != "background" && o.delivery != "foreground" &&
      o.delivery != "gesture")
    throw std::runtime_error(
        "Invalid --delivery; expected background, foreground or gesture");
  if (o.button != "left" && o.button != "right" && o.button != "middle")
    throw std::runtime_error("Invalid --button");
  if (!o.output.empty() && fs::path(o.output).extension() != ".png")
    throw std::runtime_error("Output must end in .png");
  if (!o.permissions && !o.list && (o.app.empty() && !o.window && !o.pid))
    throw std::runtime_error("Select --app, --window-id or --pid");
  if (!o.permissions && !o.list && !input && o.output.empty())
    throw std::runtime_error("Specify --output or an input action");
  if (o.permissions) {
    std::cout << "screen_recording=" << CGPreflightScreenCaptureAccess()
              << " accessibility=" << static_cast<bool>(AXIsProcessTrusted())
              << '\n';
    return 0;
  }
  const auto found = windows(o);
  if (o.list) {
    std::cout << "WINDOW_ID\tPID\tON_SCREEN\tAPP\tTITLE\n";
    for (const auto &w : found)
      std::cout << w.id << '\t' << w.pid << '\t' << w.visible << '\t' << w.app
                << '\t' << w.title << '\n';
    return 0;
  }
  if (found.empty())
    throw std::runtime_error("No matching window; use --list");
  if (found.size() != 1)
    throw std::runtime_error(
        "Multiple windows; narrow with --window-id or --title");
  const auto &w = found.front();
  if (!w.visible)
    throw std::runtime_error("Selected window is not visible");
  if (!CGPreflightScreenCaptureAccess())
    throw std::runtime_error(
        "Screen Recording permission is required for this terminal");
  if (input && !o.dry && !AXIsProcessTrusted())
    throw std::runtime_error("Accessibility permission is required for input");
#ifdef MaaMacOSInputMethod_Gesture
  const auto gesture_method = MaaMacOSInputMethod_Gesture;
#else
  if (input && !o.dry && o.delivery == "gesture")
    throw std::runtime_error(
        "Installed MaaFramework has no Gesture input method");
  const auto gesture_method = MaaMacOSInputMethod_None;
#endif
  const auto method = !input || o.dry ? MaaMacOSInputMethod_None
                      : o.delivery == "foreground"
                          ? MaaMacOSInputMethod_GlobalEvent
                      : o.delivery == "gesture" ? gesture_method
                                                : MaaMacOSInputMethod_PostToPid;
  std::unique_ptr<MaaController, decltype(&MaaControllerDestroy)> controller(
      MaaMacOSControllerCreate(w.id, MaaMacOSScreencapMethod_ScreenCaptureKit,
                               method),
      MaaControllerDestroy);
  if (!controller)
    throw std::runtime_error("MaaFramework controller creation failed");
  auto *ctrl = controller.get();
  auto wait = [&](MaaCtrlId id) {
    if (wait_controller(ctrl, id) != MaaStatus_Succeeded)
      throw std::runtime_error("MaaFramework controller operation failed");
  };
  bool raw_size = true;
  if (!MaaControllerSetOption(ctrl, MaaCtrlOption_ScreenshotUseRawSize,
                              &raw_size, sizeof(raw_size)))
    throw std::runtime_error("Cannot configure raw screenshot size");
  wait(MaaControllerPostConnection(ctrl));
  wait(MaaControllerPostScreencap(ctrl));
  std::unique_ptr<MaaImageBuffer, decltype(&MaaImageBufferDestroy)> image(
      MaaImageBufferCreate(), MaaImageBufferDestroy);
  if (!image || !MaaControllerCachedImage(ctrl, image.get()) ||
      MaaImageBufferIsEmpty(image.get()))
    throw std::runtime_error("MaaFramework screenshot failed");
  const int raw_width = MaaImageBufferWidth(image.get()),
            raw_height = MaaImageBufferHeight(image.get());
  if (o.max_dimension && std::max(raw_width, raw_height) > o.max_dimension) {
    const double scale =
        double(o.max_dimension) / std::max(raw_width, raw_height);
    if (!MaaImageBufferResize(
            image.get(), std::max(1, int(std::lround(raw_width * scale))),
            std::max(1, int(std::lround(raw_height * scale)))))
      throw std::runtime_error("Cannot resize screenshot");
  }
  const int width = MaaImageBufferWidth(image.get()),
            height = MaaImageBufferHeight(image.get());
  std::cout << "backend=MaaFramework version=" << MaaVersion()
            << " window=" << w.id << " delivery=" << o.delivery
            << " raw=" << raw_width << 'x' << raw_height << " image=" << width
            << 'x' << height << '\n';
  if (!o.output.empty())
    save_png(image.get(), o.output);
  if (!input)
    return 0;
  auto map = [](double value, int shown, int raw) {
    if (!std::isfinite(value) || value < 0 || value >= shown)
      throw std::runtime_error("Coordinate is outside the captured image");
    return std::min(raw - 1,
                    static_cast<int>(std::lround(value * raw / shown)));
  };
  const int x1 = map(o.click ? *o.x : *o.fx, width, raw_width);
  const int y1 = map(o.click ? *o.y : *o.fy, height, raw_height);
  const int x2 = o.drag ? map(*o.tx, width, raw_width) : x1;
  const int y2 = o.drag ? map(*o.ty, height, raw_height) : y1;
  std::cout << "controller_coordinates=" << x1 << ',' << y1 << " -> " << x2
            << ',' << y2 << '\n';
  if (o.dry) {
    std::cout << "dry_run: no input sent\n";
    return 0;
  }
  const int contact = o.button == "left" ? 0 : o.button == "right" ? 1 : 2;
  if (o.click) {
    for (int i = 0; i < o.count; ++i) {
      wait(MaaControllerPostClickV2(ctrl, x1, y1, contact, 0));
      if (i + 1 < o.count)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  } else {
    wait(
        MaaControllerPostSwipeV2(ctrl, x1, y1, x2, y2, o.duration, contact, 0));
  }
  std::cout
      << "Input submitted; verify the game response with a new screenshot.\n";
  return 0;
}
int main(int argc, char **argv) {
  @autoreleasepool {
    try {
      return run(argc, argv);
    } catch (const std::exception &e) {
      std::cerr << "error: " << e.what() << '\n';
      return 1;
    }
  }
}
