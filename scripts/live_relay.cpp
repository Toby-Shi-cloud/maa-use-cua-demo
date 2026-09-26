#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <mach/mach_time.h>

namespace {

using SLEventPostToPidFn = void (*)(pid_t, CGEventRef);
using SLEventSetWindowLocationFn = void (*)(CGEventRef, CGPoint);
using SLEventGetDoubleValueFieldFn = double (*)(CGEventRef, int64_t);

constexpr int64_t kRelayTag = 0x4D414152454C4159LL; // "MAARELAY"
constexpr CGEventField kCGEventTargetWindowPrivate = static_cast<CGEventField>(51);
// AppKit's NSEventTypeGesture is 29. CGEventTypes.h does not publish a
// corresponding kCGEventGesture; whether a session tap sees it is experimental.
constexpr CGEventType kObservedGestureType = static_cast<CGEventType>(29);
constexpr CGEventField kGestureDeltaXPrivate = static_cast<CGEventField>(118);
constexpr CGEventField kGestureDeltaYPrivate = static_cast<CGEventField>(119);

enum class Mode { observe, scroll, sequence, record, replay, inspect };

constexpr char kRecordMagic[8] = {'M', 'A', 'A', 'G', 'E', 'S', 'T', '2'};
constexpr uint32_t kMaxEventBytes = 8 * 1024 * 1024;
constexpr uint64_t kMaxRecordingNs = 60'000'000'000ULL;
struct RecordedEvent {
    uint64_t offset_ns{};
    uint32_t type{};
    double gesture_x{};
    double gesture_y{};
    std::vector<uint8_t> bytes;
};

struct RelayContext {
    Mode mode{Mode::scroll};
    pid_t pid{};
    CGWindowID window_id{};
    CGRect window_bounds{};
    CGPoint local_point{};
    SLEventPostToPidFn post_to_pid{};
    SLEventSetWindowLocationFn set_window_location{};
    SLEventGetDoubleValueFieldFn get_double_value_field{};
    CFMachPortRef tap{};
    uint64_t observed{};
    uint64_t relayed{};
    std::ofstream recording;
    bool recording_valid{true};
    std::chrono::steady_clock::time_point first_arrival{};

