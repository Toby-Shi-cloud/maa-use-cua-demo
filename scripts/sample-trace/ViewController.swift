/*
Diagnostic replacement for Apple's TouchAlternativesSample ViewController.swift.
The circle movement logic is intentionally unchanged. See the sample LICENSE.txt.
*/
import UIKit
import ObjectiveC.runtime
import Darwin

private func xy(_ p: CGPoint) -> [Double] { [Double(p.x), Double(p.y)] }

// Collect in memory on the main thread. Serialize/write only after a gesture ends.
private final class InputTrace {
    static let shared = InputTrace()
    private var rows = [[String: Any]]()
    private var sequence = 0
    private let writer = DispatchQueue(label: "sample.input-trace.writer")
    let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("input-trace-\(UUID().uuidString).jsonl")
    private init() { print("INPUT_TRACE_PATH \(url.path)") }
    func add(_ kind: String, _ fields: [String: Any] = [:]) {
        sequence += 1
        var row = fields
        row["kind"] = kind
        row["seq"] = sequence
        row["uptime"] = ProcessInfo.processInfo.systemUptime
        rows.append(row)
    }
    func flushSoon() {
        DispatchQueue.main.async { self.flush() }
    }
    func flush() {
        guard !rows.isEmpty else { return }
        let batch = rows
        rows.removeAll(keepingCapacity: true)
        let file = url
        writer.async {
            do {
                var bytes = Data()
                for row in batch {
                    bytes.append(try JSONSerialization.data(withJSONObject: row, options: [.sortedKeys]))
                    bytes.append(10)
                }
                if !FileManager.default.fileExists(atPath: file.path) {
                    FileManager.default.createFile(atPath: file.path, contents: nil)
                }
                let handle = try FileHandle(forWritingTo: file)
                defer { try? handle.close() }
                try handle.seekToEnd()
                try handle.write(contentsOf: bytes)
            } catch { print("INPUT_TRACE_ERROR \(error)") }
        }
    }
}

private final class TracedPan: UIPanGestureRecognizer {
    private var gestureID = 0
    private var touchIDs = [ObjectIdentifier: Int]()
    private var nextTouchID = 0
    private func sample(_ touch: UITouch) -> [String: Any] {
        ["timestamp": touch.timestamp, "phase": touch.phase.rawValue,
         "location": xy(touch.location(in: view)),
         "previous": xy(touch.previousLocation(in: view)),
         "precise": xy(touch.preciseLocation(in: view)), "type": touch.type.rawValue]
    }
    private func record(_ kind: String, _ touches: Set<UITouch>, _ event: UIEvent) {
        for touch in touches {
            let key = ObjectIdentifier(touch)
            if touchIDs[key] == nil { nextTouchID += 1; touchIDs[key] = nextTouchID }
            var row = sample(touch)
            row["gesture"] = gestureID
            row["touch"] = touchIDs[key]!
            row["eventTimestamp"] = event.timestamp
            row["panStateBeforeSuper"] = state.rawValue
            row["coalesced"] = (event.coalescedTouches(for: touch) ?? []).map { sample($0) }
            InputTrace.shared.add(kind, row)
        }
    }
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
        gestureID += 1
        record("touch.began", touches, event)
        super.touchesBegan(touches, with: event)
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent) {
        record("touch.moved", touches, event)
        super.touchesMoved(touches, with: event)
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent) {
        record("touch.ended", touches, event)
        super.touchesEnded(touches, with: event)
        InputTrace.shared.flushSoon()
    }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent) {
        record("touch.cancelled", touches, event)
        super.touchesCancelled(touches, with: event)
        InputTrace.shared.flushSoon()
    }
    override func reset() {
        InputTrace.shared.add("pan.reset", ["gesture": gestureID, "state": state.rawValue])
        super.reset()
        touchIDs.removeAll()
        InputTrace.shared.flushSoon()
    }
}

