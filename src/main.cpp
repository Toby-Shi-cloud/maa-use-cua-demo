#include <nlohmann/json.hpp>
#include "driver.hpp"
#include <unistd.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
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

asio::awaitable<int> run(int argc, char** argv, std::unique_ptr<Driver>& runtime) {
    std::string app, title, output;
    long long window_id = 0, pid = 0;
    bool list = false, all = false, permissions = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: cua-shot --app NAME --output FILE.png [options]\n"
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
        if (arg == "--check-permissions") { permissions = true; continue; }
        if (arg == "--list") { list = true; continue; }
        if (arg == "--all") { all = true; continue; }
        if (arg != "--app" && arg != "--title" && arg != "--window-id" &&
            arg != "--pid" && arg != "--output" && arg != "-o")
            throw std::runtime_error("Unknown option: " + arg);
        if (++i == argc || std::string(argv[i]).empty()) throw std::runtime_error("Missing value for " + arg);
        std::string value = argv[i];
        if (arg == "--app") app = value;
        else if (arg == "--title") title = value;
        else if (arg == "--window-id") window_id = positive(value);
        else if (arg == "--pid") pid = positive(value);
        else output = value;
    }
    if (!permissions && !list && (output.empty() || (app.empty() && !window_id && !pid)))
        throw std::runtime_error("Provide --app, --pid or --window-id and --output. See --help.");
    if (!permissions && !list && fs::path(output).extension() != ".png")
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
    auto selected = matches.front();
    for (const auto& w : matches) if (rank(w) > rank(selected)) selected = w;
    if (matches.size() > 1 && rank(selected) == std::numeric_limits<long long>::min())
        throw std::runtime_error("Multiple windows with unknown stacking order; use --window-id.");
    auto target = fs::absolute(output);
    fs::create_directories(target.parent_path());
    // Stage on the same filesystem, so failed capture cannot destroy an old output.
    std::string pattern = (target.parent_path() / ".cua-shot-XXXXXX").string();
    std::vector<char> temp(pattern.begin(), pattern.end());
    temp.push_back('\0');
    int fd = mkstemp(temp.data());
    if (fd < 0) throw std::runtime_error("Cannot create output temp file: " + std::string(std::strerror(errno)));
    close(fd);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove(path, ec); } } cleanup{temp.data()};
    json capture = {{"pid", selected.at("pid")}, {"window_id", selected.at("window_id")},
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
    image.close();
    fs::rename(cleanup.path, target);
    std::cout << "Saved " << target << " (" << selected.value("app_name", "")
              << ", window " << selected.at("window_id") << ")\n";
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