    // Leave this false first: the point of the experiment is to preserve as
    // much of the real trackpad event's provenance as possible.
    // If the target receives nothing at all, try true; some SkyLight recipes
    // require source PID == target PID for routing.
    bool stamp_source_pid = true;
};

std::optional<CGRect> window_bounds(CGWindowID wanted) {
    CFArrayRef infos = CGWindowListCopyWindowInfo(
        kCGWindowListOptionIncludingWindow, wanted);
    if (!infos) return std::nullopt;

    std::optional<CGRect> result;
    const CFIndex count = CFArrayGetCount(infos);
    for (CFIndex i = 0; i < count; ++i) {
        auto dict = static_cast<CFDictionaryRef>(
            const_cast<void*>(CFArrayGetValueAtIndex(infos, i)));
        auto number = static_cast<CFNumberRef>(
            CFDictionaryGetValue(dict, kCGWindowNumber));
        int64_t wid = 0;
        if (!number || !CFNumberGetValue(number, kCFNumberSInt64Type, &wid) ||
            static_cast<CGWindowID>(wid) != wanted) {
            continue;
        }

        auto bounds_dict = static_cast<CFDictionaryRef>(
            CFDictionaryGetValue(dict, kCGWindowBounds));
        CGRect r{};
        if (bounds_dict && CGRectMakeWithDictionaryRepresentation(bounds_dict, &r)) {
            result = r;
            break;
        }
    }

    CFRelease(infos);
    return result;
}

bool resolve_skylight(RelayContext& ctx) {
    void* sl = dlopen(
        "/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
        RTLD_LAZY | RTLD_LOCAL);
    if (!sl) return false;

    ctx.post_to_pid = reinterpret_cast<SLEventPostToPidFn>(
        dlsym(sl, "SLEventPostToPid"));

    ctx.set_window_location = reinterpret_cast<SLEventSetWindowLocationFn>(
        dlsym(sl, "SLEventSetWindowLocation"));

    // Some macOS builds expose the CoreGraphics re-export instead.
    if (!ctx.set_window_location) {
        ctx.set_window_location = reinterpret_cast<SLEventSetWindowLocationFn>(
            dlsym(RTLD_DEFAULT, "CGEventSetWindowLocation"));
    }

    ctx.get_double_value_field = reinterpret_cast<SLEventGetDoubleValueFieldFn>(
        dlsym(sl, "SLEventGetDoubleValueField"));

    return ctx.post_to_pid && ctx.set_window_location;
}

bool is_gesture(CGEventType type) { return type == kObservedGestureType; }
bool is_scroll(CGEventType type) { return type == kCGEventScrollWheel; }

template <typename T>
void write_number(std::ofstream& stream, T value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
T read_number(std::ifstream& stream) {
    T value{};
    if (!stream.read(reinterpret_cast<char*>(&value), sizeof(value)))
        throw std::runtime_error("Truncated recording");
    return value;
}

std::vector<RecordedEvent> read_recording(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open recording");
    char magic[sizeof(kRecordMagic)]{};
    if (!file.read(magic, sizeof(magic)) ||
        std::memcmp(magic, kRecordMagic, sizeof(magic)) != 0)
        throw std::runtime_error("Unknown recording format");

    std::vector<RecordedEvent> events;
    size_t adjusted_offsets = 0;
    uint64_t largest_regression_ns = 0;
    while (file.peek() != EOF) {
        if (events.size() >= 10000) throw std::runtime_error("Too many recorded events");
        RecordedEvent record;
        record.offset_ns = read_number<uint64_t>(file);
        record.type = read_number<uint32_t>(file);
        const uint32_t length = read_number<uint32_t>(file);
        record.gesture_x = read_number<double>(file);
        record.gesture_y = read_number<double>(file);
        if (length == 0 || length > kMaxEventBytes ||
            record.offset_ns > kMaxRecordingNs ||
            !std::isfinite(record.gesture_x) || !std::isfinite(record.gesture_y) ||
            (!is_scroll(static_cast<CGEventType>(record.type)) &&
             !is_gesture(static_cast<CGEventType>(record.type))))
            throw std::runtime_error("Invalid event length, type or timing");
        // Older recordings used CGEvent timestamps. ScrollWheel and Gesture
        // callbacks can arrive in order with slightly out-of-order timestamps.
        // Keep callback order and make the replay schedule nondecreasing.
        if (!events.empty() && record.offset_ns < events.back().offset_ns) {
            largest_regression_ns = std::max(largest_regression_ns,
                                             events.back().offset_ns - record.offset_ns);
            record.offset_ns = events.back().offset_ns;
            ++adjusted_offsets;
        }
        record.bytes.resize(length);
        if (!file.read(reinterpret_cast<char*>(record.bytes.data()), length))
            throw std::runtime_error("Truncated event data");
        CFDataRef data = CFDataCreate(kCFAllocatorDefault, record.bytes.data(), length);
        CGEventRef event = data ? CGEventCreateFromData(kCFAllocatorDefault, data) : nullptr;
        if (data) CFRelease(data);
        if (!event) throw std::runtime_error("Cannot decode recorded CGEvent");
        const bool same_type = static_cast<uint32_t>(CGEventGetType(event)) == record.type;
        if (same_type && is_gesture(static_cast<CGEventType>(record.type))) {
            CGEventSetDoubleValueField(event, kGestureDeltaXPrivate, record.gesture_x);
            CGEventSetDoubleValueField(event, kGestureDeltaYPrivate, record.gesture_y);
            if (CGEventGetDoubleValueField(event, kGestureDeltaXPrivate) != record.gesture_x ||
                CGEventGetDoubleValueField(event, kGestureDeltaYPrivate) != record.gesture_y) {
                CFRelease(event);
                throw std::runtime_error("Gesture fields cannot be restored");
            }
        }
        CFRelease(event);
        if (!same_type) throw std::runtime_error("Recorded event type changed on decode");
        events.push_back(std::move(record));
    }
    if (events.empty()) throw std::runtime_error("Recording contains no gesture events");
    if (adjusted_offsets)
        std::cerr << "Normalized " << adjusted_offsets
                  << " replay offsets after source timestamp regressions (largest adjustment "
                  << largest_regression_ns << " ns); callback order preserved.\n";
    return events;
}

uint64_t current_event_timestamp() {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    return static_cast<uint64_t>(
        static_cast<__uint128_t>(mach_absolute_time()) * info.numer / info.denom);
}

void log_event(RelayContext& ctx, CGEventType type, CGEventRef event,
               const char* action) {
    const CGPoint location = CGEventGetLocation(event);
    std::cerr << std::fixed << std::setprecision(3)
              << "[" << ++ctx.observed << "] " << action
              << " type=" << static_cast<unsigned>(type)
              << " timestamp=" << CGEventGetTimestamp(event)
              << " loc=(" << location.x << "," << location.y << ")"
              << " sourcePID=" << CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID)
              << " field7=" << CGEventGetIntegerValueField(event, kCGMouseEventSubtype);
    if (is_scroll(type)) {
        std::cerr << " continuous=" << CGEventGetIntegerValueField(event, kCGScrollWheelEventIsContinuous)
                  << " phase=" << CGEventGetIntegerValueField(event, kCGScrollWheelEventScrollPhase)
                  << " momentum=" << CGEventGetIntegerValueField(event, kCGScrollWheelEventMomentumPhase)
                  << " axis1=" << CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1)
                  << " axis2=" << CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis2);
    }
    if (is_gesture(type)) {
        const auto get = [&](CGEventField field) {
            return ctx.get_double_value_field
                ? ctx.get_double_value_field(event, static_cast<int64_t>(field))
                : CGEventGetDoubleValueField(event, field);
        };
        std::cerr << " field118=" << get(kGestureDeltaXPrivate)
                  << " field119=" << get(kGestureDeltaYPrivate);
    }
    std::cerr << '\n';
}

