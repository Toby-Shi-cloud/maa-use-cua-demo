#pragma once

#include <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>
#include <fstream>
#include <cstdlib>
#include <string_view>

namespace macos_gesture {

// These fields and event type were observed in a working trackpad recording on
// macOS 27. They are private and may change with the OS.
constexpr CGEventType kGestureType = static_cast<CGEventType>(29);
constexpr CGEventField kGestureSubtype = static_cast<CGEventField>(110);
constexpr CGEventField kGesturePhase = static_cast<CGEventField>(132);
constexpr CGEventField kGestureTranslationFlag = static_cast<CGEventField>(135);
constexpr CGEventField kGestureField118 = static_cast<CGEventField>(118);
constexpr CGEventField kGestureField119 = static_cast<CGEventField>(119);
constexpr CGEventField kTargetWindow = static_cast<CGEventField>(51);
constexpr int64_t kSyntheticTag = 0x4D41414745535452LL; // "MAAGESTR"

using PostToPid = void (*)(pid_t, CGEventRef);
using SetWindowLocation = void (*)(CGEventRef, CGPoint);

struct Route {
    PostToPid post{};
    SetWindowLocation set_window_location{};
};

inline Route resolve_route() {
    void* framework = dlopen(
        "/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
        RTLD_LAZY | RTLD_LOCAL);
    if (!framework) throw std::runtime_error("Could not load SkyLight for synthetic gesture.");
    Route route{
        reinterpret_cast<PostToPid>(dlsym(framework, "SLEventPostToPid")),
        reinterpret_cast<SetWindowLocation>(dlsym(framework, "SLEventSetWindowLocation"))
    };
    if (!route.set_window_location)
        route.set_window_location = reinterpret_cast<SetWindowLocation>(
            dlsym(RTLD_DEFAULT, "CGEventSetWindowLocation"));
    if (!route.post || !route.set_window_location)
        throw std::runtime_error("Synthetic gesture needs SLEventPostToPid and window location SPI.");
    return route;
}

inline uint64_t timestamp_ns() {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    return static_cast<uint64_t>(
        static_cast<__uint128_t>(mach_absolute_time()) * info.numer / info.denom);
}

inline CGEventRef make_scroll(CGEventSourceRef source, int32_t dx, int32_t dy,
                              CGScrollPhase phase) {
    CGEventRef event = CGEventCreateScrollWheelEvent(
        source, kCGScrollEventUnitPixel, 2, dy, dx);
    if (!event) throw std::runtime_error("Could not create synthetic ScrollWheel.");
    CGEventSetIntegerValueField(event, kCGScrollWheelEventIsContinuous, 1);
    CGEventSetIntegerValueField(event, kCGScrollWheelEventScrollPhase, phase);
    CGEventSetIntegerValueField(event, kCGScrollWheelEventMomentumPhase, 0);
    return event;
}

inline CGEventRef make_gesture(CGEventSourceRef source, bool translation, CGScrollPhase phase,
                               double field118, double field119) {
    CGEventRef event = CGEventCreate(source);
    if (!event) throw std::runtime_error("Could not create synthetic Gesture.");
    CGEventSetType(event, kGestureType);
    if (translation) {
        // Field 110=6 makes NSEvent report subtype 6, as in the real sequence.
        CGEventSetIntegerValueField(event, kGestureSubtype, 6);
        // The recorded translation events use 132=1/2/4 for begin/change/end
        // and 135=1 throughout. Without a phase, AppKit reports phase 0.
        CGEventSetIntegerValueField(event, kGesturePhase, phase);
        CGEventSetIntegerValueField(event, kGestureTranslationFlag, 1);
        CGEventSetDoubleValueField(event, kGestureField118, field118);
        CGEventSetDoubleValueField(event, kGestureField119, field119);
    }
    return event;
}

inline void prepare(CGEventRef event, const Route& route, pid_t pid, uint32_t window_id,
                 CGPoint local, CGPoint screen) {
    CGEventSetLocation(event, screen);
    route.set_window_location(event, local);
    CGEventSetIntegerValueField(event, kCGEventTargetUnixProcessID, pid);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, pid);
    CGEventSetIntegerValueField(event, kTargetWindow, window_id);
    CGEventSetIntegerValueField(event, kCGMouseEventWindowUnderMousePointer, window_id);
    CGEventSetIntegerValueField(
        event, kCGMouseEventWindowUnderMousePointerThatCanHandleThisEvent, window_id);
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, kSyntheticTag);
    CGEventSetTimestamp(event, timestamp_ns());
}

