#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

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

enum class Mode { observe, scroll, sequence };

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
    int first = 1;
    if (argc > 1 && argv[1][0] == '-') {
        std::string_view flag(argv[1]);
        if (flag == "--observe") mode = Mode::observe;
        else if (flag == "--scroll-only") mode = Mode::scroll;
        else if (flag == "--sequence") mode = Mode::sequence;
        else {
            std::cerr << "Unknown mode: " << flag << '\n';
            return 2;
        }
        ++first;
    }
    const int positional = argc - first;
    if (positional != 2 && positional != 4 &&
        !(mode == Mode::observe && positional == 0)) {
        std::cerr
            << "Usage: " << argv[0]
            << " [--observe|--scroll-only|--sequence] <pid> <window_id> [local_x local_y]\n"
            << "  local_x/local_y are window-local points (not Retina pixels).\n"
            << "  If omitted, the target point is the window center.\n"
            << "  --observe [no target needed] logs raw CGEvent types unchanged.\n"
            << "  --scroll-only preserves the previous scroll-only relay (default).\n"
            << "  --sequence attempts scroll + type-29 relay in callback order.\n";
        return 2;
    }

    RelayContext ctx;
    ctx.mode = mode;
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
    if (mode != Mode::observe && !skylight_ready) {
        std::cerr << "Could not resolve SLEventPostToPid / "
                     "SLEventSetWindowLocation.\n";
        return 1;
    }

    // These calls may trigger the relevant Privacy & Security prompts.
    if (!CGPreflightListenEventAccess()) {
        std::cerr << "Requesting event-listening permission...\n";
        CGRequestListenEventAccess();
    }
    if (mode != Mode::observe && !CGPreflightPostEventAccess()) {
        std::cerr << "Requesting event-posting permission...\n";
        CGRequestPostEventAccess();
    }

    const CGEventMask mask = mode == Mode::observe ? kCGEventMaskForAllEvents :
        CGEventMaskBit(kCGEventScrollWheel) | CGEventMaskBit(kObservedGestureType);
    ctx.tap = CGEventTapCreate(
        kCGSessionEventTap,
        kCGHeadInsertEventTap,
        mode == Mode::observe ? kCGEventTapOptionListenOnly : kCGEventTapOptionDefault,
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

    std::cerr
        << "Live relay mode=" << (mode == Mode::observe ? "observe" : mode == Mode::sequence ? "sequence" : "scroll-only") << ".\n"
        << (positional > 0
            ? "Target PID=" + std::to_string(ctx.pid) +
              " window=" + std::to_string(ctx.window_id) +
              " local=(" + std::to_string(ctx.local_point.x) +
              ", " + std::to_string(ctx.local_point.y) + ")\n"
            : "Observing session input; no target selected.\n")
        << (mode == Mode::observe
            ? "Perform a real two-finger gesture. Events pass through unchanged.\n"
            : "Move the real cursor outside the target, then perform a two-finger gesture.\n"
              "Matching original events are swallowed after copies are posted to the target.\n")
        << "Type 29 at the CGEvent tap is experimental; inspect the log for captured events.\n"
        << "Ctrl-C to stop.\n";

    CFRunLoopRun();

    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
    CFRelease(source);
    CFRelease(ctx.tap);
    return 0;
}
