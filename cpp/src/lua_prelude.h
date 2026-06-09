#pragma once

namespace SoundShop {

// Convenience aliases run before any user Lua chunk so that SEANCE-style names
// (sin, clamp, saw, noise, ...) work without the `math.` prefix, plus the
// non-standard waveshapers. Users can still use the `math.` library directly.
//
// Shared by the live Lua runtime (script_runtime_lua.cpp) and the static-shape
// baker (shape_expr.cpp) so both expose an identical vocabulary. The trailing
// out_pin/out globals are only meaningful to the live MIDI/Signal runtime and
// are harmless in the baker.
inline constexpr const char* kSoundShopLuaPrelude =
    "sin=math.sin cos=math.cos tan=math.tan sqrt=math.sqrt abs=math.abs "
    "exp=math.exp log=math.log floor=math.floor ceil=math.ceil "
    "min=math.min max=math.max pi=math.pi\n"
    "function pow(a,b) return a^b end\n"
    "function clamp(x,lo,hi) if x<lo then return lo elseif x>hi then return hi else return x end end\n"
    "function saw(p) p=p-math.floor(p) return 2*p-1 end\n"
    "function square(p) p=p-math.floor(p) if p<0.5 then return 1 else return -1 end end\n"
    "function triangle(p) p=p-math.floor(p) if p<0.5 then return 4*p-1 else return 3-4*p end end\n"
    "function noise(x) return math.random() end\n"
    "out_pin=0 out=0\n";

} // namespace SoundShop