class ViewController: UIViewController {
    @IBOutlet private var circle: UIImageView!
    private var startCircleLocation = CGPoint.zero
    private var startPanLocation = CGPoint.zero
    private var observers = [NSObjectProtocol]()
    override func viewDidLoad() {
        super.viewDidLoad()
        QueueTrace.install()
        Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in
            InputTrace.shared.flush()
        }
        circle.center = view.center
        let pan = TracedPan(target: self, action: #selector(panned(_:)))
        view.addGestureRecognizer(pan)
        let tap = UITapGestureRecognizer(target: self, action: #selector(tapped(_:)))
        view.addGestureRecognizer(tap)
        InputTrace.shared.add("session", ["bounds": xy(view.bounds.size.asPoint),
            "screenScale": view.contentScaleFactor, "os": ProcessInfo.processInfo.operatingSystemVersionString])
        for name in [UIScene.didActivateNotification, UIScene.willDeactivateNotification,
                     UIScene.didEnterBackgroundNotification] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { _ in
                InputTrace.shared.add("scene", ["notification": name.rawValue])
                InputTrace.shared.flushSoon()
            })
        }
        InputTrace.shared.flushSoon()
    }
    deinit { observers.forEach(NotificationCenter.default.removeObserver) }
    @objc func panned(_ recognizer: UIPanGestureRecognizer) {
        let location = recognizer.location(in: view)
        let before = circle.center
        switch recognizer.state {
        case .began:
            startCircleLocation = circle.center
            startPanLocation = location
        case .changed:
            circle.center = CGPoint(x: startCircleLocation.x + location.x - startPanLocation.x,
                                    y: startCircleLocation.y + location.y - startPanLocation.y)
        default: break
        }
        InputTrace.shared.add("pan.action", ["state": recognizer.state.rawValue,
            "location": xy(location), "translation": xy(recognizer.translation(in: view)),
            "velocity": xy(recognizer.velocity(in: view)), "touchCount": recognizer.numberOfTouches,
            "startPan": xy(startPanLocation), "startCircle": xy(startCircleLocation),
            "circleBefore": xy(before), "circleAfter": xy(circle.center),
            "bounds": xy(view.bounds.size.asPoint), "screenScale": view.window?.screen.scale ?? 0])
        if recognizer.state == .ended || recognizer.state == .cancelled { InputTrace.shared.flushSoon() }
    }
    @objc func tapped(_ recognizer: UITapGestureRecognizer) {
        InputTrace.shared.add("tap", ["location": xy(recognizer.location(in: view)),
            "circleBefore": xy(circle.center), "target": xy(view.center)])
        UIView.animate(withDuration: 0.25, animations: {
            self.circle.center = self.view.center
        }, completion: { finished in
            InputTrace.shared.add("tap.completed", ["finished": finished, "circle": xy(self.circle.center)])
            InputTrace.shared.flushSoon()
        })
    }
}
private extension CGSize { var asPoint: CGPoint { CGPoint(x: width, y: height) } }

