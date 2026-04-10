"""
SoundShop Signals — block-rate control signals for parameter automation.

Signals are composable expressions evaluated once per audio block (~100Hz).
They can drive plugin parameters, MIDI CC, and any automatable value.

Example:
    from soundshop_signals import *

    # 5Hz LFO
    lfo = sine(5.0)

    # Map to filter cutoff range
    cutoff = 2000 + 500 * lfo

    # Bind to a plugin parameter
    bind(cutoff, node_idx=1, param_idx=3)
"""

from __future__ import annotations
import math
from typing import Callable, Dict, Optional, Union, Sequence, Tuple

Number = Union[int, float]

# ==============================================================================
# Evaluation context
# ==============================================================================

class EvalContext:
    def __init__(self, sample_rate: int = 48000, block_size: int = 480,
                 beat_at_sample: Optional[Callable[[int], float]] = None):
        self.sample_rate = sample_rate
        self.block_size = block_size
        self.beat_at_sample = beat_at_sample


class ControlCache:
    """Cache values per (signal_id, block_index)."""
    def __init__(self):
        self._cache: Dict[Tuple[int, int], float] = {}

    def get(self, signal_id: int, block_index: int) -> Optional[float]:
        return self._cache.get((signal_id, block_index))

    def set(self, signal_id: int, block_index: int, value: float) -> None:
        self._cache[(signal_id, block_index)] = value

    def clear(self):
        self._cache.clear()


# ==============================================================================
# Signal base class
# ==============================================================================

class Signal:
    _next_id = 1

    def __init__(self):
        self._id = Signal._next_id
        Signal._next_id += 1

    @property
    def id(self) -> int:
        return self._id

    def at(self, sample: int, ctx: EvalContext, cache: Optional[ControlCache] = None) -> float:
        block = sample // ctx.block_size
        if cache is not None:
            hit = cache.get(self._id, block)
            if hit is not None:
                return hit
        v = float(self._eval(sample, ctx, cache))
        if cache is not None:
            cache.set(self._id, block, v)
        return v

    def _eval(self, sample: int, ctx: EvalContext, cache: Optional[ControlCache]) -> float:
        raise NotImplementedError

    def children(self) -> Tuple[Signal, ...]:
        return ()

    # Operator overloads
    def __add__(self, other): return Add(self, as_signal(other))
    def __radd__(self, other): return Add(as_signal(other), self)
    def __sub__(self, other): return Sub(self, as_signal(other))
    def __rsub__(self, other): return Sub(as_signal(other), self)
    def __mul__(self, other): return Mul(self, as_signal(other))
    def __rmul__(self, other): return Mul(as_signal(other), self)
    def __truediv__(self, other): return Div(self, as_signal(other))
    def __neg__(self): return Neg(self)
    def __abs__(self): return Rectify(self)

    # Convenience methods
    def clamp(self, lo=0.0, hi=1.0): return Clamp(self, lo, hi)
    def map_range(self, in_min, in_max, out_min, out_max): return MapRange(self, in_min, in_max, out_min, out_max)
    def smooth(self, rise_ms=10, fall_ms=10): return Smooth(self, rise_ms, fall_ms)


def as_signal(x) -> Signal:
    return x if isinstance(x, Signal) else Const(float(x))


# ==============================================================================
# Leaf signals
# ==============================================================================

class Const(Signal):
    def __init__(self, value: float):
        super().__init__()
        self.value = float(value)

    def _eval(self, sample, ctx, cache):
        return self.value


class TimeFn(Signal):
    """Arbitrary function of time in seconds: f(t) -> float"""
    def __init__(self, fn: Callable[[float], float]):
        super().__init__()
        self.fn = fn

    def _eval(self, sample, ctx, cache):
        t = sample / ctx.sample_rate
        return float(self.fn(t))


class BeatFn(Signal):
    """Arbitrary function of beat position: f(beat) -> float"""
    def __init__(self, fn: Callable[[float], float]):
        super().__init__()
        self.fn = fn

    def _eval(self, sample, ctx, cache):
        if ctx.beat_at_sample is None:
            return 0.0
        return float(self.fn(ctx.beat_at_sample(sample)))


# ==============================================================================
# Binary operations
# ==============================================================================

class BinaryOp(Signal):
    def __init__(self, a: Signal, b: Signal):
        super().__init__()
        self.a, self.b = a, b
    def children(self): return (self.a, self.b)

class Add(BinaryOp):
    def _eval(self, s, ctx, c): return self.a.at(s, ctx, c) + self.b.at(s, ctx, c)

class Sub(BinaryOp):
    def _eval(self, s, ctx, c): return self.a.at(s, ctx, c) - self.b.at(s, ctx, c)

