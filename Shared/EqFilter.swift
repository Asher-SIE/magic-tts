import Foundation

/// 方案音色均衡: 低架 250 Hz / 峰值 1.5 kHz / 高架 3.5 kHz 三个 biquad 级联,
/// 增益各 -12...+12 dB (整数 dB)。逐样本处理, 滤波状态跨块保持 — 流式渲染里
/// 一个语音段持有同一个实例 (段与段之间音色切换, 重建即状态归零)。
/// 全 0 dB 时透传, 不引入任何处理与开销。
final class Eq3Band {

    static func isNeutral(low: Int, mid: Int, high: Int) -> Bool {
        low == 0 && mid == 0 && high == 0
    }

    /// 一级 biquad (Direct Form II Transposed)。系数已按 a0 归一化。
    private struct Stage {
        var b0: Float = 0, b1: Float = 0, b2: Float = 0
        var a1: Float = 0, a2: Float = 0
        var z1: Float = 0, z2: Float = 0
    }

    private let neutral: Bool
    private var lowStage: Stage
    private var midStage: Stage
    private var highStage: Stage

    init(sampleRate: Double, low: Int, mid: Int, high: Int) {
        neutral = Self.isNeutral(low: low, mid: mid, high: high)
        lowStage = Self.lowShelf(250, gainDb: low, sampleRate: sampleRate)
        midStage = Self.peaking(1500, gainDb: mid, sampleRate: sampleRate, q: 1.0)
        highStage = Self.highShelf(3500, gainDb: high, sampleRate: sampleRate)
    }

    func process(_ x: Float) -> Float {
        if neutral { return x }
        var y = Self.run(&lowStage, x)
        y = Self.run(&midStage, y)
        return Self.run(&highStage, y)
    }

    private static func run(_ s: inout Stage, _ x: Float) -> Float {
        let y = s.b0 * x + s.z1
        s.z1 = s.b1 * x - s.a1 * y + s.z2
        s.z2 = s.b2 * x - s.a2 * y
        return y
    }

    // 系数公式取自 RBJ Audio EQ Cookbook。

    private static func lowShelf(_ f0: Double, gainDb: Int, sampleRate: Double) -> Stage {
        let (a, cw, _, alpha) = precompute(f0, gainDb: gainDb, sampleRate: sampleRate, q: 0.7)
        let sq = sqrt(a)
        let b0 = a * ((a + 1) - (a - 1) * cw + 2 * sq * alpha)
        let b1 = 2 * a * ((a - 1) - (a + 1) * cw)
        let b2 = a * ((a + 1) - (a - 1) * cw - 2 * sq * alpha)
        let a0 = (a + 1) + (a - 1) * cw + 2 * sq * alpha
        let a1 = -2 * ((a - 1) + (a + 1) * cw)
        let a2 = (a + 1) + (a - 1) * cw - 2 * sq * alpha
        return stage(b0, b1, b2, a0, a1, a2)
    }

    private static func peaking(_ f0: Double, gainDb: Int, sampleRate: Double, q: Double) -> Stage {
        let (a, cw, _, alpha) = precompute(f0, gainDb: gainDb, sampleRate: sampleRate, q: q)
        let b0 = 1 + alpha * a
        let b1 = -2 * cw
        let b2 = 1 - alpha * a
        let a0 = 1 + alpha / a
        let a1 = -2 * cw
        let a2 = 1 - alpha / a
        return stage(b0, b1, b2, a0, a1, a2)
    }

    private static func highShelf(_ f0: Double, gainDb: Int, sampleRate: Double) -> Stage {
        let (a, cw, _, alpha) = precompute(f0, gainDb: gainDb, sampleRate: sampleRate, q: 0.7)
        let sq = sqrt(a)
        let b0 = a * ((a + 1) + (a - 1) * cw + 2 * sq * alpha)
        let b1 = -2 * a * ((a - 1) + (a + 1) * cw)
        let b2 = a * ((a + 1) + (a - 1) * cw - 2 * sq * alpha)
        let a0 = (a + 1) - (a - 1) * cw + 2 * sq * alpha
        let a1 = 2 * ((a - 1) - (a + 1) * cw)
        let a2 = (a + 1) - (a - 1) * cw - 2 * sq * alpha
        return stage(b0, b1, b2, a0, a1, a2)
    }

    private static func precompute(_ f0: Double, gainDb: Int, sampleRate: Double, q: Double)
        -> (a: Double, cw: Double, sw: Double, alpha: Double) {
        let a = pow(10, Double(gainDb) / 40)
        let w0 = 2 * .pi * f0 / sampleRate
        return (a, cos(w0), sin(w0), sin(w0) / (2 * q))
    }

    private static func stage(_ b0: Double, _ b1: Double, _ b2: Double,
                              _ a0: Double, _ a1: Double, _ a2: Double) -> Stage {
        var s = Stage()
        s.b0 = Float(b0 / a0)
        s.b1 = Float(b1 / a0)
        s.b2 = Float(b2 / a0)
        s.a1 = Float(a1 / a0)
        s.a2 = Float(a2 / a0)
        return s
    }
}
