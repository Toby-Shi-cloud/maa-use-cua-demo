#pragma once

#include <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <thread>

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

inline void send(CGEventRef event, const Route& route, pid_t pid, uint32_t window_id,
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
    route.post(pid, event);
    CFRelease(event);
}

// Generate a fresh ScrollWheel + Gesture sequence, with no recorded event as
// a template. Mapping screen displacement to private fields is experimental.
inline void swipe(pid_t pid, uint32_t window_id, double bounds_x, double bounds_y,
                  double screenshot_scale, double from_x, double from_y,
                  double to_x, double to_y, uint64_t duration_ms, uint64_t steps) {
    if (pid <= 0 || window_id == 0 || !std::isfinite(screenshot_scale) ||
        screenshot_scale <= 0 || steps == 0 || steps > 200 || duration_ms < steps * 8)
        throw std::runtime_error("Invalid synthetic gesture parameters.");
    const Route route = resolve_route();
    const double dx = (to_x - from_x) / screenshot_scale / steps;
    const double dy = (to_y - from_y) / screenshot_scale / steps;
    const auto start = std::chrono::steady_clock::now();
    auto step = [&](uint64_t i, CGScrollPhase phase, double move_x, double move_y) {
        const CGPoint local = CGPointMake(
            (from_x + (to_x - from_x) * i / steps) / screenshot_scale,
            (from_y + (to_y - from_y) * i / steps) / screenshot_scale);
        const CGPoint screen = CGPointMake(bounds_x + local.x, bounds_y + local.y);
        send(make_scroll(nullptr, static_cast<int32_t>(std::lround(move_x)),
                         static_cast<int32_t>(std::lround(move_y)), phase),
             route, pid, window_id, local, screen);
        send(make_gesture(nullptr, false, phase, 0, 0), route, pid, window_id, local, screen);
        send(make_gesture(nullptr, true, phase, move_x, move_y), route, pid, window_id, local, screen);
    };
    for (uint64_t i = 1; i <= steps; ++i) {
        std::this_thread::sleep_until(start +
            std::chrono::nanoseconds(duration_ms * 1'000'000ULL * i / steps));
        step(i, i == 1 ? kCGScrollPhaseBegan : kCGScrollPhaseChanged, dx, dy);
    }
    step(steps, kCGScrollPhaseEnded, 0, 0);
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