void stamp_destination(CGEventRef e, RelayContext& ctx) {
    const CGPoint screen = CGPointMake(
        ctx.window_bounds.origin.x + ctx.local_point.x,
        ctx.window_bounds.origin.y + ctx.local_point.y);

    // Do not recreate the scroll event: mutate a copy of the real event.
    CGEventSetLocation(e, screen);
    ctx.set_window_location(e, ctx.local_point);

    CGEventSetIntegerValueField(e, kCGEventTargetUnixProcessID, ctx.pid);
    if (ctx.stamp_source_pid) {
        CGEventSetIntegerValueField(e, kCGEventSourceUnixProcessID, ctx.pid);
    }

    CGEventSetIntegerValueField(e, kCGEventTargetWindowPrivate, ctx.window_id);
    CGEventSetIntegerValueField(
        e, kCGMouseEventWindowUnderMousePointer, ctx.window_id);
    CGEventSetIntegerValueField(
        e, kCGMouseEventWindowUnderMousePointerThatCanHandleThisEvent,
        ctx.window_id);

    // Prevent a forwarded event from being relayed again if it happens to
    // re-enter the session event stream.
    CGEventSetIntegerValueField(e, kCGEventSourceUserData, kRelayTag);
}

double gesture_value(const RelayContext& ctx, CGEventRef event, CGEventField field) {
    return ctx.get_double_value_field
        ? ctx.get_double_value_field(event, static_cast<int64_t>(field))
        : CGEventGetDoubleValueField(event, field);
}