class Mul(BinaryOp):
    def _eval(self, s, ctx, c): return self.a.at(s, ctx, c) * self.b.at(s, ctx, c)

class Div(BinaryOp):
    def _eval(self, s, ctx, c):
        d = self.b.at(s, ctx, c)
        return self.a.at(s, ctx, c) / d if d != 0 else 0.0


class Neg(Signal):
    def __init__(self, x: Signal):
        super().__init__()
        self.x = x
    def _eval(self, s, ctx, c): return -self.x.at(s, ctx, c)
    def children(self): return (self.x,)


# ==============================================================================
# Modifiers
# ==============================================================================

class Clamp(Signal):
    def __init__(self, x: Signal, lo: float = 0.0, hi: float = 1.0):
        super().__init__()
        self.x, self.lo, self.hi = x, lo, hi
    def _eval(self, s, ctx, c):
        v = self.x.at(s, ctx, c)
        return max(self.lo, min(self.hi, v))
    def children(self): return (self.x,)


class Rectify(Signal):
    """Absolute value."""
    def __init__(self, x: Signal):
        super().__init__()
        self.x = x
    def _eval(self, s, ctx, c): return abs(self.x.at(s, ctx, c))
    def children(self): return (self.x,)


class Power(Signal):
    """Raise signal to a power. If unipolar, maps [-1,1] to [0,1] first."""
    def __init__(self, x: Signal, gamma: float = 2.0, unipolar: bool = False):
        super().__init__()
        self.x, self.gamma, self.unipolar = x, gamma, unipolar
    def _eval(self, s, ctx, c):
        v = self.x.at(s, ctx, c)
        if self.unipolar:
            v = (v + 1) * 0.5
        return math.copysign(abs(v) ** self.gamma, v)
    def children(self): return (self.x,)


class MapRange(Signal):
    """Linear map from [in_min, in_max] to [out_min, out_max]."""
    def __init__(self, x: Signal, in_min=0, in_max=1, out_min=0, out_max=1, clamp=True):
        super().__init__()
        self.x = x
        self.in_min, self.in_max = float(in_min), float(in_max)
        self.out_min, self.out_max = float(out_min), float(out_max)
        self.clamp = clamp
        in_range = self.in_max - self.in_min
        self.scale = (self.out_max - self.out_min) / in_range if in_range != 0 else 0

    def _eval(self, s, ctx, c):
        v = self.x.at(s, ctx, c)
        y = self.out_min + (v - self.in_min) * self.scale
        if self.clamp:
            lo, hi = min(self.out_min, self.out_max), max(self.out_min, self.out_max)
            y = max(lo, min(hi, y))
        return y
    def children(self): return (self.x,)


class Mix(Signal):
    """Linear interpolation: mix(a, b, t) = a*(1-t) + b*t"""
    def __init__(self, a: Signal, b: Signal, t: Signal):
        super().__init__()
        self.a, self.b, self.t = a, b, t
    def _eval(self, s, ctx, c):
        tv = self.t.at(s, ctx, c)
        return self.a.at(s, ctx, c) * (1 - tv) + self.b.at(s, ctx, c) * tv
    def children(self): return (self.a, self.b, self.t)


class Smooth(Signal):
    """One-pole smoothing filter with separate rise/fall times."""
    def __init__(self, x: Signal, rise_ms: float = 10, fall_ms: float = 10):
        super().__init__()
        self.x = x
        self.rise_ms = rise_ms
        self.fall_ms = fall_ms
        self._prev = 0.0
        self._prev_sample = -1

    def _eval(self, s, ctx, c):
        target = self.x.at(s, ctx, c)
        if self._prev_sample < 0:
            self._prev = target
            self._prev_sample = s
            return target

        dt = (s - self._prev_sample) / ctx.sample_rate
        self._prev_sample = s

        ms = self.rise_ms if target > self._prev else self.fall_ms
        if ms <= 0:
            self._prev = target
            return target

        tau = ms / 1000.0
        alpha = 1.0 - math.exp(-dt / tau) if dt > 0 and tau > 0 else 1.0
        self._prev += alpha * (target - self._prev)
        return self._prev

    def children(self): return (self.x,)


# ==============================================================================
# Wavetable signals
# ==============================================================================

class WavetableOverTime(Signal):
    """Play a wavetable as a curve over time. Good for custom LFO shapes."""
    def __init__(self, table, *, duration_seconds: float, phase0=0.0,
                 loop=False, interp="linear"):
        super().__init__()
        self.table = list(map(float, table))
        self.duration = duration_seconds
        self.phase0 = phase0
        self.loop = loop
        self.interp = interp

    def _eval(self, s, ctx, c):
        t = s / ctx.sample_rate
        u = self.phase0 + t / self.duration
        u01 = (u % 1.0) if self.loop else max(0, min(1, u))
        return _wavetable_lookup(self.table, u01, self.interp)


