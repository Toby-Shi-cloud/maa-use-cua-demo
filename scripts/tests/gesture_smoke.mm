#import <AppKit/AppKit.h>
#include "../../src/macos_gesture.hpp"

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
    return scroll_ok && gesture_ok && changed_ok && ended_ok ? 0 : 2;
}
