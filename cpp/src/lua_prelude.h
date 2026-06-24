#pragma once

namespace SoundShop {

// Convenience aliases run before any user Lua chunk so that SEANCE-style names
// (sin, clamp, saw, noise, ...) work without the `math.` prefix, plus the
// non-standard waveshapers. Users can still use the `math.` library directly.
//
// Shared by the live Lua runtime (script_runtime_lua.cpp) and the static-shape
// baker (shape_expr.cpp) so both expose an identical vocabulary.
//
// NOTE: this prelude must NOT define a global `out`. In the live runtime
// registerApi() registers `out` as the block-mode sample-writer FUNCTION
// (out(i, value)) for the Signal/Unified roles, and the prelude runs AFTER
// registerApi(), so a prelude `out = 0` would clobber that function and break
// every per-block signal script (the classic symptom: "attempt to call a number
// value (global 'out')"). MIDI-role pin selection still works without a default:
// readOut() reads global `out`, an unset global reads as nil -> 0 (pin 0), and a
// MIDI script can still set `out = k` to pick a pin (the MIDI role doesn't
// register the function, so there's no collision there).
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
    // Range converters + unipolar aliases. Control signals on the wire are
    // unipolar 0..1, but sin/cos/tan return bipolar -1..1, so a bare sin()
    // clips its negative half. unipolar(x) maps -1..1 -> 0..1 (wrap a whole
    // bipolar expression in it); bipolar(x) maps 0..1 -> -1..1. usin/ucos/utan
    // and usaw/usquare/utriangle are the unipolar shorthands.
    "function unipolar(x) return x*0.5+0.5 end\n"
    "function bipolar(x) return x*2-1 end\n"
    "function usin(x) return math.sin(x)*0.5+0.5 end\n"
    "function ucos(x) return math.cos(x)*0.5+0.5 end\n"
    "function utan(x) return math.tan(x)*0.5+0.5 end\n"
    "function usaw(p) return saw(p)*0.5+0.5 end\n"
    "function usquare(p) p=p-math.floor(p) if p<0.5 then return 1 else return 0 end end\n"
    "function utriangle(p) return triangle(p)*0.5+0.5 end\n"
    // GLSL-parity scalar math, so the Lua dialect exposes the same vocabulary as
    // the Built-in and GLSL shape languages. asin/acos/atan/rad/deg come from
    // math.*; sinh/cosh/tanh were dropped in Lua 5.3 so we define them (tanh is
    // the single most common waveshaping saturator, so its absence was a real
    // gap); the rest are GLSL's named helpers (mod uses GLSL's floored
    // semantics, not C fmod).
    "asin=math.asin acos=math.acos atan=math.atan radians=math.rad degrees=math.deg\n"
    "function sinh(x) return (math.exp(x)-math.exp(-x))*0.5 end\n"
    "function cosh(x) return (math.exp(x)+math.exp(-x))*0.5 end\n"
    "function tanh(x) if x>20 then return 1 elseif x<-20 then return -1 end "
    "local e=math.exp(2*x) return (e-1)/(e+1) end\n"
    "function sign(x) if x>0 then return 1 elseif x<0 then return -1 else return 0 end end\n"
    "function fract(x) return x-math.floor(x) end\n"
    "function trunc(x) if x<0 then return math.ceil(x) else return math.floor(x) end end\n"
    "function round(x) return math.floor(x+0.5) end\n"
    "function inversesqrt(x) if x>0 then return 1/math.sqrt(x) else return 0 end end\n"
    "function mod(a,b) if b==0 then return 0 else return a-b*math.floor(a/b) end end\n"
    "function mix(a,b,t) return a+(b-a)*t end\n"
    "function step(edge,x) if x<edge then return 0 else return 1 end end\n"
    "function smoothstep(e0,e1,x) local t=clamp((x-e0)/(e1-e0),0,1) return t*t*(3-2*t) end\n"
    // Remaining GLSL exponential + inverse-hyperbolic + common builtins, so the
    // Lua dialect covers the full scalar GLSL vocabulary. log2 guards x>0;
    // acosh/atanh guard their domains the same way the Built-in parser does.
    "function exp2(x) return 2^x end\n"
    "function log2(x) if x>0 then return math.log(x,2) else return 0 end end\n"
    "function fma(a,b,c) return a*b+c end\n"
    "function asinh(x) return math.log(x+math.sqrt(x*x+1)) end\n"
    "function acosh(x) if x<1 then x=1 end return math.log(x+math.sqrt(x*x-1)) end\n"
    "function atanh(x) x=clamp(x,-0.999999,0.999999) return 0.5*math.log((1+x)/(1-x)) end\n"
    "function roundEven(x) local f=math.floor(x) local d=x-f "
    "if d<0.5 then return f elseif d>0.5 then return f+1 "
    "elseif f%2==0 then return f else return f+1 end end\n";

} // namespace SoundShop