void stop_bad_recording(RelayContext& ctx, const char* reason) {
    ctx.recording_valid = false;
    std::cerr << "Recording stopped: " << reason << '\n';
    // Leave an invalid record marker so --inspect/--replay reject a valid prefix.
    write_number<uint64_t>(ctx.recording, 0);
    write_number<uint32_t>(ctx.recording, 0);
    write_number<uint32_t>(ctx.recording, 0);
    write_number<double>(ctx.recording, 0);
    write_number<double>(ctx.recording, 0);
    ctx.recording.flush();
    if (ctx.tap) CGEventTapEnable(ctx.tap, false);
    CFRunLoopStop(CFRunLoopGetCurrent());
}

void record_event(RelayContext& ctx, CGEventType type, CGEventRef event) {
    CFDataRef data = CGEventCreateData(kCFAllocatorDefault, event);
    if (!data) { stop_bad_recording(ctx, "CGEventCreateData failed"); return; }
    CGEventRef decoded = CGEventCreateFromData(kCFAllocatorDefault, data);
    bool preserved = decoded && CGEventGetType(decoded) == type;
    const double gesture_x = is_gesture(type) ? gesture_value(ctx, event, kGestureDeltaXPrivate) : 0;
    const double gesture_y = is_gesture(type) ? gesture_value(ctx, event, kGestureDeltaYPrivate) : 0;
    if (preserved && is_scroll(type)) {
        preserved = CGEventGetIntegerValueField(decoded, kCGScrollWheelEventScrollPhase) ==
                    CGEventGetIntegerValueField(event, kCGScrollWheelEventScrollPhase);
    }
    if (preserved && is_gesture(type)) {
        preserved = std::isfinite(gesture_x) && std::isfinite(gesture_y);
        if (preserved) {
            CGEventSetDoubleValueField(decoded, kGestureDeltaXPrivate, gesture_x);
            CGEventSetDoubleValueField(decoded, kGestureDeltaYPrivate, gesture_y);
            preserved = std::abs(gesture_value(ctx, decoded, kGestureDeltaXPrivate) - gesture_x) <= 1e-9 &&
                        std::abs(gesture_value(ctx, decoded, kGestureDeltaYPrivate) - gesture_y) <= 1e-9;
        }
    }
    if (!preserved) {
        std::cerr << "Round-trip mismatch: type " << static_cast<unsigned>(type)
                  << " -> " << (decoded ? static_cast<unsigned>(CGEventGetType(decoded)) : 0)
                  << " field118 " << (is_gesture(type) ? gesture_value(ctx, event, kGestureDeltaXPrivate) : 0)
                  << " -> " << (decoded && is_gesture(type) ? gesture_value(ctx, decoded, kGestureDeltaXPrivate) : 0)
                  << " field119 " << (is_gesture(type) ? gesture_value(ctx, event, kGestureDeltaYPrivate) : 0)
                  << " -> " << (decoded && is_gesture(type) ? gesture_value(ctx, decoded, kGestureDeltaYPrivate) : 0)
                  << '\n';
    }
    if (decoded) CFRelease(decoded);
    if (!preserved || CFDataGetLength(data) <= 0 ||
        CFDataGetLength(data) > kMaxEventBytes) {
        CFRelease(data);
        stop_bad_recording(ctx, "flattened event lost type, phase or Gesture fields");
        return;
    }
    // Different event types can carry timestamps from slightly different
    // points in the input pipeline. Use callback arrival time for scheduling.
    const auto arrival = std::chrono::steady_clock::now();
    if (ctx.observed == 0) ctx.first_arrival = arrival;
    const uint64_t offset = std::chrono::duration_cast<std::chrono::nanoseconds>(
        arrival - ctx.first_arrival).count();
    if (offset > kMaxRecordingNs || ctx.observed >= 10000) {
        CFRelease(data);
        stop_bad_recording(ctx, "60-second / 10000-event limit reached");
        return;
    }
    const uint32_t type_number = static_cast<uint32_t>(type);
    const uint32_t length = static_cast<uint32_t>(CFDataGetLength(data));
    write_number(ctx.recording, offset);
    write_number(ctx.recording, type_number);
    write_number(ctx.recording, length);
    write_number(ctx.recording, gesture_x);
    write_number(ctx.recording, gesture_y);
    ctx.recording.write(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), length);
    ctx.recording.flush();
    CFRelease(data);
    if (!ctx.recording) { stop_bad_recording(ctx, "file write failed"); return; }
    log_event(ctx, type, event, "recorded");
}