class WavetableOsc(Signal):
    """Wavetable oscillator with optional time-varying frequency."""
    def __init__(self, table, freq_hz, *, phase0=0.0, loop=True, interp="linear"):
        super().__init__()
        self.table = list(map(float, table))
        self.freq = as_signal(freq_hz)
        self.phase0 = phase0
        self.loop = loop
        self.interp = interp

    def _eval(self, s, ctx, c):
        if isinstance(self.freq, Const):
            t = s / ctx.sample_rate
            phase = self.phase0 + t * self.freq.value
        else:
            # Approximate: use frequency at this sample
            t = s / ctx.sample_rate
            f = self.freq.at(s, ctx, c)
            phase = self.phase0 + t * f

        u01 = (phase % 1.0) if self.loop else max(0, min(1, phase))
        return _wavetable_lookup(self.table, u01, self.interp)

    def children(self): return (self.freq,)


def _wavetable_lookup(table, phase01, interp="linear"):
    n = len(table)
    if n == 0: return 0.0
    if n == 1: return table[0]
    pos = phase01 * n
    if interp == "nearest":
        return table[int(pos) % n]
    # linear
    i0 = int(pos)
    frac = pos - i0
    return table[i0 % n] * (1 - frac) + table[(i0 + 1) % n] * frac


# ==============================================================================
# Convenience constructors
# ==============================================================================

def sine(freq_hz=1.0, phase=0.0):
    """Sine wave LFO: outputs -1 to +1."""
    f = float(freq_hz)
    p = float(phase)
    return TimeFn(lambda t: math.sin(2 * math.pi * f * t + p))

def triangle(freq_hz=1.0):
    """Triangle wave LFO: outputs -1 to +1."""
    f = float(freq_hz)
    return TimeFn(lambda t: 2 * abs(2 * ((t * f) % 1) - 1) - 1)

def sawtooth(freq_hz=1.0):
    """Sawtooth wave: outputs -1 to +1."""
    f = float(freq_hz)
    return TimeFn(lambda t: 2 * ((t * f) % 1) - 1)

def square(freq_hz=1.0, duty=0.5):
    """Square wave: outputs -1 or +1."""
    f = float(freq_hz)
    d = float(duty)
    return TimeFn(lambda t: 1.0 if ((t * f) % 1) < d else -1.0)

def ramp(duration, start=0.0, end=1.0):
    """Linear ramp from start to end over duration seconds."""
    return TimeFn(lambda t: start + (end - start) * min(t / duration, 1.0))

def envelope(attack=0.01, decay=0.1, sustain=0.7, release=0.5):
    """Simple ADSR envelope (time-based, not gate-triggered)."""
    a, d, s, r = attack, decay, sustain, release
    def env(t):
        if t < a:
            return t / a if a > 0 else 1.0
        t -= a
        if t < d:
            return 1.0 - (1.0 - s) * (t / d) if d > 0 else s
        t -= d
        # sustain holds for a bit then release
        hold = 1.0  # 1 second sustain hold
        if t < hold:
            return s
        t -= hold
        if t < r:
            return s * (1.0 - t / r) if r > 0 else 0.0
        return 0.0
    return TimeFn(env)

def random_hold(freq_hz=1.0, seed=42):
    """Sample-and-hold random signal, new value every period."""
    import random
    rng = random.Random(seed)
    f = float(freq_hz)
    values = {}
    def rh(t):
        idx = int(t * f)
        if idx not in values:
            values[idx] = rng.uniform(-1, 1)
        return values[idx]
    return TimeFn(rh)

def beat_pulse(beats=1.0, duty=0.5):
    """Pulse signal synced to beats."""
    return BeatFn(lambda b: 1.0 if (b % beats) < (beats * duty) else 0.0)

def beat_ramp(beats=4.0):
    """Ramp from 0 to 1 over N beats, repeating."""
    return BeatFn(lambda b: (b % beats) / beats)


# ==============================================================================
# Binding signals to parameters
# ==============================================================================

_bindings = []  # list of (signal, node_idx, param_idx)

def bind(signal: Signal, node_idx: int, param_idx: int):
    """Bind a signal to a plugin parameter.
    The signal will be evaluated each audio block and the value applied."""
    _bindings.append((signal, node_idx, param_idx))
    print(f"Bound signal {signal.id} to node {node_idx} param {param_idx}")

def get_bindings():
    """Return current bindings for the graph processor."""
    return _bindings

def clear_bindings():
    """Remove all signal bindings."""
    _bindings.clear()
