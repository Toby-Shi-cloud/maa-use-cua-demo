#include <nlohmann/json.hpp>
#include "driver.hpp"
#include "macos_drag.hpp"
#include "macos_gesture.hpp"
#include <unistd.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <cmath>
#include <optional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using nlohmann::json;
namespace fs = std::filesystem;

long long positive(const std::string& text) {
    long long value = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value <= 0)
        throw std::runtime_error("Expected a positive integer: " + text);
    return value;
}

double coordinate(const std::string& text) {
    double value = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) || value < 0)
        throw std::runtime_error("Expected a finite non-negative coordinate: " + text);
    return value;
}

long long non_negative(const std::string& text) {
    long long value = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value < 0)
        throw std::runtime_error("Expected a non-negative integer: " + text);
    return value;
}

asio::awaitable<int> run(int argc, char** argv, std::unique_ptr<Driver>& runtime) {
    std::string app, title, output, button = "left", delivery = "background";
    std::string gesture_easing = "linear";
    long long gesture_end_hold_ms = 0, gesture_start_hold_ms = 0;
    bool gesture_profile_option = false;
    std::optional<double> x, y, from_x, from_y, to_x, to_y;
    long long count = 1, max_dimension = 0, duration_ms = 500, steps = 20;
    bool click = false, gesture_click = false, drag = false, gesture = false, dry_run = false;
    bool action_option = false, button_option = false, count_option = false;
    bool duration_option = false, steps_option = false;
    long long window_id = 0, pid = 0;
    bool list = false, all = false, permissions = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: cua-shot --app NAME --output FILE.png [options]\n"
                         "       cua-shot --app NAME --click --x X --y Y [--output BEFORE.png]\n"
                         "       cua-shot --app NAME --gesture-click --x X --y Y\n"
                         "       cua-shot --app NAME --drag --from-x X --from-y Y --to-x X --to-y Y\n"
                         "       cua-shot --app NAME --gesture --from-x X --from-y Y --to-x X --to-y Y\n"
                         "  --max-dimension N Screenshot long-edge limit; 0 = native (default)\n"
                         "  --click           Click in window screenshot coordinates\n"
                         "  --gesture-click   Experimental background tap using Gesture events\n"
                         "  --drag            Background drag in screenshot coordinates\n"
                         "  --gesture         Experimental synthetic ScrollWheel + Gesture swipe\n"
                         "  --x X --y Y       Pixels from screenshot top-left (not screen coordinates)\n"
                         "  --from-x/--from-y Start point for --drag or --gesture\n"
                         "  --to-x/--to-y     End point for --drag or --gesture\n"
                         "  --duration-ms N   Motion duration (default 500); gesture-click hold (default 80)\n"
                         "  --steps N         Motion interpolation steps, 1..200 (default 20)\n"
                         "  --gesture-easing MODE linear (default) or smoothstep; gesture only\n"
                         "  --gesture-start-hold-ms N One-unit seed then wait; included in duration\n"
                         "  --gesture-end-hold-ms N Hold before ended, included in duration; gesture only\n"
                         "  --button BUTTON   left, right or middle (default left)\n"
                         "  --count N         1 or 2 (default 1)\n"
                         "  --delivery MODE   background (default) or foreground\n"
                         "  --dry-run         Capture and validate input, but do not send it\n"
                         "       cua-shot --list [--app NAME] [--all]\n"
                         "  --app NAME        Exact app name from --list (case-sensitive)\n"
                         "  --title TEXT      Window title contains TEXT\n"
                         "  --window-id ID    Select a specific window\n"
                         "  --pid PID         Filter by process ID\n"
                         "  --output, -o FILE Save PNG; parent directories are created\n"
                         "  --check-permissions  Show host OS permission status\n"
                         "  --list            Print matching windows\n"
                         "  --all             Include off-screen/minimized windows\n"
                         "Multiple matches: select the highest z_index (frontmost).\n";
            co_return 0;
        }
        if (arg == "--click") { click = true; continue; }
        if (arg == "--gesture-click") { gesture_click = true; continue; }
        if (arg == "--drag") { drag = true; continue; }
        if (arg == "--gesture") { gesture = true; continue; }
        if (arg == "--dry-run") { dry_run = true; continue; }
        if (arg == "--check-permissions") { permissions = true; continue; }
        if (arg == "--list") { list = true; continue; }
        if (arg == "--all") { all = true; continue; }
        if (arg != "--app" && arg != "--title" && arg != "--window-id" &&
            arg != "--pid" && arg != "--output" && arg != "-o" &&
            arg != "--x" && arg != "--y" && arg != "--from-x" && arg != "--from-y" &&
            arg != "--to-x" && arg != "--to-y" && arg != "--button" && arg != "--count" &&
            arg != "--delivery" && arg != "--duration-ms" && arg != "--steps" &&
            arg != "--gesture-start-hold-ms" && arg != "--gesture-easing" && arg != "--gesture-end-hold-ms" &&
            arg != "--max-dimension")
            throw std::runtime_error("Unknown option: " + arg);
        if (++i == argc || std::string(argv[i]).empty()) throw std::runtime_error("Missing value for " + arg);
        std::string value = argv[i];
        if (arg == "--app") app = value;
        else if (arg == "--title") title = value;
        else if (arg == "--window-id") window_id = positive(value);
        else if (arg == "--pid") pid = positive(value);
        else if (arg == "--max-dimension") {
            max_dimension = value == "0" ? 0 : positive(value);
            if (max_dimension > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("--max-dimension exceeds uint32 range.");
        }
        else if (arg == "--x") x = coordinate(value);
        else if (arg == "--y") y = coordinate(value);
        else if (arg == "--from-x") from_x = coordinate(value);
        else if (arg == "--from-y") from_y = coordinate(value);
        else if (arg == "--to-x") to_x = coordinate(value);
        else if (arg == "--to-y") to_y = coordinate(value);
        else if (arg == "--button") { button = value; action_option = true; button_option = true; }
        else if (arg == "--count") { count = positive(value); action_option = true; count_option = true; }
        else if (arg == "--delivery") { delivery = value; action_option = true; }
        else if (arg == "--duration-ms") { duration_ms = non_negative(value); action_option = true; duration_option = true; }
        else if (arg == "--gesture-easing") { gesture_easing = value; gesture_profile_option = true; }
        else if (arg == "--gesture-start-hold-ms") { gesture_start_hold_ms = non_negative(value); gesture_profile_option = true; }
        else if (arg == "--gesture-end-hold-ms") { gesture_end_hold_ms = non_negative(value); gesture_profile_option = true; }
        else if (arg == "--steps") { steps = positive(value); action_option = true; steps_option = true; }
        else output = value;
    }
    if (gesture_profile_option && !gesture)
        throw std::runtime_error("Gesture profile options require --gesture.");
    if (gesture_easing != "linear" && gesture_easing != "smoothstep")
        throw std::runtime_error("--gesture-easing must be linear or smoothstep.");
    if (gesture_click && !duration_option) duration_ms = 80;
    const bool motion = drag || gesture;
    const bool point_action = click || gesture_click;
    const bool input_action = point_action || motion;
    if (static_cast<int>(click) + static_cast<int>(gesture_click) +
        static_cast<int>(drag) + static_cast<int>(gesture) > 1)
        throw std::runtime_error("--click, --gesture-click, --drag and --gesture are mutually exclusive.");
    if (input_action && (list || permissions))
        throw std::runtime_error("Input actions, --list and --check-permissions are mutually exclusive.");
    if (list && permissions)
        throw std::runtime_error("--list and --check-permissions are mutually exclusive.");
    if (!input_action && (x || y || from_x || from_y || to_x || to_y || dry_run || action_option))
        throw std::runtime_error("Input options require --click, --gesture-click, --drag or --gesture.");
    if (point_action && (!x || !y))
        throw std::runtime_error("--click/--gesture-click requires both --x and --y.");
    if (point_action && (from_x || from_y || to_x || to_y || steps_option ||
                         (click && duration_option)))
        throw std::runtime_error("Motion coordinates and --steps require --drag or --gesture; --duration-ms also supports --gesture-click.");
    if (motion && (!from_x || !from_y || !to_x || !to_y))
        throw std::runtime_error("--drag/--gesture requires --from-x, --from-y, --to-x and --to-y.");
    if (motion && (x || y || count != 1 || (gesture && count_option)))
        throw std::runtime_error("Click coordinates and --count cannot be used with --drag/--gesture.");
    if (gesture && (button_option || delivery != "background" || duration_ms == 0))
        throw std::runtime_error("--gesture requires background delivery, positive duration and no --button.");
    if (gesture_click && (button_option || count_option || delivery != "background"))
        throw std::runtime_error("--gesture-click requires background delivery and no --button/--count.");
    if (button != "left" && button != "right" && button != "middle")
        throw std::runtime_error("--button must be left, right or middle.");
    if (delivery != "background" && delivery != "foreground")
        throw std::runtime_error("--delivery must be background or foreground.");
    if (count > 2) throw std::runtime_error("--count must be 1 or 2.");
    if (steps > 200) throw std::runtime_error("--steps must be between 1 and 200.");
    if (duration_ms > 10000) throw std::runtime_error("--duration-ms must be between 0 and 10000.");
    if (gesture_click && (duration_ms < 20 || duration_ms > 500))
        throw std::runtime_error("--gesture-click --duration-ms must be between 20 and 500.");
    if (gesture && (gesture_end_hold_ms > duration_ms || gesture_start_hold_ms > duration_ms - gesture_end_hold_ms ||
                    duration_ms - gesture_end_hold_ms - gesture_start_hold_ms < steps * 8))
        throw std::runtime_error("--gesture needs at least 8 ms per step excluding start/end holds; increase --duration-ms or reduce --steps.");
    if (!permissions && !list && ((!input_action && output.empty()) || (app.empty() && !window_id && !pid)))
        throw std::runtime_error("Provide --app, --pid or --window-id and either --output or an input action. See --help.");
    if (!permissions && !list && !output.empty() && fs::path(output).extension() != ".png")
        throw std::runtime_error("Output must have a .png extension.");
    json query = {{"on_screen_only", !all && !window_id}};
    if (pid) query["pid"] = pid;
    runtime = std::make_unique<Driver>();
    auto& driver = *runtime;
    if (permissions) {
        std::cout << (co_await driver.call("check_permissions", json::object())).dump(2) << '\n';
        co_return 0;
    }
    auto result = co_await driver.call("list_windows", query);
    if (!result.contains("windows") || !result["windows"].is_array())
        throw std::runtime_error("Unexpected list_windows response: " + result.dump());
    std::vector<json> matches;
    for (const auto& w : result["windows"]) {
        if (!app.empty() && w.value("app_name", "") != app) continue;
        if (!title.empty() && w.value("title", "").find(title) == std::string::npos) continue;
        if (window_id && w.at("window_id").get<long long>() != window_id) continue;
        matches.push_back(w);
    }
    if (matches.empty()) throw std::runtime_error("No matching window. Try --list --all; app must be running.");
    if (list) {
        std::cout << "WINDOW_ID\tPID\tON_SCREEN\tAPP\tTITLE\n";
        for (const auto& w : matches)
            std::cout << w.at("window_id") << '\t' << w.at("pid") << '\t'
                      << w.value("is_on_screen", false) << '\t'
                      << w.value("app_name", "") << '\t' << w.value("title", "") << '\n';
        co_return 0;
    }
    auto rank = [](const json& w) {
        return w.contains("z_index") && w["z_index"].is_number_integer()
            ? w["z_index"].get<long long>() : std::numeric_limits<long long>::min();
    };
    if (input_action && matches.size() != 1)
        throw std::runtime_error("Input action matched multiple windows; narrow with --title or --window-id.");
    auto selected = matches.front();
    for (const auto& w : matches) if (rank(w) > rank(selected)) selected = w;
    if (matches.size() > 1 && rank(selected) == std::numeric_limits<long long>::min())
        throw std::runtime_error("Multiple windows with unknown stacking order; use --window-id.");
    if (input_action && !selected.value("is_on_screen", false))
        throw std::runtime_error("Pixel input requires an on-screen window.");
    // Named session keeps this override in memory in Cua 0.28.2.
    const std::string session = "cua-shot";
    json session_args = {{"session", session}};
    co_await driver.call("start_session", session_args);
    json config_args = {{"session", session}, {"max_image_dimension", max_dimension}};
    co_await driver.call("set_config", config_args);
    auto target = output.empty() ? fs::temp_directory_path() / "cua-click.png" : fs::absolute(output);
    fs::create_directories(target.parent_path());
    // Stage on the same filesystem, so failed capture cannot destroy an old output.
    std::string pattern = (target.parent_path() / ".cua-shot-XXXXXX").string();
    std::vector<char> temp(pattern.begin(), pattern.end());
    temp.push_back('\0');
    int fd = mkstemp(temp.data());
    if (fd < 0) throw std::runtime_error("Cannot create output temp file: " + std::string(std::strerror(errno)));
    close(fd);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove(path, ec); } } cleanup{temp.data()};
    json capture = {{"session", session}, {"pid", selected.at("pid")}, {"window_id", selected.at("window_id")},
                    {"include_accessibility_tree", false}, {"include_screenshot", true}};
    capture["screenshot_out_file"] = cleanup.path.string();
    std::string response;
    try { response = (co_await driver.call("get_window_state", capture)).dump(); }
    catch (const std::exception& error) {
        throw std::runtime_error(std::string(error.what()) +
            "\nRun --check-permissions. In-process capture uses the launching host's Screen Recording permission; see README.md.");
    }
    std::ifstream image(cleanup.path, std::ios::binary);
    std::array<unsigned char, 8> signature{};
    image.read(reinterpret_cast<char*>(signature.data()), signature.size());
    constexpr std::array<unsigned char, 8> png{137, 80, 78, 71, 13, 10, 26, 10};
    if (signature != png || fs::file_size(cleanup.path) <= 8)
        throw std::runtime_error("Driver returned no PNG. Check Screen Recording permission.\n" + response);
    // PNG IHDR stores pixel dimensions as big-endian integers.
    image.seekg(16);
    std::array<unsigned char, 8> dimensions{};
    if (!image.read(reinterpret_cast<char*>(dimensions.data()), dimensions.size()))
        throw std::runtime_error("PNG is missing dimensions.");
    auto dimension = [&](int offset) {
        uint32_t value = 0;
        for (int i = offset; i < offset + 4; ++i) value = (value << 8) | dimensions[i];
        return value;
    };
    auto width = dimension(0), height = dimension(4);
    json metadata;
    if (input_action) {
        metadata = json::parse(response);
        std::cerr << (gesture_click ? "Gesture click" : gesture ? "Gesture" : drag ? "Drag" : "Click")
                  << " frame: PNG=" << width << "x" << height
                  << " window_bounds=" << metadata.value("window_bounds", json{}).dump()
                  << " backing_scale=" << metadata.value("screenshot_scale", json{}).dump()
                  << " frame_valid=" << metadata.value("screenshot_frame_valid", json{}).dump() << '\n';
    }
    if (point_action && (*x >= width || *y >= height))
        throw std::runtime_error("Click outside screenshot bounds: " + std::to_string(width) + "x" + std::to_string(height));
    if (motion && (*from_x >= width || *from_y >= height || *to_x >= width || *to_y >= height))
        throw std::runtime_error("Motion endpoint outside screenshot bounds: " + std::to_string(width) + "x" + std::to_string(height));
    image.close();
    if (!output.empty()) {
        fs::rename(cleanup.path, target);
        std::cout << "Saved " << target << " (" << selected.value("app_name", "")
                  << ", window " << selected.at("window_id") << ")\n";
    }
    if (click) {
        json request = {{"session", session}, {"pid", selected.at("pid")}, {"window_id", selected.at("window_id")},
                        {"x", *x}, {"y", *y}, {"button", button}, {"count", count},
                        {"delivery_mode", delivery}};
        if (dry_run) std::cout << "Validated click (not sent): " << request.dump() << '\n';
        else {
            auto clicked = co_await driver.call("click", request);
            std::cout << "Click response: " << clicked.dump() << '\n';
            if (clicked.value("route", "") == "accessibility" || clicked.value("path", "") == "ax")
                std::cerr << "Note: Cua delivered AXPress to an accessibility element, not a precise mouse event. "
                             "Background delivery was preserved; the UI effect is not verified.\n";
        }
    }
    if (gesture_click) {
        json request = { {"session", session}, {"pid", selected.at("pid")},
                         {"window_id", selected.at("window_id")}, {"x", *x}, {"y", *y},
                         {"duration_ms", duration_ms}, {"delivery_mode", "background"} };
        if (dry_run) {
            std::cout << "Validated synthetic gesture click (not sent): "
                      << request.dump() << '\n';
        } else {
            const auto bounds = metadata.value("window_bounds", json{});
            const double bounds_x = bounds.value("x", std::numeric_limits<double>::quiet_NaN());
            const double bounds_y = bounds.value("y", std::numeric_limits<double>::quiet_NaN());
            const double scale = metadata.value("screenshot_scale", std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(bounds_x) || !std::isfinite(bounds_y) || !std::isfinite(scale) || scale <= 0)
                throw std::runtime_error("Driver did not return a valid window frame for gesture click.");
            macos_gesture::tap(selected.at("pid").get<pid_t>(),
                selected.at("window_id").get<uint32_t>(), bounds_x, bounds_y, scale,
                *x, *y, static_cast<uint64_t>(duration_ms));
            std::cout << "Synthetic gesture click posted (effect unverified): "
                      << request.dump() << '\n';
        }
    }
    if (motion) {
        json request = {
            {"session", session}, {"pid", selected.at("pid")}, {"window_id", selected.at("window_id")},
            {"from_x", *from_x}, {"from_y", *from_y}, {"to_x", *to_x}, {"to_y", *to_y},
            {"button", button}, {"duration_ms", duration_ms}, {"steps", steps}, {"delivery_mode", delivery}
        };
        if (gesture) {
            request.erase("button");
            request["gesture_easing"] = gesture_easing;
            request["gesture_end_hold_ms"] = gesture_end_hold_ms;
            request["gesture_start_hold_ms"] = gesture_start_hold_ms;
        }
        if (dry_run) {
            std::cout << "Validated " << (gesture ? "synthetic gesture" : "drag")
                      << " (not sent): " << request.dump() << '\n';
        } else if (drag && delivery == "foreground") {
            auto dragged = co_await driver.call("drag", request);
            std::cout << "Foreground CUA drag response: " << dragged.dump() << '\n';
        } else {
            const auto bounds = metadata.value("window_bounds", json{});
            const double bounds_x = bounds.value("x", std::numeric_limits<double>::quiet_NaN());
            const double bounds_y = bounds.value("y", std::numeric_limits<double>::quiet_NaN());
            const double scale = metadata.value("screenshot_scale", std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(bounds_x) || !std::isfinite(bounds_y) || !std::isfinite(scale) || scale <= 0)
                throw std::runtime_error("Driver did not return a valid window frame for motion.");
            if (gesture) {
                macos_gesture::swipe(selected.at("pid").get<pid_t>(),
                    selected.at("window_id").get<uint32_t>(), bounds_x, bounds_y, scale,
                    *from_x, *from_y, *to_x, *to_y,
                    static_cast<uint64_t>(duration_ms), static_cast<uint64_t>(steps),
                    gesture_easing == "smoothstep" ? macos_gesture::Easing::smoothstep : macos_gesture::Easing::linear,
                    static_cast<uint64_t>(gesture_end_hold_ms), static_cast<uint64_t>(gesture_start_hold_ms));
                std::cout << "Synthetic gesture posted (effect unverified): " << request.dump() << '\n';
            } else {
                macos_drag::Button drag_button = macos_drag::Button::left;
                if (button == "right") drag_button = macos_drag::Button::right;
                else if (button == "middle") drag_button = macos_drag::Button::middle;
                macos_drag::drag(selected.at("pid").get<pid_t>(),
                                 selected.at("window_id").get<uint32_t>(), bounds_x, bounds_y, scale,
                                 *from_x, *from_y, *to_x, *to_y,
                                 static_cast<uint64_t>(duration_ms), static_cast<uint64_t>(steps), drag_button);
                std::cout << "Drag delivered in background: " << request.dump() << '\n';
            }
        }
    }
    co_return 0;
}

// Drain the runtime even when CLI work throws; destructors cannot co_await.
asio::awaitable<int> entry(int argc, char** argv) {
    std::unique_ptr<Driver> runtime;
    std::exception_ptr failure;
    int result = 1;
    try { result = co_await run(argc, argv, runtime); }
    catch (...) { failure = std::current_exception(); }
    if (runtime) {
        try { co_await runtime->shutdown(); }
        catch (...) { if (!failure) failure = std::current_exception(); }
    }
    if (failure) std::rethrow_exception(failure);
    co_return result;
}

int main(int argc, char** argv) {
    asio::io_context io;
    int exit_code = 1;
    asio::co_spawn(io, entry(argc, argv), [&](std::exception_ptr error, int result) {
        if (error) {
            try { std::rethrow_exception(error); }
            catch (const std::exception& e) { std::cerr << "Error: " << e.what() << '\n'; }
        } else exit_code = result;
    });
    io.run();
    return exit_code;
}