void replay_recording(RelayContext& ctx, const std::vector<RecordedEvent>& events) {
    const auto start = std::chrono::steady_clock::now();
    const uint64_t timestamp_base = current_event_timestamp();
    for (size_t i = 0; i < events.size(); ++i) {
        const auto& record = events[i];
        std::this_thread::sleep_until(start + std::chrono::nanoseconds(record.offset_ns));
        CFDataRef data = CFDataCreate(kCFAllocatorDefault, record.bytes.data(), record.bytes.size());
        CGEventRef event = data ? CGEventCreateFromData(kCFAllocatorDefault, data) : nullptr;
        if (data) CFRelease(data);
        if (!event) throw std::runtime_error("Failed to decode event during replay");
        if (is_gesture(static_cast<CGEventType>(record.type))) {
            CGEventSetDoubleValueField(event, kGestureDeltaXPrivate, record.gesture_x);
            CGEventSetDoubleValueField(event, kGestureDeltaYPrivate, record.gesture_y);
        }
        CGEventSetTimestamp(event, timestamp_base + record.offset_ns);
        stamp_destination(event, ctx);
        ctx.post_to_pid(ctx.pid, event);
        ++ctx.relayed;
        log_event(ctx, static_cast<CGEventType>(record.type), event, "replay-attempt");
        CFRelease(event);
    }
    std::cerr << "Replay attempted " << ctx.relayed
              << " events; verify the target application's response.\n";
}