inline void send(CGEventRef event, const Route& route, pid_t pid, uint32_t window_id,
                 CGPoint local, CGPoint screen) {
    prepare(event, route, pid, window_id, local, screen);
    route.post(pid, event);
    CFRelease(event);
}

// Generate a fresh ScrollWheel + Gesture sequence, with no recorded event as
// a template. Mapping screen displacement to private fields is experimental.
enum class Easing { linear, smoothstep };

inline double progress(double t, Easing easing) {
    return easing == Easing::smoothstep ? t * t * (3 - 2 * t) : t;
}

inline void swipe_with_route(const Route& route, pid_t pid, uint32_t window_id, double bounds_x, double bounds_y,
                  double screenshot_scale, double from_x, double from_y,
                  double to_x, double to_y, uint64_t duration_ms, uint64_t steps,
                  Easing easing = Easing::linear, uint64_t end_hold_ms = 0, uint64_t start_hold_ms = 0) {
    if (pid <= 0 || window_id == 0 || !std::isfinite(screenshot_scale) ||
        screenshot_scale <= 0 || steps == 0 || steps > 200 || end_hold_ms > duration_ms ||
        start_hold_ms > duration_ms - end_hold_ms ||
        duration_ms - end_hold_ms - start_hold_ms < steps * 8)
        throw std::runtime_error("Invalid synthetic gesture parameters.");
    // Diagnostic-only prelude matrix; default path stays unchanged.
    const char* prelude_env = std::getenv("CUA_GESTURE_PRELUDE");
    const std::string_view prelude = prelude_env ? prelude_env : "";
    if (!prelude.empty() && (start_hold_ms < 30 ||
        (prelude != "control" && prelude != "changed" && prelude != "maybegin" && prelude != "both")))
        throw std::runtime_error("Diagnostic prelude requires start hold >=30ms and control/changed/maybegin/both.");
    const char* order_env = std::getenv("CUA_GESTURE_ORDER");
    const std::string_view order = order_env ? order_env : "";
    if (!order.empty() && order != "SGT" && order != "STG" && order != "GST" &&
        order != "TGS" && order != "ST")
        throw std::runtime_error("Diagnostic order must be SGT/STG/GST/TGS/ST.");
    const char* gate_env = std::getenv("CUA_GESTURE_GATE");
    const std::string_view gate = gate_env ? gate_env : "";
    if (!gate.empty() && gate != "step")
        throw std::runtime_error("Diagnostic gate must be step.");
    const double dx = (to_x - from_x) / screenshot_scale;
    const double dy = (to_y - from_y) / screenshot_scale;
    const double distance = std::hypot(dx, dy);
    const double seed_fraction = start_hold_ms && distance > 0 ? std::min(1.0, distance) / distance : 0;
    const double seed_x = dx * seed_fraction, seed_y = dy * seed_fraction;
    const char* prime_env = std::getenv("CUA_GESTURE_PRIME");
    char* prime_end = nullptr;
    const double prime_units = prime_env ? std::strtod(prime_env, &prime_end) : 0;
    if (prime_env && (prime_end == prime_env || *prime_end || !std::isfinite(prime_units) ||
        prime_units < 0 || prime_units > distance * (1 - seed_fraction) ||
        (prelude != "changed" && prelude != "both")))
        throw std::runtime_error("Diagnostic prime requires changed/both prelude and a finite displacement within total distance.");
    const double prime_fraction = distance > 0 ? prime_units / distance : 0;
    const double prime_x = dx * prime_fraction, prime_y = dy * prime_fraction;
    const char* pan_env = std::getenv("CUA_GESTURE_PAN_PRIME");
    char* pan_end = nullptr;
    const double pan_units = pan_env ? std::strtod(pan_env, &pan_end) : 0;
    if (pan_env && (pan_end == pan_env || *pan_end || !std::isfinite(pan_units) ||
        pan_units < 0 || pan_units > distance * (1 - seed_fraction - prime_fraction) ||
        start_hold_ms < 100 || (prelude != "changed" && prelude != "both")))
        throw std::runtime_error("Diagnostic pan prime requires changed/both, start hold >=100ms, and displacement within remaining distance.");
    const double pan_fraction = distance > 0 ? pan_units / distance : 0;
    const double pan_x = dx * pan_fraction, pan_y = dy * pan_fraction;
    struct TraceRow { uint64_t step, before, after; int phase; double dx, dy; };
    const char* trace_path = std::getenv("CUA_GESTURE_TRACE");
    std::vector<TraceRow> trace;
    if (trace_path) trace.reserve(steps + 4);
    const auto start = std::chrono::steady_clock::now();
    auto step = [&](uint64_t i, CGScrollPhase phase, double move_x, double move_y) {
        const double p = progress(static_cast<double>(i) / steps, easing);
        const double location_fraction = i == 0 ? 0 : seed_fraction + prime_fraction + pan_fraction + (1 - seed_fraction - prime_fraction - pan_fraction) * p;
        const CGPoint local = CGPointMake(
            from_x / screenshot_scale + dx * location_fraction,
            from_y / screenshot_scale + dy * location_fraction);
        const CGPoint screen = CGPointMake(bounds_x + local.x, bounds_y + local.y);
        const uint64_t before = trace_path ? timestamp_ns() : 0;
        struct ScopedStop {
            pid_t pid;
            bool active;
            ScopedStop(pid_t target, bool enabled) : pid(target), active(enabled) {
                if (active) {
                    if (kill(pid, SIGSTOP) != 0)
                        throw std::runtime_error("Could not stop target for diagnostic gesture step.");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            ~ScopedStop() { if (active) kill(pid, SIGCONT); }
            void resume() {
                if (active) {
                    if (kill(pid, SIGCONT) != 0)
                        throw std::runtime_error("Could not resume target after diagnostic gesture step.");
                    active = false;
                }
            }
        } stopped(pid, gate == "step");
        if (!order.empty()) {
            // Prepare all metadata before posting; posting itself is still not atomic.
            CGEventRef events[] = {
                make_scroll(nullptr, static_cast<int32_t>(std::lround(move_x)),
                            static_cast<int32_t>(std::lround(move_y)), phase),
                make_gesture(nullptr, false, phase, 0, 0),
                make_gesture(nullptr, true, phase, move_x, move_y)
            };
            for (auto event : events) prepare(event, route, pid, window_id, local, screen);
            for (char type : order) route.post(pid, events[type == 'S' ? 0 : type == 'G' ? 1 : 2]);
            for (auto event : events) CFRelease(event);
        } else {
        send(make_scroll(nullptr, static_cast<int32_t>(std::lround(move_x)),
                         static_cast<int32_t>(std::lround(move_y)), phase),
             route, pid, window_id, local, screen);
        send(make_gesture(nullptr, false, phase, 0, 0), route, pid, window_id, local, screen);
        send(make_gesture(nullptr, true, phase, move_x, move_y), route, pid, window_id, local, screen);
        }
        stopped.resume();
        if (trace_path) trace.push_back({i, before, timestamp_ns(), static_cast<int>(phase), move_x, move_y});
    };
    // Send begin at from before moving. steps counts movement updates;
    // optional start hold uses a one-unit seed, deducted from later displacement.
    if (!prelude.empty()) {
        if (prelude == "maybegin" || prelude == "both") {
            const CGPoint local = CGPointMake(from_x / screenshot_scale, from_y / screenshot_scale);
            const CGPoint screen = CGPointMake(bounds_x + local.x, bounds_y + local.y);
            const auto before = timestamp_ns();
            send(make_scroll(nullptr, 0, 0, static_cast<CGScrollPhase>(8)), route, pid, window_id, local, screen);
            send(make_gesture(nullptr, false, static_cast<CGScrollPhase>(128), 0, 0), route, pid, window_id, local, screen);
            send(make_gesture(nullptr, true, static_cast<CGScrollPhase>(128), 0, 0), route, pid, window_id, local, screen);
            if (trace_path) trace.push_back({0, before, timestamp_ns(), 128, 0, 0});
        }
        // All diagnostic arms use the same begin time and main-movement schedule.
        std::this_thread::sleep_until(start + std::chrono::milliseconds(10));
    }
    step(0, kCGScrollPhaseBegan, seed_x, seed_y);
    if (prelude == "changed" || prelude == "both") {
        std::this_thread::sleep_until(start + std::chrono::milliseconds(20));
        step(0, kCGScrollPhaseChanged, prime_x, prime_y);
    }
    if (pan_env) {
        std::this_thread::sleep_until(start + std::chrono::milliseconds(60));
        step(0, kCGScrollPhaseChanged, pan_x, pan_y);
    }
    for (uint64_t i = 1; i <= steps; ++i) {
        std::this_thread::sleep_until(start +
            std::chrono::nanoseconds(start_hold_ms * 1'000'000ULL +
                (duration_ms - end_hold_ms - start_hold_ms) * 1'000'000ULL * i / steps));
        const double fraction = progress(static_cast<double>(i) / steps, easing) -
            progress(static_cast<double>(i - 1) / steps, easing);
        // Keep the original linear arithmetic (including rounding at half pixels).
        step(i, kCGScrollPhaseChanged,
             easing == Easing::linear ? (dx - seed_x - prime_x - pan_x) / steps : (dx - seed_x - prime_x - pan_x) * fraction,
             easing == Easing::linear ? (dy - seed_y - prime_y - pan_y) / steps : (dy - seed_y - prime_y - pan_y) * fraction);
    }
    std::this_thread::sleep_until(start + std::chrono::milliseconds(duration_ms));
    step(steps, kCGScrollPhaseEnded, 0, 0);
    if (trace_path) {
        std::ofstream output(trace_path);
        if (!output) throw std::runtime_error("Gesture sent but trace file could not be opened.");
        output.precision(17);
        output << "step,before_ns,after_ns,phase,dx,dy\n";
        for (const auto& r : trace)
            output << r.step << ',' << r.before << ',' << r.after << ',' << r.phase
                   << ',' << r.dx << ',' << r.dy << '\n';
        if (!output) throw std::runtime_error("Gesture sent but trace file write failed.");
    }
}

inline void swipe(pid_t pid, uint32_t window_id, double bounds_x, double bounds_y,
                  double screenshot_scale, double from_x, double from_y,
                  double to_x, double to_y, uint64_t duration_ms, uint64_t steps,
                  Easing easing = Easing::linear, uint64_t end_hold_ms = 0, uint64_t start_hold_ms = 0) {
    swipe_with_route(resolve_route(), pid, window_id, bounds_x, bounds_y,
                     screenshot_scale, from_x, from_y, to_x, to_y, duration_ms, steps, easing, end_hold_ms, start_hold_ms);
}

// Experimental tap candidate. A one-point translation is enough to give the
// ScrollDrag module a nonzero gesture while keeping the contact almost stationary.
// Only the target application's response can establish whether it is a tap.
inline void tap_with_route(const Route& route, pid_t pid, uint32_t window_id,
                           CGPoint local, CGPoint screen, uint64_t duration_ms) {
    send(make_scroll(nullptr, 1, 0, kCGScrollPhaseBegan),
         route, pid, window_id, local, screen);
    send(make_gesture(nullptr, false, kCGScrollPhaseBegan, 0, 0),
         route, pid, window_id, local, screen);
    send(make_gesture(nullptr, true, kCGScrollPhaseBegan, 1, 0),
         route, pid, window_id, local, screen);
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    send(make_scroll(nullptr, 0, 0, kCGScrollPhaseEnded),
         route, pid, window_id, local, screen);
    send(make_gesture(nullptr, false, kCGScrollPhaseEnded, 0, 0),
         route, pid, window_id, local, screen);
    send(make_gesture(nullptr, true, kCGScrollPhaseEnded, 0, 0),
         route, pid, window_id, local, screen);
}

inline void tap(pid_t pid, uint32_t window_id, double bounds_x, double bounds_y,
                double screenshot_scale, double x, double y, uint64_t duration_ms) {
    if (pid <= 0 || window_id == 0 || !std::isfinite(bounds_x) ||
        !std::isfinite(bounds_y) || !std::isfinite(screenshot_scale) ||
        screenshot_scale <= 0 || !std::isfinite(x) || !std::isfinite(y) ||
        x < 0 || y < 0 || duration_ms < 20 || duration_ms > 500)
        throw std::runtime_error("Invalid synthetic gesture tap parameters.");
    const CGPoint local = CGPointMake(x / screenshot_scale, y / screenshot_scale);
    const CGPoint screen = CGPointMake(bounds_x + local.x, bounds_y + local.y);
    tap_with_route(resolve_route(), pid, window_id, local, screen, duration_ms);
}

} // namespace macos_gesture
