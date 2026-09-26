// Exercise Apple's CGEvent flatten/unflatten path and our on-disk reader.
#define main live_relay_cli_main
#include "../live_relay.cpp"
#undef main

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string path = argv[1];
    RelayContext context;
    context.mode = Mode::record;
    context.recording.open(path, std::ios::binary | std::ios::trunc);
    context.recording.write(kRecordMagic, sizeof(kRecordMagic));
    CGEventRef event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 1, 3);
    if (!event) return 3;
    CGEventSetIntegerValueField(event, kCGScrollWheelEventIsContinuous, 1);
    CGEventSetIntegerValueField(event, kCGScrollWheelEventScrollPhase, kCGScrollPhaseBegan);
    CGEventSetTimestamp(event, current_event_timestamp());
    const auto source_timestamp = CGEventGetTimestamp(event);
    record_event(context, kCGEventScrollWheel, event);
    CFRelease(event);
    CGEventRef gesture = CGEventCreate(nullptr);
    if (!gesture) return 8;
    CGEventSetType(gesture, kObservedGestureType);
    CGEventSetDoubleValueField(gesture, kGestureDeltaXPrivate, 2.5);
    CGEventSetDoubleValueField(gesture, kGestureDeltaYPrivate, -1.25);
    // The source timestamp moves backwards even though this callback is later.
    CGEventSetTimestamp(gesture, source_timestamp - 1'000'000);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    record_event(context, kObservedGestureType, gesture);
    CFRelease(gesture);
    context.recording.close();
    if (!context.recording_valid) return 4;
    auto records = read_recording(path);
    if (records.size() != 2 || records[0].type != kCGEventScrollWheel ||
        records[0].offset_ns != 0 || records[1].type != kObservedGestureType ||
        records[1].offset_ns == 0) return 5;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, records[0].bytes.data(),
                                  records[0].bytes.size());
    CGEventRef decoded = CGEventCreateFromData(kCFAllocatorDefault, data);
    CFRelease(data);
    if (!decoded) return 6;
    const bool okay = CGEventGetIntegerValueField(decoded, kCGScrollWheelEventScrollPhase)
        == kCGScrollPhaseBegan;
    CFRelease(decoded);
    CFDataRef gesture_data = CFDataCreate(kCFAllocatorDefault, records[1].bytes.data(),
                                          records[1].bytes.size());
    CGEventRef decoded_gesture = CGEventCreateFromData(kCFAllocatorDefault, gesture_data);
    CFRelease(gesture_data);
    if (!decoded_gesture) return 9;
    CGEventSetDoubleValueField(decoded_gesture, kGestureDeltaXPrivate, records[1].gesture_x);
    CGEventSetDoubleValueField(decoded_gesture, kGestureDeltaYPrivate, records[1].gesture_y);
    const bool gesture_okay = CGEventGetType(decoded_gesture) == kObservedGestureType &&
        CGEventGetDoubleValueField(decoded_gesture, kGestureDeltaXPrivate) == 2.5 &&
        CGEventGetDoubleValueField(decoded_gesture, kGestureDeltaYPrivate) == -1.25;
    CFRelease(decoded_gesture);
    if (!okay || !gesture_okay) return 7;

    // An older on-disk recording can contain a timestamp regression. It
    // should remain readable, with the second event scheduled after the first.
    const std::string legacy_path = path + ".legacy";
    std::ofstream legacy(legacy_path, std::ios::binary | std::ios::trunc);
    legacy.write(kRecordMagic, sizeof(kRecordMagic));
    for (size_t i = 0; i < records.size(); ++i) {
        write_number<uint64_t>(legacy, i == 0 ? 2'000'000 : 1'000'000);
        write_number<uint32_t>(legacy, records[i].type);
        write_number<uint32_t>(legacy, static_cast<uint32_t>(records[i].bytes.size()));
        write_number<double>(legacy, records[i].gesture_x);
        write_number<double>(legacy, records[i].gesture_y);
        legacy.write(reinterpret_cast<const char*>(records[i].bytes.data()),
                     records[i].bytes.size());
    }
    legacy.close();
    const auto recovered = read_recording(legacy_path);
    return recovered.size() == 2 &&
        recovered[0].offset_ns == 2'000'000 &&
        recovered[1].offset_ns == 2'000'000 ? 0 : 10;
}