CGEventRef relay_callback(CGEventTapProxy,
                          CGEventType type,
                          CGEventRef event,
                          void* user_info) {
    auto* ctx = static_cast<RelayContext*>(user_info);

    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput) {
        if (ctx && ctx->tap) CGEventTapEnable(ctx->tap, true);
        return event;
    }

    if (!ctx) return event;

    if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == kRelayTag) {
        return event;
    }

    if (ctx->mode == Mode::observe) {
        log_event(*ctx, type, event, "observed");
        return event;
    }

    if (!is_scroll(type) && !is_gesture(type)) return event;

    if (ctx->mode == Mode::record) {
        if (is_gesture(type) || CGEventGetIntegerValueField(
                event, kCGScrollWheelEventIsContinuous) != 0)
            record_event(*ctx, type, event);
        return event;
    }

    if (is_gesture(type) && ctx->mode != Mode::sequence) return event;

    // For this experiment we only want trackpad-like continuous scrolling.
    // A physical mouse wheel normally has this field == 0.
    if (is_scroll(type) &&
        CGEventGetIntegerValueField(event, kCGScrollWheelEventIsContinuous) == 0) {
        return event;
    }

    CGEventRef copy = CGEventCreateCopy(event);
    if (!copy) {
        log_event(*ctx, type, event, "copy-failed");
        return event;
    }

    log_event(*ctx, type, event, "relay-attempt");
    stamp_destination(copy, *ctx);
    ctx->post_to_pid(ctx->pid, copy);
    ++ctx->relayed;
    CFRelease(copy);

    // Active event tap: swallow the original scroll so the foreground app
    // doesn't scroll as well. This also makes the experiment unambiguous.
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    Mode mode = Mode::scroll;
    std::string recording_path;
    int first = 1;
    if (argc > 1 && argv[1][0] == '-') {
        std::string_view flag(argv[1]);
        if (flag == "--observe") mode = Mode::observe;
        else if (flag == "--scroll-only") mode = Mode::scroll;
        else if (flag == "--sequence") mode = Mode::sequence;
        else if (flag == "--record" || flag == "--replay" || flag == "--inspect") {
            if (argc < 3 || argv[2][0] == '\0') {
                std::cerr << flag << " requires a recording path.\n";
                return 2;
            }
            mode = flag == "--record" ? Mode::record :
                   flag == "--replay" ? Mode::replay : Mode::inspect;
            recording_path = argv[2];
            ++first;
        }
        else {
            std::cerr << "Unknown mode: " << flag << '\n';
            return 2;
        }
        ++first;
    }
    const int positional = argc - first;
    if ((mode == Mode::observe || mode == Mode::record || mode == Mode::inspect)
            ? positional != 0
            : (positional != 2 && positional != 4)) {
        std::cerr
            << "Usage: " << argv[0]
            << " [--scroll-only|--sequence] <pid> <window_id> [local_x local_y]\n"
            << "       " << argv[0] << " --observe\n"
            << "       " << argv[0] << " --record FILE.cgevents\n"
            << "       " << argv[0] << " --inspect FILE.cgevents\n"
            << "       " << argv[0] << " --replay FILE.cgevents <pid> <window_id> [local_x local_y]\n"
            << "  local_x/local_y are window-local points (not Retina pixels).\n"
            << "  If omitted, the target point is the window center.\n"
            << "  --observe [no target needed] logs raw CGEvent types unchanged.\n"
            << "  --scroll-only preserves the previous scroll-only relay (default).\n"
            << "  --sequence attempts scroll + type-29 relay in callback order.\n";
        return 2;
    }

    RelayContext ctx;
    ctx.mode = mode;
    std::vector<RecordedEvent> recorded_events;
    if (mode == Mode::replay || mode == Mode::inspect) {
        try { recorded_events = read_recording(recording_path); }
        catch (const std::exception& e) {
            std::cerr << "Invalid recording: " << e.what() << '\n';
            return 1;
        }
        std::cerr << "Validated " << recorded_events.size() << " events, duration "
                  << recorded_events.back().offset_ns / 1e9 << " s.\n";
        if (mode == Mode::inspect) {
            for (const auto& item : recorded_events)
                std::cout << "type=" << item.type << " at=" << item.offset_ns
                          << " ns bytes=" << item.bytes.size()
                          << " field118=" << item.gesture_x
                          << " field119=" << item.gesture_y << '\n';
            return 0;
        }
    }
    if (positional > 0) {
        ctx.pid = static_cast<pid_t>(std::strtol(argv[first], nullptr, 10));
        ctx.window_id = static_cast<CGWindowID>(std::strtoul(argv[first + 1], nullptr, 10));
        if (ctx.pid <= 0 || ctx.window_id == 0) {
            std::cerr << "PID and window ID must be positive.\n";
            return 2;
        }

        auto bounds = window_bounds(ctx.window_id);
        if (!bounds) {
            std::cerr << "Could not find CGWindowID " << ctx.window_id << "\n";
            return 1;
        }
        ctx.window_bounds = *bounds;

        if (positional == 4) {
            ctx.local_point.x = std::strtod(argv[first + 2], nullptr);
            ctx.local_point.y = std::strtod(argv[first + 3], nullptr);
        } else {
            ctx.local_point = CGPointMake(
                ctx.window_bounds.size.width / 2.0,
                ctx.window_bounds.size.height / 2.0);
        }
        if (!std::isfinite(ctx.local_point.x) || !std::isfinite(ctx.local_point.y) ||
            ctx.local_point.x < 0 || ctx.local_point.y < 0 ||
            ctx.local_point.x >= ctx.window_bounds.size.width ||
            ctx.local_point.y >= ctx.window_bounds.size.height) {
            std::cerr << "Target point is outside the window bounds.\n";
            return 2;
        }
    }

    const bool skylight_ready = resolve_skylight(ctx);
    if (mode != Mode::observe && mode != Mode::record && !skylight_ready) {
        std::cerr << "Could not resolve SLEventPostToPid / "
                     "SLEventSetWindowLocation.\n";
        return 1;
    }

    // These calls may trigger the relevant Privacy & Security prompts.
    if (mode != Mode::replay && !CGPreflightListenEventAccess()) {
        std::cerr << "Requesting event-listening permission...\n";
        CGRequestListenEventAccess();
    }
    if (mode != Mode::observe && mode != Mode::record && !CGPreflightPostEventAccess()) {
        std::cerr << "Requesting event-posting permission...\n";
        CGRequestPostEventAccess();
    }

    if (mode == Mode::replay) {
        try { replay_recording(ctx, recorded_events); }
        catch (const std::exception& e) {
            std::cerr << "Replay failed: " << e.what() << '\n';
            return 1;
        }
        return 0;
    }

    const CGEventMask mask = mode == Mode::observe ? kCGEventMaskForAllEvents :
        CGEventMaskBit(kCGEventScrollWheel) | CGEventMaskBit(kObservedGestureType);
    ctx.tap = CGEventTapCreate(
        kCGSessionEventTap,
        kCGHeadInsertEventTap,
        (mode == Mode::observe || mode == Mode::record)
            ? kCGEventTapOptionListenOnly : kCGEventTapOptionDefault,
        mask,
        relay_callback,
        &ctx);

    if (!ctx.tap) {
        std::cerr
            << "CGEventTapCreate failed. Grant Input Monitoring / Accessibility "
               "permission to this binary/Terminal, then relaunch it.\n";
        return 1;
    }

    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(
        kCFAllocatorDefault, ctx.tap, 0);
    if (!source) {
        std::cerr << "CFMachPortCreateRunLoopSource failed.\n";
        CFRelease(ctx.tap);
        return 1;
    }

    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
    CGEventTapEnable(ctx.tap, true);

    if (mode == Mode::record) {
        if (std::filesystem::exists(recording_path)) {
            std::cerr << "Recording file already exists; choose a new path.\n";
            CFRelease(source);
            CFRelease(ctx.tap);
            return 1;
        }
        ctx.recording.open(recording_path, std::ios::binary | std::ios::out);
        if (!ctx.recording) {
            std::cerr << "Cannot create recording file.\n";
            CFRelease(source);
            CFRelease(ctx.tap);
            return 1;
        }
        ctx.recording.write(kRecordMagic, sizeof(kRecordMagic));
        ctx.recording.flush();
    }

    std::cerr
        << "Live relay mode=" << (mode == Mode::observe ? "observe" :
             mode == Mode::record ? "record" : mode == Mode::sequence ? "sequence" : "scroll-only") << ".\n"
        << (positional > 0
            ? "Target PID=" + std::to_string(ctx.pid) +
              " window=" + std::to_string(ctx.window_id) +
              " local=(" + std::to_string(ctx.local_point.x) +
              ", " + std::to_string(ctx.local_point.y) + ")\n"
            : "Observing session input; no target selected.\n")
        << ((mode == Mode::observe || mode == Mode::record)
            ? "Perform one real two-finger gesture. Events pass through unchanged.\n"
            : "Move the real cursor outside the target, then perform a two-finger gesture.\n"
              "Matching original events are swallowed after copies are posted to the target.\n")
        << (mode == Mode::record ? "Recording to " + recording_path + ". Ctrl-C after the gesture ends.\n" : "")
        << "Type 29 at the CGEvent tap is experimental; inspect the log for captured events.\n"
        << "Ctrl-C to stop.\n";

    CFRunLoopRun();

    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
    CFRelease(source);
    CFRelease(ctx.tap);
    return ctx.recording_valid ? 0 : 1;
}
