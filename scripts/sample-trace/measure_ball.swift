import AppKit
import Foundation

// Detect the complete blue circle across the observed screenshot color profiles.
// Use this same predicate for every configuration and retain the original PNGs.
for path in CommandLine.arguments.dropFirst() {
    guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)),
          let rep = NSBitmapImageRep(data: data), let pixels = rep.bitmapData,
          rep.bitsPerSample == 8, rep.samplesPerPixel >= 3,
          !rep.bitmapFormat.contains(.alphaFirst) else { fatalError("Unsupported bitmap: \(path)") }
    let width = rep.pixelsWide, height = rep.pixelsHigh
    let stride = rep.bytesPerRow, channels = rep.samplesPerPixel
    var sx = 0.0, sy = 0.0, count = 0
    var minX = width, maxX = 0, minY = height, maxY = 0
    for y in 0..<height {
        for x in 0..<width {
            let p = pixels + y * stride + x * channels
            let r = Int(p[0]), g = Int(p[1]), b = Int(p[2])
            if b > 200 && r < 80 && g > 60 && g < 200 && b-r > 120 && b-g > 50 {
                sx += Double(x); sy += Double(y); count += 1
                minX = min(minX, x); maxX = max(maxX, x)
                minY = min(minY, y); maxY = max(maxY, y)
            }
        }
    }
    print("\(path) size=\(width)x\(height) center=(\(sx/Double(count)),\(sy/Double(count))) bounds=(\(minX),\(minY),\(maxX),\(maxY)) n=\(count)")
}
