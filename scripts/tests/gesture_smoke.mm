#import <AppKit/AppKit.h>
#include "../../src/macos_gesture.hpp"
#include <vector>

static std::vector<CGEventRef> captured;
static void capture_post(pid_t, CGEventRef event) {
    captured.push_back(CGEventCreateCopy(event));
}
static void ignore_local(CGEventRef, CGPoint) {}

int main() {
    CGEventRef scroll = macos_gesture::make_scroll(nullptr, -2, 5, kCGScrollPhaseBegan);
    const bool scroll_ok = CGEventGetType(scroll) == kCGEventScrollWheel &&
        CGEventGetIntegerValueField(scroll, kCGScrollWheelEventIsContinuous) == 1 &&
        CGEventGetIntegerValueField(scroll, kCGScrollWheelEventScrollPhase) == kCGScrollPhaseBegan;
    CFRelease(scroll);

    CGEventRef gesture = macos_gesture::make_gesture(
        nullptr, true, kCGScrollPhaseBegan, 15, -2);
    NSEvent* cocoa = [NSEvent eventWithCGEvent:gesture];
    const bool gesture_ok = CGEventGetType(gesture) == macos_gesture::kGestureType &&
        cocoa && [cocoa type] == NSEventTypeGesture && [cocoa subtype] == 6 &&
        [cocoa phase] == NSEventPhaseBegan &&
        CGEventGetDoubleValueField(gesture, macos_gesture::kGestureField118) == 15 &&
        CGEventGetDoubleValueField(gesture, macos_gesture::kGestureField119) == -2;
    CFRelease(gesture);
    gesture = macos_gesture::make_gesture(nullptr, true, kCGScrollPhaseChanged, 15, -2);
    cocoa = [NSEvent eventWithCGEvent:gesture];
    const bool changed_ok = cocoa && [cocoa phase] == NSEventPhaseChanged;
    CFRelease(gesture);
    gesture = macos_gesture::make_gesture(nullptr, true, kCGScrollPhaseEnded, 0, 0);
    cocoa = [NSEvent eventWithCGEvent:gesture];
    const bool ended_ok = cocoa && [cocoa phase] == NSEventPhaseEnded;
    CFRelease(gesture);

    const macos_gesture::Route fake_route{capture_post, ignore_local};
    macos_gesture::tap_with_route(fake_route, 123, 456,
        CGPointMake(10, 20), CGPointMake(30, 40), 0);
    bool tap_ok = captured.size() == 6;
    if (tap_ok) {
        const CGEventType types[] = {kCGEventScrollWheel, macos_gesture::kGestureType,
            macos_gesture::kGestureType, kCGEventScrollWheel,
            macos_gesture::kGestureType, macos_gesture::kGestureType};
        for (size_t i = 0; i < captured.size(); ++i) {
            tap_ok &= CGEventGetType(captured[i]) == types[i] &&
                CGEventGetIntegerValueField(captured[i], kCGEventTargetUnixProcessID) == 123 &&
                CGEventGetIntegerValueField(captured[i], macos_gesture::kTargetWindow) == 456;
        }
        NSEvent* began = [NSEvent eventWithCGEvent:captured[2]];
        NSEvent* ended = [NSEvent eventWithCGEvent:captured[5]];
        tap_ok &= [began subtype] == 6 && [began phase] == NSEventPhaseBegan &&
            [ended subtype] == 6 && [ended phase] == NSEventPhaseEnded &&
            CGEventGetDoubleValueField(captured[2], macos_gesture::kGestureField118) == 1 &&
            CGEventGetDoubleValueField(captured[5], macos_gesture::kGestureField118) == 0;
    }
    for (CGEventRef event : captured) CFRelease(event);
    captured.clear();
    macos_gesture::swipe_with_route(fake_route, 123, 456, 100, 200,
        2, 40, 80, 100, 20, 24, 3);
    bool swipe_ok = captured.size() == 15;
    double total_x = 0, total_y = 0;
    if (swipe_ok) {
        for (size_t i = 0; i < 5; ++i) {
            CGEventRef event = captured[i * 3 + 2];
            const CGPoint location = CGEventGetLocation(event);
            const size_t move = i < 4 ? i : 3;
            const int phase = i == 0 ? 1 : i == 4 ? 4 : 2;
            swipe_ok &= location.x == 120 + move * 10 &&
                location.y == 240 - move * 10 &&
                CGEventGetIntegerValueField(event, macos_gesture::kGesturePhase) == phase;
            const double dx = CGEventGetDoubleValueField(event, macos_gesture::kGestureField118);
            const double dy = CGEventGetDoubleValueField(event, macos_gesture::kGestureField119);
            swipe_ok &= std::abs(dx - (i > 0 && i < 4 ? 10 : 0)) < 1e-9 &&
                        std::abs(dy - (i > 0 && i < 4 ? -10 : 0)) < 1e-9;
            total_x += dx;
            total_y += dy;
        }
        swipe_ok &= total_x == 30 && total_y == -30;
    }
    for (CGEventRef event : captured) CFRelease(event);
    captured.clear();
    macos_gesture::swipe_with_route(fake_route, 123, 456, 100, 200,
        2, 40, 80, 100, 20, 40, 4, macos_gesture::Easing::smoothstep, 8);
    bool easing_ok = captured.size() == 18;
    const double expected[] = {0, 4.6875, 10.3125, 10.3125, 4.6875, 0};
    if (easing_ok) {
        double sum = 0;
        for (size_t i = 0; i < 6; ++i) {
            const double dx = CGEventGetDoubleValueField(captured[i * 3 + 2],
                macos_gesture::kGestureField118);
            easing_ok &= std::abs(dx - expected[i]) < 1e-9;
            sum += dx;
        }
        easing_ok &= std::abs(sum - 30) < 1e-9;
        const auto end = CGEventGetLocation(captured.back());
        easing_ok &= end.x == 150 && end.y == 210;
    }
    for (CGEventRef event : captured) CFRelease(event);
    captured.clear();
    macos_gesture::swipe_with_route(fake_route, 123, 456, 100, 200,
        2, 40, 80, 100, 20, 120, 4, macos_gesture::Easing::linear, 40, 40);
    bool seed_ok = captured.size() == 18;
    double seed_sum_x = 0, seed_sum_y = 0;
    if (seed_ok) {
        for (size_t i = 2; i < captured.size(); i += 3) {
            seed_sum_x += CGEventGetDoubleValueField(captured[i], macos_gesture::kGestureField118);
            seed_sum_y += CGEventGetDoubleValueField(captured[i], macos_gesture::kGestureField119);
        }
        const double seed_x = CGEventGetDoubleValueField(captured[2], macos_gesture::kGestureField118);
        const double seed_y = CGEventGetDoubleValueField(captured[2], macos_gesture::kGestureField119);
        seed_ok &= std::abs(std::hypot(seed_x, seed_y) - 1) < 1e-5 &&
            std::abs(seed_sum_x - 30) < 1e-5 && std::abs(seed_sum_y + 30) < 1e-5;
        seed_ok &= CGEventGetLocation(captured[2]).x == 120 &&
            CGEventGetLocation(captured.back()).x == 150;
    }
    for (CGEventRef event : captured) CFRelease(event);
    bool invalid_hold_rejected = false;
    try {
        macos_gesture::swipe_with_route(fake_route, 123, 456, 0, 0,
            2, 40, 80, 100, 20, 40, 4, macos_gesture::Easing::linear, 41);
    } catch (const std::runtime_error&) { invalid_hold_rejected = true; }
    bool prelude_ok = true;
    for (const char* mode : {"control", "changed", "maybegin", "both"}) {
        setenv("CUA_GESTURE_PRELUDE", mode, 1);
        const std::string_view name(mode);
        if (name == "changed" || name == "both") setenv("CUA_GESTURE_PRIME", "4", 1);
        else unsetenv("CUA_GESTURE_PRIME");
        if (name == "changed" || name == "both") setenv("CUA_GESTURE_PAN_PRIME", "15", 1);
        else unsetenv("CUA_GESTURE_PAN_PRIME");
        captured.clear();
        macos_gesture::swipe_with_route(fake_route, 123, 456, 100, 200,
            2, 40, 80, 100, 20, 180, 4, macos_gesture::Easing::linear, 40, 100);
        const bool may = name == "maybegin" || name == "both";
        const bool changed = name == "changed" || name == "both";
        prelude_ok &= captured.size() == 18 + (may ? 3 : 0) + (changed ? 6 : 0);
        if (may) {
            prelude_ok &= CGEventGetIntegerValueField(captured[0], kCGScrollWheelEventScrollPhase) == 8;
            prelude_ok &= CGEventGetIntegerValueField(captured[2], macos_gesture::kGesturePhase) == 128;
        }
        double total = 0;
        for (size_t i = 2; i < captured.size(); i += 3)
            total += CGEventGetDoubleValueField(captured[i], macos_gesture::kGestureField118);
        prelude_ok &= std::abs(total - 30) < 1e-5;
        for (auto event : captured) CFRelease(event);
    }
    unsetenv("CUA_GESTURE_PAN_PRIME");
    unsetenv("CUA_GESTURE_PRELUDE");
    unsetenv("CUA_GESTURE_PRIME");
    bool order_ok = true;
    for (const char* mode : {"SGT", "STG", "GST", "TGS", "ST"}) {
        setenv("CUA_GESTURE_ORDER", mode, 1);
        captured.clear();
        macos_gesture::swipe_with_route(fake_route, 123, 456, 100, 200,
            2, 40, 80, 100, 20, 32, 4);
        const std::string_view order(mode);
        order_ok &= captured.size() == 6 * order.size();
        double total = 0;
        for (size_t i = 0; i < captured.size(); ++i) {
            auto event = captured[i];
            char type = order[i % order.size()];
            order_ok &= CGEventGetType(event) == (type == 'S' ? kCGEventScrollWheel : macos_gesture::kGestureType);
            if (type == 'T') {
                order_ok &= CGEventGetIntegerValueField(event, macos_gesture::kGestureSubtype) == 6;
                total += CGEventGetDoubleValueField(event, macos_gesture::kGestureField118);
            }
            CFRelease(event);
        }
        order_ok &= std::abs(total - 30) < 1e-5;
    }
    unsetenv("CUA_GESTURE_ORDER");
    return scroll_ok && gesture_ok && changed_ok && ended_ok && tap_ok && swipe_ok &&
        easing_ok && seed_ok && invalid_hold_rejected && prelude_ok && order_ok ? 0 : 2;
}
