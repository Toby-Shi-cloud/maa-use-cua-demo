#pragma once

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

namespace macos_drag {

enum class Button { left, right, middle };

namespace detail {

using SkyLightPostToPid = void (*)(pid_t, CGEventRef);
using SetWindowLocation = void (*)(CGEventRef, double, double);

inline SkyLightPostToPid sky_light_post_to_pid() {
    static SkyLightPostToPid post = [] {
        void* framework = dlopen(
            "/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
            RTLD_LAZY | RTLD_GLOBAL);
        if (!framework) return static_cast<SkyLightPostToPid>(nullptr);
        return reinterpret_cast<SkyLightPostToPid>(dlsym(framework, "SLEventPostToPid"));
    }();
    return post;
}

inline SetWindowLocation set_window_location() {
    static SetWindowLocation set = [] {
        void* framework = dlopen(
            "/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
            RTLD_LAZY | RTLD_GLOBAL);
        if (!framework) return static_cast<SetWindowLocation>(nullptr);
        return reinterpret_cast<SetWindowLocation>(dlsym(framework, "CGEventSetWindowLocation"));
    }();
    return set;
}

inline void post(CGEventRef event, pid_t pid) {
    if (auto sky_light = sky_light_post_to_pid()) sky_light(pid, event);
    // AppKit targets receive the public route, while Chromium/Catalyst targets
    // often require the SkyLight route above. Posting both mirrors klyk's
    // background event strategy and keeps the fallback useful on older macOS.
    CGEventPostToPid(pid, event);
}

inline void set_field(CGEventRef event, CGEventField field, int64_t value) {
    CGEventSetIntegerValueField(event, field, value);
}

inline void stamp(CGEventRef event, pid_t pid, uint32_t window_id,
                  CGPoint local, int64_t click_group, int64_t click_state,
                  int64_t button_number) {
    // CGEventCreateMouseEvent already carries screen coordinates. The private
    // window-local stamp is optional; never replace the screen point with the
    // local point when the SPI is absent.
    if (auto set_location = set_window_location()) set_location(event, local.x, local.y);
    set_field(event, kCGEventTargetUnixProcessID, pid);
    set_field(event, kCGMouseEventClickState, click_state);
    set_field(event, kCGMouseEventButtonNumber, button_number);
    set_field(event, kCGMouseEventSubtype, 0);
    set_field(event, kCGMouseEventWindowUnderMousePointer, window_id);
    set_field(event, kCGMouseEventWindowUnderMousePointerThatCanHandleThisEvent, window_id);
    // kCGMouseEventWindowNumber is private; this is the documented event
    // field used by WindowServer for the target window (same as CUA).
    set_field(event, static_cast<CGEventField>(51), window_id);
    set_field(event, kCGMouseEventClickState, click_state);
    // Field 58 is the click-group ID used by AppKit/Chromium to associate the
    // down, dragged, and up events with one gesture.
    set_field(event, static_cast<CGEventField>(58), click_group);
}

inline void sleep_ms(uint64_t ms) {
    if (ms) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace detail

// Send a complete drag in window screenshot-pixel coordinates. The caller
// supplies the WindowServer frame and capture scale established by the same
// screenshot used to choose the coordinates.
inline void drag(pid_t pid, uint32_t window_id, double bounds_x, double bounds_y,
                 double screenshot_scale, double from_x, double from_y,
                 double to_x, double to_y, uint64_t duration_ms, uint64_t steps,
                 Button button) {
    if (pid <= 0 || window_id == 0) throw std::runtime_error("Invalid drag target.");
    if (!std::isfinite(screenshot_scale) || screenshot_scale <= 0)
        throw std::runtime_error("Invalid screenshot scale.");
    if (steps == 0 || steps > 200) throw std::runtime_error("Drag steps must be 1..200.");

    CGMouseButton cg_button = kCGMouseButtonLeft;
    CGEventType down_type = kCGEventLeftMouseDown;
    CGEventType dragged_type = kCGEventLeftMouseDragged;
    CGEventType up_type = kCGEventLeftMouseUp;
    int64_t button_number = 0;
    switch (button) {
        case Button::left: break;
        case Button::right:
            cg_button = kCGMouseButtonRight;
            down_type = kCGEventRightMouseDown;
            dragged_type = kCGEventRightMouseDragged;
            up_type = kCGEventRightMouseUp;
            button_number = 1;
            break;
        case Button::middle:
            cg_button = kCGMouseButtonCenter;
            down_type = kCGEventOtherMouseDown;
            dragged_type = kCGEventOtherMouseDragged;
            up_type = kCGEventOtherMouseUp;
            button_number = 2;
            break;
    }

    const double from_local_x = from_x / screenshot_scale;
    const double from_local_y = from_y / screenshot_scale;
    const double to_local_x = to_x / screenshot_scale;
    const double to_local_y = to_y / screenshot_scale;
    const CGPoint from_screen = CGPointMake(bounds_x + from_local_x, bounds_y + from_local_y);
    const CGPoint to_screen = CGPointMake(bounds_x + to_local_x, bounds_y + to_local_y);
    const uint64_t step_delay = steps > 1 ? duration_ms / steps : duration_ms;
    const int64_t click_group =
        static_cast<int64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    if (!source) throw std::runtime_error("CGEventSourceCreate failed.");
    bool button_down = false;
    auto release = [&] {
        if (!button_down) return;
        CGEventRef up = CGEventCreateMouseEvent(source, up_type, to_screen, cg_button);
        if (up) {
            detail::stamp(up, pid, window_id, CGPointMake(to_local_x, to_local_y),
                          click_group, 1, button_number);
            detail::post(up, pid);
            CFRelease(up);
        }
        button_down = false;
    };
    try {
        // Prime AppKit's cursor-tracking state before the background mouseDown.
        CGEventRef moved = CGEventCreateMouseEvent(source, kCGEventMouseMoved,
                                                    from_screen, cg_button);
        if (!moved) throw std::runtime_error("CGEventCreate mouseMoved failed.");
        detail::stamp(moved, pid, window_id, CGPointMake(from_local_x, from_local_y),
                      click_group, 0, button_number);
        detail::post(moved, pid);
        CFRelease(moved);
        detail::sleep_ms(12);

        CGEventRef down = CGEventCreateMouseEvent(source, down_type, from_screen, cg_button);
        if (!down) throw std::runtime_error("CGEventCreate mouseDown failed.");
        detail::stamp(down, pid, window_id, CGPointMake(from_local_x, from_local_y),
                      click_group, 1, button_number);
        detail::post(down, pid);
        CFRelease(down);
        button_down = true;
        detail::sleep_ms(16);

        for (uint64_t i = 1; i <= steps; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(steps);
            const double sx = from_screen.x + (to_screen.x - from_screen.x) * t;
            const double sy = from_screen.y + (to_screen.y - from_screen.y) * t;
            const double lx = from_local_x + (to_local_x - from_local_x) * t;
            const double ly = from_local_y + (to_local_y - from_local_y) * t;
            CGEventRef dragged = CGEventCreateMouseEvent(
                source, dragged_type, CGPointMake(sx, sy), cg_button);
            if (!dragged) throw std::runtime_error("CGEventCreate mouseDragged failed.");
            detail::stamp(dragged, pid, window_id, CGPointMake(lx, ly),
                          click_group, 1, button_number);
            detail::post(dragged, pid);
            CFRelease(dragged);
            detail::sleep_ms(step_delay);
        }
        detail::sleep_ms(50);
        release();
        detail::sleep_ms(100);
    } catch (...) {
        release();
        CFRelease(source);
        throw;
    }
    CFRelease(source);
}

}  // namespace macos_drag