// Diagnostic-only hooks. Pairing mode deliberately changes event delivery order.
private enum QueueTrace {
    typealias Next = @convention(c) (AnyObject, Selector, UInt, AnyObject?, AnyObject?, Bool) -> AnyObject?
    typealias Scroll = @convention(c) (AnyObject, Selector, AnyObject) -> Void
    typealias GetCG = @convention(c) (AnyObject, Selector) -> UnsafeRawPointer?
    typealias GetDouble = @convention(c) (UnsafeRawPointer, UInt32) -> Double
    typealias GetInteger = @convention(c) (UnsafeRawPointer, UInt32) -> Int64
    static var installed = false
    static let preScrollDelayUS: useconds_t = 0 // Diagnostic control; never enabled by default.
    static let pairEvents = false // Diagnostic only: buffer one synthetic S/T pair.
    static var pairedTranslation: AnyObject?
    static var earlyTranslation: AnyObject?
    static var pairActive = false
    static var scrollDepth = 0
    static var deferred = [AnyObject]()
    static func matches(_ translation: [String: Any]?, scroll: [String: Any]?) -> Bool {
        guard let translation, let scroll,
              let translationPhase = translation["gesturePhase"] as? Int64,
              let scrollPhase = scroll["scrollPhase"] as? Int64,
              let translationTime = translation["eventTimestamp"] as? Double,
              let scrollTime = scroll["eventTimestamp"] as? Double else { return false }
        // In TGS order the prepared translation precedes the scroll by microseconds.
        // Older translations from a previous step must never be attached to this scroll.
        return translationPhase == scrollPhase && translationTime >= scrollTime - 0.0005
    }
    static func fields(_ object: AnyObject) -> [String: Any]? {
        guard let event = object as? NSObject,
              let eventType = event.value(forKey: "type") as? NSNumber,
              eventType.intValue == 22 || eventType.intValue == 29 else { return nil }
        let selector = NSSelectorFromString("CGEvent")
        guard event.responds(to: selector), let method = class_getInstanceMethod(type(of: event), selector) else { return nil }
        let get = unsafeBitCast(method_getImplementation(method), to: GetCG.self)
        guard let cg = get(event, selector),
              let d = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "CGEventGetDoubleValueField"),
              let n = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "CGEventGetIntegerValueField") else { return nil }
        let getDouble = unsafeBitCast(d, to: GetDouble.self)
        let getInteger = unsafeBitCast(n, to: GetInteger.self)
        return ["tag": getInteger(cg, 42), "eventType": eventType.intValue,
            "eventTimestamp": event.value(forKey: "timestamp") ?? 0,
            "subtype": getInteger(cg, 110), "gesturePhase": getInteger(cg, 132),
            "scrollPhase": getInteger(cg, 99),
            "dx": getDouble(cg, 118), "dy": getDouble(cg, 119)]
    }
    static func event(_ object: AnyObject, kind: String) {
        if let row = fields(object) { InputTrace.shared.add(kind, row) }
    }
    static func values(_ object: AnyObject) -> [String: Any] {
        var result = [String: Any]()
        guard let cls = NSClassFromString("UINSGameModuleScrollDrag") else { return result }
        var count: UInt32 = 0
        guard let ivars = class_copyIvarList(cls, &count) else { return result }
        defer { free(ivars) }
        let base = Unmanaged.passUnretained(object).toOpaque()
        for i in 0..<Int(count) {
            let ivar = ivars[i]
            guard let name = ivar_getName(ivar), let code = ivar_getTypeEncoding(ivar) else { continue }
            let key = String(cString: name), encoding = String(cString: code)
            let ptr = base.advanced(by: ivar_getOffset(ivar))
            if encoding == "d" { result[key] = ptr.load(as: Double.self) }
            else if encoding == "B" { result[key] = ptr.load(as: Bool.self) }
            else if encoding == "q" { result[key] = ptr.load(as: Int64.self) }
            else if encoding == "{CGPoint=dd}" { result[key] = xy(ptr.load(as: CGPoint.self)) }
        }
        return result
    }
    static func install() {
        guard !installed else { return }; installed = true
        let nextSelector = NSSelectorFromString("nextEventMatchingMask:untilDate:inMode:dequeue:")
        if let cls = NSClassFromString("NSApplication"), let method = class_getInstanceMethod(cls, nextSelector) {
            let original = unsafeBitCast(method_getImplementation(method), to: Next.self)
            let block: @convention(block) (AnyObject, UInt, AnyObject?, AnyObject?, Bool) -> AnyObject? = { object, mask, date, mode, dequeue in
                if pairEvents && pairActive && scrollDepth > 0 && mask & (UInt(1) << 29) != 0 {
                    // Supply exactly the captured translation to the internal drain.
                    if let result = pairedTranslation {
                        if dequeue { pairedTranslation = nil }
                        event(result, kind: dequeue ? "queue.dequeue" : "queue.peek")
                        return result
                    }
                    return nil
                }
                while true {
                    let result: AnyObject?
                    if pairEvents && dequeue, let index = deferred.firstIndex(where: {
                        let type = ($0 as? NSObject)?.value(forKey: "type") as? NSNumber
                        return type.map { mask & (UInt(1) << $0.uintValue) != 0 } ?? false
                    }) { result = deferred.remove(at: index) }
                    else { result = original(object, nextSelector, mask, date, mode, dequeue) }
                    guard let result = result else { return nil }
                    let row = fields(result)
                    let synthetic = (row?["tag"] as? Int64) == 0x4D41414745535452
                    if pairEvents && dequeue && scrollDepth == 0 && synthetic {
                        let type = row?["eventType"] as? Int
                        if type == 29 && (row?["subtype"] as? Int64) == 6 {
                            if let old = earlyTranslation { event(old, kind: "pair.orphan") }
                            earlyTranslation = result
                            event(result, kind: "pair.captureEarly")
                            continue
                        }
                        if type == 22 {
                            if let early = earlyTranslation {
                                if matches(fields(early), scroll: row) { pairedTranslation = early }
                                else { event(early, kind: "pair.stale") }
                            }
                            earlyTranslation = nil
                            let deadline = Date(timeIntervalSinceNow: 0.05) as NSDate
                            while pairedTranslation == nil && deadline.timeIntervalSinceNow > 0 {
                                guard let candidate = original(object, nextSelector, UInt(1) << 29,
                                                               deadline, mode, true) else { break }
                                let f = fields(candidate)
                                if (f?["tag"] as? Int64) == 0x4D41414745535452 &&
                                   (f?["subtype"] as? Int64) == 6 && matches(f, scroll: row) {
                                    pairedTranslation = candidate
                                    event(candidate, kind: "pair.capture")
                                } else if (f?["tag"] as? Int64) == 0x4D41414745535452 &&
                                          (f?["subtype"] as? Int64) == 6 {
                                    event(candidate, kind: "pair.stale")
                                } else { deferred.append(candidate) }
                            }
                            pairActive = true
                            InputTrace.shared.add("pair.ready", ["matched": pairedTranslation != nil,
                                "scrollPhase": row?["scrollPhase"] ?? 0])
                        }
                    }
                    event(result, kind: dequeue ? "queue.dequeue" : "queue.peek")
                    return result
                }
            }
            method_setImplementation(method, imp_implementationWithBlock(block))
            InputTrace.shared.add("hook", ["method": "NSApplication.nextEvent", "installed": true])
        }
        let scrollSelector = NSSelectorFromString("scrollWheel:")
        if let cls = NSClassFromString("UINSGameModuleScrollDrag"), let method = class_getInstanceMethod(cls, scrollSelector) {
            let original = unsafeBitCast(method_getImplementation(method), to: Scroll.self)
            let block: @convention(block) (AnyObject, AnyObject) -> Void = { object, argument in
                event(argument, kind: "scrollDrag.enter")
                InputTrace.shared.add("scrollDrag.before", values(object))
                if preScrollDelayUS > 0 { usleep(preScrollDelayUS) }
                scrollDepth += 1
                original(object, scrollSelector, argument)
                scrollDepth -= 1
                if pairActive {
                    // End-phase ScrollDrag does not drain; its translation has zero delta.
                    if let unused = pairedTranslation { event(unused, kind: "pair.unused") }
                    pairedTranslation = nil
                    pairActive = false
                }
                InputTrace.shared.add("scrollDrag.exit", values(object))
            }
            method_setImplementation(method, imp_implementationWithBlock(block))
            InputTrace.shared.add("hook", ["method": "ScrollDrag.scrollWheel", "installed": true])
        }
    }
}
