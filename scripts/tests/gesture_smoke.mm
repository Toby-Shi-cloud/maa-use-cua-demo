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
    return scroll_ok && gesture_ok && changed_ok && ended_ok && tap_ok ? 0 : 2;
}
