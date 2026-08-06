# Known Issues

Running log of bugs and tech debt that aren't fixed yet. New entries go at the
top. When something is fixed, delete the entry (git history is the archive).

---

## CLEANUP: delete the now-unused `cpp/third_party/rubberband` directory

**Found:** 2026-08-05, finishing the Rubber Band removal.

The GPL v2 licensing problem is **resolved** — `PitchShiftProcessor` now runs on
the in-house `PhaseVocoderShifter`, and `cpp/CMakeLists.txt` no longer builds or
links Rubber Band at all. Verified: the shipped `SEANCE.exe` contains zero
occurrences of the `RubberBand` symbol prefix.

What remains is housekeeping. `cpp/third_party/rubberband/` (2.3 MB of GPL v2
source) is still on disk. It is untracked/gitignored, so it is not in the repo
and not in any build, but it should be deleted from working copies so nobody
wires it back up by accident. Left for the user rather than done automatically,
because deleting untracked files is unrecoverable.

`cpp/setup_dependencies.bat` no longer downloads it, so a fresh clone will not
reacquire it — this is purely about existing working copies.

---

## REMOVED: Wavelet Pitch Shift (kept here as the rebuild spec)

**Found:** 2026-08-05, by adding a CPU-budget self-test to the wavelet suite.

**Resolved 2026-08-05 by deleting the node.** `WaveletPitchShiftProcessor` did
not work in any respect. Four independent defects, all measured, not inferred:

**1. It is 7x too slow to run in real time.** Measured **1.45x realtime** by
`wavelet-cpu: Pitch Shift ...` — one instance alone consumes ~70% of a single
core's entire audio budget, leaving nothing for the rest of the graph. Every
other wavelet effect measures 16x–1000x. The cause is direct time-domain
convolution in `cwt()`/`icwt()`: 24 Morlet scales whose widths are `6*scale+1`
sum to ~2700 taps, so cwt+icwt is ~11,000 flops per sample per channel
(~1.06 Gflop/s at 48 kHz stereo).

**2. It transposes by the wrong amount.** Feeding a 440 Hz sine and measuring
the dominant output frequency:

| Semitones | Expected | Measured |
|---|---|---|
| +12 | 880 Hz | **1362 Hz** |
| +7  | 659 Hz | **904 Hz**  |
| +1  | 466 Hz | **442 Hz** (no shift at all) |

**3. It cannot resolve small intervals.** 24 log-spaced scales over 2..64 is
5 octaves at ~4.8 scales/octave — about **2.5 semitones per scale step**. The
shift logic snaps to the *nearest existing scale index* (`bestIdx`), so any
shift under ~1.25 semitones quantises to zero, which is exactly what the +1
row above shows. Phase is also copied verbatim rather than rescaled by the
pitch ratio, and when two source scales collide on one target index the
magnitudes accumulate while the phase is simply overwritten by whichever ran
last.

**4. The output is effectively silent.** Input RMS 0.353 → output RMS
0.0005, i.e. **about -58 dB**. `icwt()` is not the inverse of `cwt()`: it
discards the imaginary part and normalises by a sum of `1/scale^2` weights
rather than the Morlet admissibility constant, so it has no unity-gain path
even at ratio 1.

There is also a fifth, structural problem: `cwt`/`icwt` are called per block
with zero-padding outside the block (`if (idx < 0 || idx >= N) continue;`).
The widest wavelet is `6*64+1 = 385` samples against a 512-sample block, so
each block is analysed in near-total isolation — guaranteeing amplitude
collapse at both block edges and discontinuities at the block rate.

**Good news for the fix:** `cwt`/`icwt` have exactly **one** caller in the
whole codebase (this node), so they can be rewritten freely. And because the
node currently emits -58 dB of wrong-pitch noise, no saved project can
meaningfully depend on its sound — there is no compatibility burden.

**What the proper fix looks like.** All four defects have known remedies:
- *Gain*: reconstruct with the standard Morlet admissibility constant
  (Torrence & Compo delta reconstruction), keeping complex coefficients rather
  than magnitude+phase.
- *Accuracy*: shift by a **fractional** index with interpolation along the log
  scale axis instead of snapping to `bestIdx`, and multiply phase by the pitch
  ratio. Interpolation is what buys semitone resolution — bumping the grid to
  12 scales/octave instead would triple an already-unaffordable cost.
- *Continuity*: keep overlap history across blocks instead of zero-padding.
- *Speed*: FFT-based convolution — one shared forward FFT, then per scale a
  complex multiply plus an inverse FFT. Rough estimate ~1.4M flops per block
  per channel versus 5.5M today.

**Why it was dropped rather than fixed.** Its stated selling point was
transient preservation, and `IndependentPitchShiftProcessor` already delivers
exactly that — it splits transient from tonal via wavelet thresholding, shifts
only the tonal part, and recombines, so drum punch and plucked attacks survive.
As of 2026-08-05 that node genuinely works (0 cents error at +12 semitones,
13.7x realtime). A CWT scale-shift shifter would therefore have been a second,
much slower node competing with a working one. Even done correctly it is
unlikely to clear the 10x-realtime bar every other wavelet effect meets, so it
would have had to ship offline/bounce-only.

The rejected alternative worth recording: reimplementing it on
`PhaseVocoderShifter` while keeping the name. That is cheap and real-time, but
the node would no longer be "wavelet" anything — it would duplicate Ind. Pitch
Shift under a misleading label.

**What removal touched.** The class, the `__waveletpitch__` factory branch, the
Effects-menu entry (id 231, now an intentional gap), the creation site, and the
`knownBug()` self-tests. `cwt()`/`icwt()` in `wavelet.h` were left in place but
carry a warning comment: they now have no callers, and they are the two
functions the remedies above would replace.

**Loading an old project** that contains a `__waveletpitch__` node is safe: the
factory falls through to `PassthroughProcessor`. Note this makes such a node
*louder* than before, since it used to emit ~-58 dB. Nothing can meaningfully
depend on its previous sound.

## BUG: most wavelet effect nodes still allocate on the audio thread

**Found:** 2026-08-05, while extracting the wavelet DSP for plugin work.

Every wavelet effect node runs `dwt()`/`idwt()` inside `processBlock`. Written
naively that is a heap allocation storm on the real-time thread: `getWaveletFilter()`
rebuilds the whole 4-vector filter bank **every block**, the padded/dry buffers are
`std::vector`s constructed per channel per block, and `dwtStep`/`idwtStep` each
allocated two more per level per channel. Order of 35 malloc/free pairs per block
per node - thousands per second. `malloc` can block on a global lock, so this is a
genuine dropout source, and it is an automatic fail in plugin validation
(pluginval strictness 10 flags allocation in `processBlock`).

**Fixed** — `wavelet.h` now has `WaveletWorkspace` (caller-owned transform
scratch) and `WaveletFxScratch` (workspace + cached filter bank + padded/dry
buffers). `prepare()` in `prepareToPlay` sizes everything once; the steady state is
allocation-free, covered by the `wavelet-scratch: ... never reallocates` tests.

All twelve *working* wavelet effects are converted: Transient Split, Wavelet
Denoiser, Wavelet Bitcrush, Octave Shift, Wavelet Multiband Comp, Wavelet Reverb,
Independent Pitch Shift, Wavelet Complexity, Asymmetric Filter, Wavelet Pitch
Tracker, Wavelet Vocoder, Formant Pitch Shift.

**Deliberately not converted: Wavelet Pitch Shift.** Skipped because it was
slated for a rewrite; the node has since been deleted outright (see the entry
above), so the skipped work would have been thrown away entirely.

Two traps worth remembering for the same conversion elsewhere:
- `std::vector<bool>` cannot be reused without reallocating (it is a bitset
  specialisation, not a container of bools). Wavelet Complexity's keep-mask is a
  `juce::uint8` vector for this reason.
- `reserve()` + `clear()`/`assign()`/`resize()` is the allocation-free idiom;
  constructing a vector with a size argument always allocates, even into an
  existing variable.

**Sweep outside the wavelet family: DONE 2026-08-06.** See the entry below.

---

## BUG: audio-thread allocation outside the wavelet family (sweep results)

**Found:** 2026-08-06, completing the "sweep the same class of bug elsewhere"
item above. Method: a script that brace-matches every `processBlock` body in
`cpp/src` and flags local `std::vector` / `juce::AudioBuffer` / `juce::String` /
`std::string` construction, `new`, `make_unique`, `resize`, `push_back`,
`emplace_back`, `setSize`, and `std::function` construction. 66 raw hits,
triaged below. Re-run it before trusting this list again — it is a snapshot.

### Fixed (commit `14c2615`)

- **`GraphProcessor::processBlock`** (`graph_processor.cpp`) — built a fresh
  `juce::AudioBuffer` **and** `juce::MidiBuffer` on *every audio callback*, the
  hottest path in the app. Now members reused via
  `setSize(..., avoidReallocating=true)` / `clear()`.
- **`CurveEQProcessor`** (`builtin_effects.h`) — constructed a whole `FFT`
  (twiddle + bit-reversal tables) plus seven `std::vector`s per block, and
  re-evaluated the response curve on every transform-size change (i.e. during
  an FFT Size knob drag). Now uses the new `FFTLadder` (one prebuilt FFT per
  selectable size), `FFT`'s allocation-free pointer API, and a gain table
  precomputed per size in `prepareToPlay`. Guarded by a capacity-growth test.

### Fixed (commit `231ef5d`)

- **`SignalEQProcessor`** (`builtin_effects.h`) — same shape as Curve EQ: an
  `FFT` plus seven `std::vector`s per block in FFT mode (`Mode` = 1). Converted
  to the same `FFTLadder` + pointer-API + prepared-scratch design. Two
  differences worth knowing if you touch it:
  - The per-bin gain table **cannot** be precomputed the way Curve EQ's is —
    every point coordinate is signal-modulatable, so the curve can change per
    block. Instead `fftGains` is `reserve`d to the largest row in
    `prepareToPlay` and refilled with `resize` (not `assign`) — growing to a
    size `<= capacity` is guaranteed not to reallocate.
  - `bands` is `reserve`d to `kMaxPoints`, because adding/removing a point in
    the editor lands on the audio thread as a `bands.resize()`.
  - The duplicated biquad cascade is now `processBiquad()`, and it is also the
    fallback whenever the FFT path can't run (block too short, or bigger than
    `prepareToPlay` promised). It has the same magnitude response by
    construction, so falling back doesn't move the curve — better than Curve
    EQ's bail-to-dry.
  Guarded by a capacity-growth test that also sweeps Mode and the point count.

### Fixed (commit `59dff56`)

- **`SMSProcessor`** (`builtin_effects.h`) — worst of the three: an `FFT` plus
  eight `std::vector`s **per frame**, not per block, so a single 2048-sample
  block at FFT Size 256 hit the allocator ~15 times over. Converted to the same
  `FFTLadder` + prepared-scratch design as the two EQs. Two real bugs fell out
  of writing the tests for it (SMS had none before):
  - **Heap corruption on non-power-of-two host blocks.** The transform size was
    clamped with `if (fftSize > n) fftSize = n;` — which hands a
    non-power-of-two size straight to `FFT`, whose `assert(isPow2)` is compiled
    out in Release. `logN` then rounds *up*, so the bit-reversal permutation
    produces indices past the end of the spectrum buffer and `transform()`
    writes outside it. 480 samples is an entirely ordinary host block size.
    Now rounds down to a power of two, and the `FFTLadder` lookup returns
    nullptr for anything it wasn't built for, so the class of mistake can't
    recur.
  - **Unnormalised overlap-add.** Hann² at 50% overlap sums to `0.5·(1+cos²)`,
    rippling between 0.5 and 1.0 — so SMS imposed a tremolo at the frame rate
    (~43 Hz at FFT Size 1024 / 44.1 kHz) and ran ~2.5 dB quiet. (The code even
    carried a comment claiming it was "constant 1.0 after normalization"; there
    was no normalization.) Now divides by the accumulated window power, and a
    test pins `Threshold = 0` at exactly unity gain.
  Also gained a tail frame so the end of each block isn't left dry.

### Fixed (commit `de3f0d2`)

- **`SpectrumTapProcessor::processBlock`** (`spectrum_tap.cpp`) — three
  `std::vector`s sized by `bins.size()` on every callback (`binParamIdx`,
  `sigOut`, `customTarget`). Now members sized in `rebuildBins()`, the only
  place the bin count changes — and that changes the node's pin count, so it
  happens off the audio thread via a graph rebuild. `processBlock` refills them
  in place and bails (rather than resizing) if they ever disagree with `bins`.
  Guarded by a capacity-growth test.
  - The *other* allocation in this file is deliberately left: `processBlock`
    calls `rebuildBins()` itself when the "Bin " param count no longer matches
    `bins`, and that path allocates freely (`decodeScript`, `SpectralCurve`, a
    new `FFT`). It's a safety net that shouldn't normally fire — adding or
    removing a bin changes the pin count and therefore rebuilds the graph — so
    closing it properly means routing bin-count changes exclusively through the
    rebuild path, not micro-optimising the fallback.

### Fixed (commit `09ef5a8`)

- **`TerrainSynthProcessor`** (`terrain_synth.cpp`) — by far the worst of the
  set. The original triage listed the per-*block* allocations; reading the code
  turned up much worse ones per *sample*:
  - **`Terrain::sample`** built a fresh `std::vector<int> indices(nd)` inside
    its corner loop — **2^nd heap allocations per call**, and it is called once
    per output sample per active voice (twice in graintable mode). A 2D terrain
    playing an eight-note chord was on the order of 1.4 million allocations a
    second. Now uses stack arrays and accumulates the flat index directly. Also
    fixed a latent out-of-bounds read: it indexed `coord[d]` unconditionally,
    which `sampleMipmap`'s 1-element fallback violated on any multi-dimensional
    terrain. There is now a single `product(dims) == data.size()` check per call
    instead of a `jlimit` per corner, so the unchecked `at()` can't run off the
    end when dims and data desync.
  - **`Traversal::evaluate` returned a `std::vector<float>` by value**, once per
    sample. Converted to an out-parameter (`evaluate(coord, ...)`); the Path and
    Physics branches now copy element-wise instead of assigning a whole vector.
  - **`auto pitchCoord = coord` / `coordA` / `coordB`** — up to three more
    vector copies per sample *per voice*. `coordA` was never even modified, so
    it was a copy for nothing.
  - **Position params were addressed by building a `std::string`.** `"Position "
    + std::to_string(k + 1)` plus a linear scan over `node.params`, for every
    axis of every sample. The names are now cached in `wtPositionNames`
    (rebuilt wherever `wtEffectiveAxes` is set) and the *values* are resolved
    once per block — they're plain node params and cannot change mid-block.
  - **Scatter blend**: `qpos`/`weights`/`dists`/`blended`/`coeffs` per block,
    plus `getWaveletFilter("db2")` returning three vectors by value, plus the
    `dwt`/`idwt` convenience overloads that construct a `WaveletWorkspace`
    internally — *per frame*. Roughly 40 allocations a block with eight frames.
    All now members; `wavelet.h` documents those overloads as offline-only and
    the workspace-taking ones are used instead.
  - **`refreshPartialBank`** (AdditiveBank mode, once per block) constructed an
    `FFT` — rebuilding its twiddle and bit-reversal tables from scratch — and
    used the allocating `forwardReal` overload. Now an `FFTLadder` prepared in
    `prepareToPlay`, because the transform size follows the terrain and a script
    reload can change it without a `prepareToPlay`, so lazy rebuild-on-change
    would still allocate mid-stream.
  - **`computeGranularWeights` / `computeInharmonicWeights`** built a fresh
    `std::vector<std::vector<float>>` of every entry's position per block — one
    allocation per side-table entry. Now gathered into grow-only member rows;
    `computeSideTableWeights` takes an explicit `entryCount` because the scratch
    can be longer than the live entry list.
  Guarded by a capacity-growth test (`terrain-alloc`) sweeping Position, Synth
  Mode and block length over 60 blocks, with a non-vacuity check that the synth
  is still audible. The test earned its keep immediately: it caught the
  AdditiveBank buffers sizing on first use, which is why the warm-up visits all
  three Synth Modes before the measurement.
  - Left as-is, deliberately: `st.phase.resize`, `v.granStreams.resize`,
    `v.inhStreams.resize` and `ensureScatterScratch` all allocate only when the
    wavetable's *shape* changes, which is a message-thread edit. Same
    grow-once-then-never pattern as the triaged-benign entries below.
  - Not closed: `reloadIfScriptChanged()` is called from `processBlock` and
    parses scripts / rebuilds terrains, which allocates freely. That's a
    structural problem (script reloads should be handed over from the message
    thread, not performed on the audio thread), not something to paper over
    here.

### Fixed (commit `d0a6480`) — `SoundFontProcessor`

Two allocation sites, plus a much more serious bug found on the way in.

- `std::vector<float> interleaved(numSamples * 2, 0.0f)` was a **local** in the
  SF2/TSF render path, i.e. a heap allocation on literally every audio callback.
  Now a member, sized in `prepareToPlay` and `resize`d (never `assign`ed) if a
  host hands over a block longer than promised.
- `SFZInstrument::findRegions` returned `std::vector<const SFZRegion*>` **by
  value**, so every note-on allocated. Now takes an out-parameter and does
  `clear()` + `push_back`; the caller's `regionMatches` member is reserved to
  the region count in `prepareToPlay` *and* in `loadFile` (a user can pick a new
  .sfz after prepare, changing the region count).
- Both proved by `soundfont:` in `--self-test`: warm up, snapshot
  `scratchCapacityBytes()`, then 60 blocks of note-on/note-off traffic at six
  different block lengths, assert zero capacity growth, plus a non-vacuity peak
  check. The instrument is generated on the fly (a looping sine `.wav` and a
  two-region `.sfz` in the temp dir) so there's no binary fixture and no
  dependency on a SoundFont being installed.

**The real find: `.sf2` and `.sfz` instrument nodes never loaded anything.**
`loadFile()` stripped the script tag with `script.substr(7)`, but `"__sf2__:"`
and `"__sfz__:"` are **8** characters including the colon — so every path
arrived with a leading `':'`, `tsf_load_filename` / `juce::File` couldn't find
it, and the node rendered silence. `SfizzProcessor` got the equivalent offset
right (`substr(10)` for `"__sfizz__:"`), which is why the sfizz path worked and
this one didn't, and presumably why it went unnoticed. Fixed to `substr(8)`; the
self-test now asserts `proc.isSFZ()` after construction, which pins the offset
down for good.

### Fixed (commit `e3a6c61`) — `SignalShapeProcessor`

The worst offender of the sweep, because the cost scaled with the expression
vocabulary rather than being one buffer. Per block, `processBlock` constructed:

- `std::vector<const float*> sigChans` — the s1..sN read pointers. Also handed
  to the block-mode runtime as `ScriptBlockCtx::sig`, so it had to outlive the
  call anyway. Now a member; `clear()` + `push_back` keeps the capacity.
- `std::unordered_map<std::string,float> vars` — **a whole map per block**: a
  bucket array plus one node allocation for each of the ~12 fixed variables and
  every s1..sN, all freed again at the end of the block. Roughly 15 allocations
  per callback. Now a member that survives across blocks, so `operator[]` on an
  existing key only overwrites the value. Rebuilt only when `signalInputCount`
  changes (tracked by `varsSigCount`), so shrinking the s-list can't leave a
  stale `sN` visible to an expression.
- `std::function<float(float)> shapeFn` — the `shape(pos)` sampler. Built once
  now (in the constructor and in `prepareToPlay`; it captures only `this`, and
  `shapeSamples` is a member, so a shape rebuild doesn't invalidate it). It fits
  MSVC's small-buffer optimisation today, but whether a `std::function`
  heap-allocates is an implementation detail not worth betting the audio thread
  on.
- `ScriptVars sv` on the transport play edge (two sites) — same map cost, now
  the reused `startVars` member.
- Not an allocation but on the same hot path: the per-sample s-list binding
  rebuilt a `"s" + std::to_string(i + 1)` key for every input of every sample.
  Cached as `sigVarNames`.
- `outPtrs.assign` / `blockOut.assign` / `outBufs.assign` → `resize`. Growing to
  a size within capacity is *guaranteed* not to reallocate for `resize`;
  `assign` carries no such guarantee.

`prepareToPlay` (previously an inline one-liner that only stored the sample
rate) now pre-sizes all of it and seeds both maps with their fixed keys.

Proved by `signalshape:` in `--self-test`: 60 blocks sweeping Rate / Beat Sync /
Phase / block length with note traffic and moving control inputs, asserting zero
growth in `scratchCapacityBytes()`. That proxy counts `vars.bucket_count() +
vars.size()`, so both a rehash and a single newly-inserted key (one node
allocation) register. Paired with a check that the output is still *modulating*,
not merely non-zero — a stuck constant would pass a peak test while proving the
expression never ran.

### Fixed (commit `fd854f1`) — `MidiScriptProcessor`

Sibling of the Signal Shape node and had exactly the same problem, so the same
fix: `sigChans` and the `vars` binding map were both `processBlock` locals, so
every callback built and tore down an `unordered_map` with a node allocation per
variable (11 fixed keys plus s1..sN), and `buildVars` rebuilt a
`"s" + std::to_string(i + 1)` key for every input of every sample. All are now
members; `buildVars` reads the s-list from them instead of taking it as a
parameter. `prepareToPlay` (previously an inline one-liner storing only the
sample rate) pre-sizes the scratch, seeds `vars` with every key the program can
bind, reserves `pendingOffs` to its `kMaxPendingOffs` hard cap, and sets
`varsSigCount` to match so the s-list rebuild only fires if the doc later
changes the input count.

Proved by `midiscript:` in `--self-test`: 60 blocks at six block lengths driving
a program that reads s1/s2 and calls `note()` (so `pendingOffs` is exercised
across block boundaries too), asserting zero growth in `scratchCapacityBytes()`,
paired with a check that the program was still emitting MIDI during the sweep.

### Fixed (commit `93f6ee3`) — the `builtin_effects.h` sweep

Five processors in one pass. The triage list had only named two of them; reading
the code turned up three more, and in two cases the named site wasn't the worst
one in the function.

**`ArpeggiatorProcessor`.** Held notes lived in a `std::set<int>`, so every
note-on allocated a tree node on the audio thread — and it bought nothing, since
walking a `std::bitset<128>` in index order already yields the ascending order
the sequence builder wanted. That's why the separate `baseNotes` copy and its
`std::sort` are gone too. `seq` is now a member reserved in `prepareToPlay` to
the true worst case (128 held notes × 4 octave copies × 2 for the up-down
pattern's descending tail); `clear()` keeps the capacity. The up-down pattern
used to copy the whole sequence into a scratch vector and reverse it, and now
appends its tail in place.

**`MixtureProcessor`.** `juce::MidiBuffer output` was a `processBlock` local —
a fresh heap buffer per callback. Now a member: `swapWith` hands it the old
input's storage and `MidiBuffer::clear()` keeps the allocation
(`Array::clearQuick`), so after a few blocks it stops touching the allocator.

**`FMSynthProcessor`.** Built a `std::string` per operator per block purely to
concatenate a param name. Replaced with two `static constexpr const char*`
tables. (These particular strings are short enough for MSVC's SSO, so this was
probably not reaching the heap in practice — but the dance was pointless
regardless and the next operator name added could have crossed the line.)

**`PitchDetectorProcessor`.** The per-hop analysis copy was a local
`std::vector<float>` sized to `window` — and `window` is *derived from the
Min Hz param*, so turning that knob resized a heap buffer on the audio thread.
Now a member sized once to `kMaxWindow` in `prepareToPlay`.

**`ParticleSynthProcessor` / `SpectralGrainProcessor`.** Unbounded
`push_back` per grain spawn with no `reserve`. Both now reserve a hard cap
(`kMaxGrains` / `kMaxActiveGrains`, 1024) and *enforce* it at the spawn site, so
capacity and size can never diverge. The cap doubles as a runaway guard on the
Density param; it drops the *newest* grain rather than stealing the oldest, so a
runaway Density just stops getting denser instead of turning into a stutter.
Both classes had a vestigial "safety: cap grain count" that erased the oldest
half of the cloud — dead now (the spawn guard fires first) and the wrong shape
anyway, since it cut grains mid-envelope, which is audible as a click. Removed.

Proved by `arpeggiator:`, `particlesynth:`, `spectralgrain:` and `pitchdetect:`
in `--self-test`, all following the established recipe: warm up, snapshot
`scratchCapacityBytes()`, sweep parameters and block lengths, assert zero
growth, paired with a non-vacuity check. Two extra precautions in these, because
the capacity proxy is easy to make vacuous:

- Each test **warms up at the *small* end** (a 6-note chord, Density 5) and only
  then sweeps to the worst case. Warming up already-saturated would settle the
  capacity at its ceiling under *either* implementation, so the assertion would
  pass even with the `reserve` deleted.
- Each asserts the reserve is **big enough** (`before >= 128*4*2*sizeof(int)`,
  `reservedGrainCount() >= kMaxGrains`), so "capacity didn't grow" can't pass
  merely because the sequence or cloud never got long.

`MixtureProcessor` and `FMSynthProcessor` have no capacity test: `MidiBuffer`
exposes no capacity accessor, and the FM fix removes a construction outright
rather than relocating storage. Both are fixed by inspection.

### Still open — `AudioTimelineProcessor` does file I/O on the audio thread

This was the last entry on the "still allocating" list, but reading it shows the
allocation is the *least* of what's wrong, and it can't be closed with the
capacity-reserve recipe the rest of the sweep used. Recorded here in full rather
than fixed, because the honest fix is an architecture change, and reserving
`readBuf` in isolation would be a stop-gap pointing away from that architecture
(a preloaded or streamed design has no such buffer at all).

`AudioTimelineProcessor::processBlock` (`graph_processor.cpp` ~437) calls
`getAudio(clip.audioFilePath)` per clip per block, and `getAudio` is only ever
called from there. Three distinct audio-thread hazards, worst first:

1. **Blocking disk reads every block.** `audio->reader->read(...)` (~line 511) is
   a synchronous read straight off a `FileInputStream`. A cold cache, a spinning
   disk, or a network share can stall the callback for milliseconds — orders of
   magnitude past the block deadline.
2. **The whole file is *opened* on the audio thread.** On a cache miss `getAudio`
   does `File::existsAsFile()`, `formatManager.createReaderFor(file)` (opens the
   file and parses its header — for Ogg/MP3 that's a decoder spin-up),
   `make_shared`, a `std::map` insert, **and an `fprintf(stderr, ...)`**. So the
   first block in which any clip becomes audible does a file open, several
   allocations and a blocking stderr write. A plausible explanation for a glitch
   on the first bar after pressing play on a freshly loaded project.
3. **`juce::AudioBuffer<float> readBuf(fileChannels, fileSamplesToRead)`**
   (~line 510) — the original triage entry. One allocation per streaming clip
   per block.

**The proper fix** is to get file access off the audio thread entirely. Two
shapes, both real options:

- *Preload into memory.* When a clip's `audioFilePath` is set (project load,
  drag-drop, end of a recording), decode the file into a `juce::AudioBuffer` on
  a background thread and publish it into the cache; `processBlock` then only
  reads memory. Simple, removes all three hazards at once, and matches SEANCE's
  "moderate project sizes" scope — but a 5-minute stereo 44.1 kHz file is
  ~105 MB as float, so a big project could balloon.
- *Real streaming.* A per-clip background-filled ring buffer
  (`juce::BufferingAudioSource` over an `AudioFormatReaderSource`, or a
  hand-rolled equivalent, since clip playback also needs the slip offset and
  rate conversion). Bounded memory, more machinery, and it needs a policy for
  what to play when the buffer underruns.

Either way the cache must be **populated from the message thread** and the audio
thread must only ever *look up*, never create. Note also that the sample-rate
conversion at ~line 516 is nearest-neighbour (`(int)(s * fileRatio)`), which
aliases audibly on any file whose rate doesn't match the device; whichever
design lands should carry a real resampler.

Not scheduled. Nothing else from the sweep remains.

### Noticed while testing — `ParticleSynthProcessor` can exceed 0 dBFS

The `particlesynth:` self-test measures a peak of ~1.2 at Volume 0.5. The
`1/sqrt(grainCount)` normalisation is a statistical average, not a bound, so
correlated grains overshoot; unlike `SpectralGrainProcessor` (which ends its
sample loop with a `jlimit(-1, 1)`) the particle synth has no clamp. Pre-existing
behaviour, not a regression from the allocation work, and arguably the node
shouldn't hard-clip on its own — but it means a Particle node can clip whatever
it feeds. Decide between a limiter, a soft-clip, or leaving it to the user.

### Triaged as benign (verified, do not re-report)

- `pan_processor.h:48` — `juce::AudioBuffer(float* const*, ...)` is the
  pointer-**wrapping** constructor. Wraps, never allocates.
- `poly_voice_processor.cpp:161`, `terrain_synth.cpp:2456/2460`,
  `signal_shape_node.cpp:403` — `setSize(..., avoidReallocating=true)`. No
  allocation once the buffer has seen the largest block.
- `signal_lfo.h:53`, `signal_logic.h:47`, `signal_math.h:60`,
  `signal_sample_hold.h:49` — `scratch.resize(n)` on a member. Allocates only on
  the first block after a size increase, then never. Would be tidier to size
  these in `prepareToPlay`, but they are not a steady-state defect.
- `main_window.cpp:3644` (`FreezeTapProcessor`) — `push_back` into a vector the
  caller pre-`reserve`s via `reserveSamples()`. Correct by construction, though
  it does depend on the caller reserving enough.
- `terrain_synth.cpp:3100`, `3137` — false positives; those are `const&`
  function/lambda parameters, not constructions.

**Why this matters beyond dropouts:** allocation in `processBlock` is an
automatic fail in plugin validation (pluginval strictness 10), so every entry in
the "still allocating" list is also a blocker for the plugin spin-offs in
`agent-todo.md`.

**The reusable tools now exist:** `FFTLadder` in `fft_util.h` for anything that
picks a transform size at run time, `FFT`'s pointer API for allocation-free
transforms, and the capacity-growth test idiom (`scratchCapacityBytes()` +
assert it doesn't grow over a parameter sweep) for proving a conversion worked.

---

## DOC GAP: the wavelet effect suite is absent from REFERENCE.md

**Found:** 2026-08-05. **FIXED 2026-08-06** — `REFERENCE.md` now has a
`## Wavelet effects` section (TOC entry included) covering all twelve nodes:
a shared-behaviour preamble (block-based DWT, the band↔Hz mapping for `Levels`,
what `Mix` does, which wavelet family each node uses and why, which nodes report
latency), then one subsection per effect with a param table (default / range /
units), the neutral setting, and the honest caveats. A closing
"Neutral settings at a glance" table cross-references the `wavelet-fx:`
self-tests. Writing it turned up two real bugs, logged separately below
("Wavelet Reverb's Decay is per-block" and "Asymmetric Filter's ms knobs").

Original rationale, kept because it still applies to anything added to this
suite: this is the part of SEANCE with no free equivalent (unlike the wavetable
synth, which competes with Vital), so it is the most likely thing to be
productised — every new wavelet node needs the same treatment on day one.

**Same gap, the grain synths (found 2026-08-06, FIXED 2026-08-06).** `REFERENCE.md`
described how **Particle Cloud** and **Spectral Grain** relate to the shared
AHDSR envelope but never listed their params — "Density" appeared in neither
`REFERENCE.md`, the README, nor `docs/`.

**Fix:** `REFERENCE.md` gained a `## Granular synths (Particle Cloud, Spectral
Grain)` section (TOC entry included): the Density × Grain Size product that
governs cloud thickness, a param table per node, Particle Cloud's per-grain
random stereo panning and linear grain envelope, Spectral Grain's bank model
(16 IFFTs of one spectrum, `f` = bin index at a fixed 1024-point FFT, note
number as playback rate), the level/clipping difference between the two, the
grain ceiling, and Stop behaviour.

`README.md` was also missing **Spectral Grain entirely** from its instrument
inventory — a whole node type absent from the feature tour — so it gained a
bullet, and the Particle Cloud bullet now links into the new reference anchor.

As with the wavelet pass, writing it turned up real bugs — five this time,
logged separately below ("transport Stop permanently silenced four built-in
synths", and the three Spectral Grain defects).

**Third surface, deliberately not done: `docs/` has no granular tutorial.**
Neither node is mentioned on any tutorial page. Nothing in this pass *required*
one — the fixes changed no documented workflow, since there was no documented
workflow — but "make an evolving pad with a grain cloud" is a good fit for a
task-oriented page next to `wavelet-effects.html`, and the Density × Grain Size
interaction is exactly the sort of thing a tutorial teaches better than a spec
table. If written, add it to `MainContentComponent`'s Help menu and
`docs/index.html`.

**Correction to this entry's original rationale.** It claimed the 1024-grain
ceiling "*is* user-observable, in that Density past the point where the cloud
saturates stops making it denser." **That is false and the docs must not repeat
it.** Steady-state grain count is `Density × GrainSize × voices`, so at the
param maxima the reachable ceiling is 100 grains for Particle Cloud
(200/s × 0.5 s, monophonic) and 320 for Spectral Grain (200/s × 0.2 s × 8
voices) — 3–10× under the cap. A Signal cable can't get there either:
`applySignalModulations` ends with `p.value = std::clamp(modVal, p.minVal,
p.maxVal)`, so a cable cannot push a param past its declared range. The 1024
cap is an audio-thread safety net that is unreachable by an order of magnitude,
and it is documented as such.

---

## FIXED (2026-08-06, c6d6345): pressing transport Stop permanently silenced four built-in synths

**Severity: this was the worst bug in the tree.** One press of **Stop** made FM
Synth, Phase Distortion Synth, Additive Synth and Spectral Grain go silent for
the *rest of the session*, with no error and no way to recover short of
deleting and re-adding the node.

Transport panic calls `AudioProcessorGraph::reset()`, which calls `reset()` on
every processor. All four implemented that as `voices.clear()` — but their voice
pool is sized **only in the constructor** (`voices.resize(16)` / `resize(12)` /
`resize(8)`); `prepareToPlay` never refilled it. So after Stop:

1. `allocVoice()` scanned an empty vector, found no free voice;
2. fell through to its steal path, which computes `idx = 0` and does
   `return voices[idx]` — **out of bounds on an empty vector**, writing the new
   note's state into the freed-but-still-owned buffer;
3. the render loop's `for (auto& v : voices)` then iterated **zero** voices.

Silent, permanent, and undefined behaviour on the audio thread.

**Fix:** `reset()` now resets the voices in place (`for (auto& v : voices) v =
Voice{};`) rather than removing them, each class grows a `kMaxVoices` constant
so the pool size has one definition, and `allocVoice()` refuses to index an
empty pool. Particle Cloud had the mirror-image defect — its `reset()` cleared
the grain list but left `noteAmpEnv` running, and the spawn loop is gated on
that envelope, so for a held note panic silenced the cloud for a fraction of a
millisecond and then let it grow straight back. It now calls
`noteAmpEnv.hardReset()` too.

Guarded by twelve assertions (`panic/fmsynth:`, `panic/pdsynth:`,
`panic/additive:`, `panic/particlesynth:`) covering both halves for each synth:
panic really silences a held note, *and* the synth still plays afterwards. Both
defects were reinstated to confirm the tests catch them (the three
`voices.clear()` synths read exactly 0.00000 for "still plays after a transport
Stop"; Particle Cloud reads 0.39244 for "actually silences a held note").

**Related gap, still open:** none of the synths in `builtin_effects.h` handle
MIDI **All Notes Off** / **All Sound Off**, though `builtin_synth.cpp`,
`drum_synth.cpp`, `multi_sampler.cpp`, `poly_voice_processor.cpp`,
`soundfont_processor.cpp` and `terrain_synth.cpp` all do. `GraphProcessor`
emits `allNotesOff` on every channel when the transport stops
(`graph_processor.cpp:94`), so those synths ignore it and rely entirely on the
panic path above. That's fine for Stop but wrong for any other source of an
All Notes Off (a controller's panic button, an incoming MIDI file). The fix is
a shared helper — the message means "release every held note" — applied to each
MIDI-accepting processor in `builtin_effects.h`.

---

## FIXED (2026-08-06, 7146029): Spectral Grain advanced every grain once per VOICE instead of once per sample

`activeGrains` is a single pool shared by all voices, but the grain render loop
sat **inside** the per-voice loop. With V voices sounding, every grain's read
position was stepped V times per sample, so each grain played V× too fast (V×
shorter, and at V× the pitch ratio) and was summed V times. And because
`g.rate` is baked from the *spawning* voice's pitch while every voice rendered
every grain, a chord came out as one smeared pitch rather than distinct notes.
It was also `O(V·G)` where `O(G)` would do.

**Fix:** `ActiveGrain` carries a `voiceIdx`, and the render is two passes per
sample — advance the voices into a small `voiceGain[kMaxVoices]` array, then
walk the grain pool **once**, scaling each grain by its owning voice's gain.
`allocVoice()` drops a recycled slot's leftover grains so a new note can't
inherit the previous note's cloud.

Guarded by `spectralgrain: grain lifetime is independent of how many voices are
sounding`. The observable is grain **lifetime** via the steady-state cloud size
(`grains = voices × Density × GrainSize`), which is identical either way for one
voice — that's the control assertion — and differs by the voice count for a
chord. Reinstating the bug reads 3.00 grains/voice against 10.00 fixed.

---

## FIXED (2026-08-06, 7146029): Spectral Grain's Grain Size knob was inert above 21 ms, and grain length depended on the note played

Grain length was clamped with `std::min(grainSizeSamples, grainBank[0].size())`.
The bank is `kGrainFFTSize` = **1024 samples**, i.e. 21 ms at 48 kHz, so the
entire upper 90% of the Grain Size range (1–200 ms) did nothing at all. Worse,
a grain played *above* A4 reads the bank faster than 1 sample/sample, so it ran
off the end and died early — **grain duration depended on which note you
played**, which is not something a "Grain Size" control should do.

**Fix:** bank entries are inverse *real* FFTs of a full spectrum and are
therefore exactly periodic with period `kGrainFFTSize`, so the read now wraps
(`(int)g.pos % wave.size()`) and loops seamlessly. Grain length is whatever the
knob asks for, at any pitch, with no click. The Hann window is taken over the
grain's own length rather than the bank's.

Guarded by `spectralgrain: Grain Size keeps scaling past the FFT frame`.
Reinstating the clamp makes the 100 ms / 50 ms grain-count ratio read exactly
1.00000 (both sides pinned at 2 grains) against 2.00000 fixed.

---

## FIXED (2026-08-06, 33060be): Wavelet Reverb's Decay was applied per BLOCK, so the tail depended on the audio buffer size

**Fix:** the per-block coefficient is now derived from the block size —
`blockDecay = powf(decay, 16.0f * n / tailLen)` — so a sample accumulates
`decay^16` over a full traversal of the buffer at *every* block size. The
exponent 16 was chosen so that at the common 512-sample buffer the coefficient
comes out as plain `decay`: projects made before the fix sound unchanged, and
every other buffer size now matches them instead of diverging. `decay >= 1` is
short-circuited to exactly 1 so the existing neutrality test stays bit-exact.

Guarded by `wavelet-fx: Reverb decay is independent of the audio buffer size
(512 vs 64 samples)` in `self_test.cpp`. Writing that test was considerably
harder than the fix, and the reasoning is recorded in a long comment above it —
three plausible measurement designs each turned out to measure something other
than the decay (the node doesn't ring, so there's no fade to watch; its raw
output level is block-size dependent by design, because the readout window sits
in the transform's boundary region; and the attenuation staircase splatters
broadband energy when multiplied into a continuous tone, which made Decay = 0.9
measure *louder* than Decay = 1). The version that works feeds an impulse and
skips its first 1024 samples, so the reading isn't dominated by output the decay
hasn't acted on yet. Verified by reinstating the bug: it reads 1.09 against the
fixed code and 122204 against the broken code.

**Not fixed:** the tail is still hard-bounded at 8192 samples (≈171 ms at
48 kHz) regardless of Decay, which is short for a reverb, and Decay is still a
dimensionless 0–1 knob rather than a time in seconds. Sizing the buffer from a
requested decay time remains the natural follow-up. Original report below.

---

**Found:** 2026-08-06, while writing the REFERENCE.md wavelet section — the
neutral setting (Decay = 1) had to be explained, which meant working out what
Decay actually does.

`WaveletReverbProcessor::processBlock` (`builtin_effects.h`) ages the tail with

```cpp
for (int i = 0; i < tailLen - n; ++i)
    tail[i] = tail[i + n] * decay;
```

Each sample migrates left by `n` (the block size) once per block and is
multiplied by `decay` once per block. Over its life in the 8192-sample buffer a
sample is therefore attenuated `tailLen / n` times — **16 times at a 512-sample
buffer, 128 times at a 64-sample buffer.** At Decay = 0.9 that is 0.9^16 ≈ 0.19
versus 0.9^128 ≈ 1.4e-6: the same knob position is a usable ambience on one
audio device and effectively dry on another. Nothing else in SEANCE has
buffer-size-dependent audible behaviour, and it makes the node impossible to
preset or to reproduce between machines.

**Repro:** add a Wavelet Reverb at Decay ≈ 0.9, Mix 1.0, play a percussive
source, then change the audio device buffer size in Preferences. The tail
shortens dramatically as the buffer gets smaller.

**Proper fix:** make Decay a **time**, not a per-shift multiplier. Expose it as
an RT60-style decay in seconds (or keep the 0–1 knob and map it onto one), and
derive the per-block coefficient as `powf(targetGain, (float)n / (float)tailLen)`
— or better, `expf(-(float)n / (decaySeconds * sampleRate))` — so the audible
decay is invariant under block size and sample rate. The self-test's
`Decay = 1` neutrality case still passes under either formulation (a decay time
of infinity gives a coefficient of exactly 1), so the existing test stays valid;
add a new one that runs the same input at two block sizes and asserts the tail
envelopes match.

Note the tail is also hard-bounded at 8192 samples (≈171 ms at 48 kHz)
regardless of Decay, which is short for a reverb. If the decay-time rework
happens, sizing the buffer from the requested decay time is the natural
companion change.

---

## MOSTLY FIXED (2026-08-06, be38ce9): Asymmetric Filter's Pre-Attack / Post-Decay were labelled in ms but indexed in coefficients

**Fixed — problem 2, the wrong axis.** The gain envelope is now built along the
**time** axis in samples, from onset positions converted out of finest-band
coefficient index (× 2, that band's stride), and then resampled onto each band by
that band's own stride. Each coefficient takes the **mean** of the envelope over
the span of time it represents rather than a point sample: the coarse bands
stride up to `2^Levels` samples at a time, so point sampling would alias a short
pre-attack ramp into them or step straight over it. A `double` prefix sum makes
each of those means an O(1) subtraction, so the pass stays allocation-free and
the CPU budget test is unchanged (100× realtime).

Guarded by `wavelet-fx: Asymmetric Filter's pre-attack lands before the onset,
not at the start of the block`. Verified by reinstating the bug: it reads 0.00
against the fixed code and 0.28 against the broken code, and the broken version's
in-region figure collapses from 48.8 to 2.1 — i.e. the old code applied most of
its gain nowhere near the onset it had just detected.

**Also fixed:** the hardcoded `0.5 * maxFine` onset threshold is now a
**Sensitivity** param (0–1, default 0.5 = the old constant, so existing projects
are unchanged). It stays a fraction of the block's own peak rather than an
absolute level, so it doesn't need re-dialling between a quiet and a loud take.

**Still open — problem 1, the range bound.** `preSamples`/`postSamples` are now
*clamped* to the padded block length rather than silently overflowing it, so the
behaviour is honest, but the underlying limit is unchanged: the analysis window
is one audio block, so at 48 kHz / 512 samples nothing above ≈10.7 ms can extend
any further, and where the knob flattens still moves with the device buffer size.
Clamping is not the real fix and was not intended as one. **The real fix is a
fixed-size analysis frame decoupled from the device buffer** — buffer input into
(say) 4096-sample frames, process each frame whole, and report the framing
latency to PDC. Since the DWT round-trip is exact, overlap-adding at 50% hop with
sqrt-Hann analysis and synthesis windows would reconstruct the neutral setting
bit-exactly (sqrt-Hann² = Hann, which is COLA at that hop), so the existing unity
test would survive the rework — and it would also remove the per-block envelope
discontinuities that exist today at every block boundary.

Original report below.

---

**Found:** 2026-08-06, same pass as the entry above.

`AsymmetricFilterProcessor::processBlock` converts the two time knobs with
`preSamples = (int)(preMs * 0.001 * sampleRate)` and then uses those counts as
**indices into `gainEnv`**, which is `padLen` entries long and spans the
concatenated wavelet coefficient array. Two things are wrong with that:

1. **The counts overflow the block.** At 48 kHz the default Pre-Attack of 20 ms
   is 960 samples, but a 512-sample block pads to 512 coefficients — the
   pre-region already covers the entire block. Every Pre-Attack setting above
   roughly `1000 * blockSize / sampleRate` ms (≈10.7 ms at 512/48 k) behaves
   identically, so most of the knob's travel is dead and where the dead zone
   starts moves with the audio buffer size.
2. **The coefficient axis is not a time axis.** Onsets are detected in the
   finest detail band (`t = i - finestStart`, so `t` counts *finest-band*
   coefficients, each worth 2 input samples) but `gainEnv` is indexed across the
   whole array, where an index in the approximation band is worth `2^Levels`
   input samples. So a "20 ms pre-attack region" is neither 20 ms nor
   consistently scaled across bands, and small `t` values land in the
   approximation region rather than near the onset.

The node still sounds like something useful — that is why this is a
correctness/labelling bug rather than a broken-feature bug — but the params
don't mean what they say, and it can't be presetted reliably.

**Proper fix:** build the gain envelope in the **time domain** at the onset
positions (converted out of finest-band coefficient index by multiplying by 2),
then map that envelope onto each band by decimating it by that band's stride, so
one wall-clock millisecond covers the same wall-clock span in every band. Clamp
`preSamples`/`postSamples` to the padded block length and note in the tooltip
that the usable range is bounded by the audio buffer size — or, better, keep
enough history to make the ms values honest independent of block size. While in
there: the onset threshold is hardcoded at `0.5 * maxFine`; it should be a
Sensitivity param.

**Documented meanwhile:** `REFERENCE.md` → Wavelet effects → Asymmetric Filter
carries both caveats explicitly, telling the user to treat the two knobs as
shape controls rather than literal milliseconds.

---

## TECH DEBT (architectural): processors hold `Node&`; params should come from an APVTS

**Noticed:** 2026-08-05, auditing the `Node&` lifetime question. **Not currently
broken** — this is a fragile pattern that is *held* safe by runtime discipline,
plus the enabler for extracting effects as standalone plugins.

**Stage 1 landed 2026-08-05:** `NodeGraph::nodes` is now a **`std::deque<Node>`**
instead of a `std::vector<Node>`. deque guarantees that `push_back` /
`emplace_back` never invalidate references or pointers to existing elements, so
the *growth* half of the hazard — which is what all three shipped crashes below
actually were — is now impossible by construction rather than prevented by
discipline. What remains is under "Why it is still debt".

**The pattern.** Every native processor takes and stores a reference into the
graph's node container — e.g. `ConvolutionProcessor(Node& node)` with a
`Node& node;` member; ~37 effect classes in `builtin_effects.h` alone, plus the
synths, signal nodes and analyzers. `GraphProcessor::createNodeProcessor` hands
out these references and the processor reads its parameters off the `Node` live
on the audio thread every block. This still violates the spirit of the CLAUDE.md
rule "never store `Node&` across call boundaries — store `int nodeId` and look it
up"; the deque makes the pattern survivable, not correct.

**It has already caused three shipped crashes** — all three were vector growth,
and all three are structurally fixed by the deque: `SEANCE.exe.63000.dmp` (MOD
import `addNode` loop), `.118460.dmp` (new MIDI timeline — reallocation
move-constructed every Node, nulling the moved-from `shared_ptr`, and the audio
thread locked a null `*node.mpePassthroughMutex`), `.80308.dmp` (deleting the
wavetable node).

**Why it is safe right now** (verified 2026-08-05, all paths re-checked):
- `nodes` is a `std::deque`, so **appending a node never moves an existing one**
  and no outstanding `Node&` is invalidated by graph growth. Note deque does
  *not* save you from mid-container `erase` or from `clear()`.
- `NodeGraph::mutationLock` is a `recursive_mutex`; the audio callback takes a
  **try-lock** and emits silence rather than blocking.
- `addNode()` / `addLink()` lock internally, so even a single interactive add is
  covered without the caller remembering.
- All five structural mutation sites hold the lock: `node_graph_component.cpp`
  :5397 and :5565 (`deleteNodeAndDescendants`, held to end of function),
  `scripting.cpp:834`, `main_window.cpp:3265` (new project),
  `project_file.cpp:637` (load, via the `onLoadSnapshot` lock).
- The rebuild trigger in `GraphProcessor::processBlock` is
  `rebuildRequested || nodes.size() != lastNodeCount || links.size() != lastLinkCount`.
  A mutation that destroys elements but leaves the **count unchanged** would slip
  past the size heuristic and strand every processor on freed storage. Undo is
  exactly that shape (snapshot restore does `nodes.clear()` + repopulate; undoing
  a rename or param tweak keeps the count identical) and the deque does *not*
  help there, because `clear()` destroys everything. **That hole is closed**
  because `main_window.cpp`'s `onLoadSnapshot` ends with an explicit
  `requestRebuild()`. This is load-bearing — if that call is ever removed, undo
  becomes a use-after-free.

**Why it is still debt.** The deque removes the growth hazard, but node *removal*
(`erase`, `clear()`) still invalidates references, so safety on those paths still
depends on every future mutation site remembering `mutationLock` and on the
rebuild being triggered, with no compile-time enforcement. The adjacent race is
*still open* — see "node pin-vector mutations don't hold `mutationLock`" below,
where several editors mutate `pinsIn`/`pinsOut` unlocked. And `Node` remains
shared mutable state in *both* directions: `applySignalModulations`
(`signal_modulation.h:49`) writes back into `node.params[]` from the audio thread
while the UI reads the same fields.

**Stage 2 (not started).** Migrate processors to owned params. Scope measured
2026-08-05: 131 `Node&` constructor sites, 75 `Node&` members across 34 files,
164 `paramByName()` call sites (which are also a linear *string-compare scan on
the audio thread*, `builtin_effects.h:19`), 51 `applySignalModulations()` sites.
Too large to do atomically against a green 1020-test build — pilot the pattern on
two or three processors first, including one that uses signal modulation, and
settle how the UI reads back the audio-thread-written modulated value before
grinding through the rest.

**Proper fix.** Stop reading parameters through a `Node&` entirely: give each
processor a `juce::AudioProcessorValueTreeState` (or an equivalent param
abstraction) owned by the processor, and have the graph *push* values into it on
edit rather than having the audio thread *pull* them out of a shared vector. The
processor then owns its state, holds no reference into `graph.nodes`, and the
whole reallocation hazard class disappears rather than being guarded.

**Why this is worth doing beyond the race.** This is the same refactor required to
ship any effect as a standalone VST3/AU. The DSP already subclasses
`juce::AudioProcessor` — which is exactly what a plugin wrapper needs — so the
only things binding it to the app are (a) the `Node&` parameter source and (b) the
editors living in the app's component hierarchy. Doing this once removes the
crash-hazard class *and* unblocks plugin extraction. Sequence it before any
serious plugin-productization work so the extraction isn't done twice.

---

## NEEDS MANUAL VERIFICATION: hosted-plugin (VST3/AU) editor knob-drag recording

**Implemented** (phase 5) but **not covered by self-tests** — it can only be
exercised by dragging a real plugin's knobs, so it needs a manual pass with an
actual VST3.

**How it works.** Because hosted plugin params are *not* SEANCE native param rows
(they live behind JUCE's `AudioProcessorParameter` interface, read live from the
processor), they have no `Param::automation` lane. Their recorded lanes instead
live in `Node::pluginParamAutomation` (a `std::map<int,AutomationLane>` keyed by
plugin-param index, values normalized 0..1) with transient rec state in
`Node::pluginParamRec`. The listener is the already-attached
`GraphProcessor::LatencyChangeListener`, extended to queue
`audioProcessorParameterChangeGestureBegin/End` + `audioProcessorParameterChanged`
events (`drainParamEvents`). The UI timer drains them (`processPluginParamEvents`),
flips per-param writing flags per the node/global cascade, samples the live
normalized value each tick, and on stop simplifies + commits one snapshot. Read-back
pushes lane values through `applyAutomation` (`setValue`, which does NOT notify
listeners, so playback can't feed back into the recorder). Save/load via
`pluginAuto=` node lines; undo via the same `serializeForUndo` path.

**Known caveat (gesture-less plugins).** Plugins that change a param via
`setValueNotifyingHost` *without* bracketing `begin/endChangeGesture` still record:
Touch arms on the first `parameterChanged` and ends via a 250 ms idle timeout
(`processPluginParamEvents` / the timer). This is heuristic — a plugin that streams
continuous changes then pauses mid-drag could clip the tail. Latch/Write are
unaffected (they run to Stop regardless).

**MIDI-learned CC moves are also captured.** A learned CC that drives a plugin
param is applied on the audio thread via `setValue` (no listener callback), so it
can't arrive through `drainParamEvents`. Instead the audio callback captures each
matched CC target into `AudioEngine::ccRecTouched` (drained by
`drainCcRecTouched`); `processPluginParamEvents` treats each touch like a
gesture-less `parameterChanged` (arms Touch/Latch, ends Touch on the 250 ms idle
timeout), and the timer samples the already-CC-driven live value. Same caveat as
gesture-less plugins applies to the Touch tail.

**To verify manually:** load a VST3, Auto→Touch, Play, drag a knob in the plugin's
own window, Stop, rewind, Play → the knob should retrace the move. Check per-node
red recording outline appears while dragging.

---

## OPEN (UX/naming): "auto-cache" menu label implies auto-freezing, which it doesn't do

**Noticed:** 2026-07-07, same cache trace. The node right-click menu offers
"Enable auto-cache" / "Disable auto-cache" (`node_graph_component.cpp:4509`,
toggling `cache.autoCache`, default `true`). The name reads as "automatically
freeze this node for me," but `autoCache` never renders a cache — nothing in the
codebase autonomously freezes a node. `autoCache` is purely a **reuse +
hash-invalidation gate**: it lets an *already-existing* cache be reused and
auto-invalidated when inputs change (`AudioCacheManager::isCacheValid`,
`audio_cache.cpp:162`). The only thing that ever *produces* a cache is the
explicit "Freeze (cache audio)" command (or the Output-mix capture on stop,
which is excluded from live substitution). In practice the hash-checked branch
(`graph_processor.cpp:816`) barely fires for live interior-node playback at all;
its real beneficiaries are offline export and the capture-from-song dialog,
which use it to skip a redundant re-render.

**Proper fix:** rename the toggle to something honest, e.g. "Reuse freeze until
inputs change" (or fold it into the Freeze submenu as "Auto-invalidate on
edit"), and update the tooltip to say it controls *reuse/invalidation*, not
auto-freezing. No behavioral change needed — purely label + tooltip. Update
REFERENCE.md's cache section in the same commit.

---

## OPEN (dead code): measured-HRTF loaders for the 3D Spatializer are never called

**Noticed:** 2026-06-30, while writing the REFERENCE.md section for the 3D
Spatializer. The README lists "Measured HRTF datasets (SOFA / WAV)" under
**Roadmap** (correct — it's not shipped), but there are two half-written loader
functions that look like the feature is partly built and could mislead a future
reader into thinking it just needs hooking up:

- `Spatializer3DProcessor::loadHrtfDirectory` (`spatializer_3d.cpp:97`) — parses
  `hrtf_az{N}_el{M}_L/R.wav` files into `measuredHrtfs`, plus
  `selectMeasuredHrtf` (`:152`) to pick the nearest entry.
- `HRTFTable::loadFromDirectory` (`hrtf_data.cpp:193`) — a *different* loader for
  MIT-KEMAR-style `H{elev}e{azimuth}a.wav` names, with `hasExternalData()`.

**Nothing calls any of them** (verified by grep): no UI, no menu item, no
project-file hook. `Spatializer3DProcessor::processBlock` only ever calls
`HRTFTable::instance().lookup()` (the synthetic spherical-head model), so even
if `measuredHrtfs` were populated it would be ignored — `selectMeasuredHrtf`
writes `currentIR_*` but `processBlock` immediately overwrites them from the
synthetic table each block. There is also **no `.sofa` parser at all** despite
the Roadmap wording.

**Proper fix (when the Roadmap item is built):** decide on ONE loader path
(fold the spatializer-local one into `HRTFTable` so the convolution actually
reads measured IRs), add a real `.sofa` reader, and wire a "Load HRTF dataset…"
action onto the node's right-click menu + a project-file field so the choice
persists. Until then, the two stubs are dead weight — either build the feature
or delete the stubs so they stop implying it's half-done.

---

## OPEN (latent): node pin-vector mutations don't hold `mutationLock`

**Noticed:** 2026-06-23, while fixing the new-MIDI-timeline crash
(`SEANCE.exe.118460.dmp`). That crash — `MidiInputProcessor::processBlock`
locking a null `*node.mpePassthroughMutex` — was a data race between the audio
thread (holding a `Node&` into `graph.nodes`) and a GUI-thread
`graph.nodes.push_back()` that **reallocated** the vector, move-constructing
every Node and nulling the moved-from `shared_ptr` members. Fixed by (a) making
`NodeGraph::mutationLock` a `std::recursive_mutex`, (b) locking inside
`addNode()`/`addLink()` so even a single interactive add is covered, and (c)
locking the remaining `graph.links.erase` sites in the editors.

**Still open:** the audio thread holds `mutationLock` for the *entire* callback,
including `GraphProcessor::rebuildGraph`, which reads each `node.pinsIn` /
`node.pinsOut` (pin counts + `pin.kind` in `widenForControl`). Several editor
operations mutate a node's pin vectors (`nd->pinsIn.erase` /
`nd->pinsOut.erase`, and `nd->pinsIn = std::move(newPins)`) **without** taking
the lock — e.g. `control_bank.cpp` (remove slider), `spectrum_tap.cpp`,
`midi_mod_node.cpp` (resize signals), `layered_wave_editor.cpp` (drop mod
pins). Lower crash-risk than the Node case (`Pin` shifts in place; the audio
thread reads `pin.kind`, an enum, not `pin.name`), but still a torn-read race.
**Proper fix:** wrap those pin-vector mutations in
`std::lock_guard<std::recursive_mutex>(graph.mutationLock)` too — the lock is
recursive so nesting under a batch caller is safe. Most of these functions
already lock the adjacent `links.erase`; widen that scope to cover the pin
erase. Not done yet to keep the crash fix scoped.

---

## OPEN (latent): baked-chain "Pin" checkbox shows but does nothing

**Noticed:** 2026-06-22, while investigating the morph-pin reports.
`warp_editor.cpp:534` sets the Pin checkbox visibility from the callbacks, but
line 541 `addAndMakeVisible(*row.mod)` force-overrides it to visible — so on a
**baked** warp chain (callbacks unset, e.g. a granular/spectral per-element
editor) the Pin checkbox SHOWS but its onClick early-returns
(`if (!cb.setModulated) return;`) → "clicking does nothing." Harmless on the
frame-scope Summation Morph editor (callbacks wired). Fix: use
`addChildComponent` + explicit `setVisible(cond)` so unwired editors hide the
box. Low priority (cosmetic on editors that have no pinnable params anyway).

---

## TECH DEBT / LANDMINE: incremental build can silently leave an ODR layout mismatch (heap corruption)

**Hit:** 2026-06-19. SEANCE was crashing at startup with
`STATUS_HEAP_CORRUPTION (0xC0000374)`. Root cause was **not** a source bug — it
was a **stale incremental build** producing an ODR / object-layout mismatch:

- `terrain_synth.h` was changed (commit `7f62357`, which grew
  `TerrainSynthProcessor` — `sizeof` is now `0x3600`, last member `partialBank`
  at offset `0x35C0`).
- `terrain_synth.obj` recompiled with the new layout, but
  `graph_processor.obj` (which `#include`s `terrain_synth.h` and does
  `make_unique<TerrainSynthProcessor>` in `rebuildGraph`) was **never
  recompiled** — its `.obj` was 10 h older than the header. MSBuild's header
  dependency tracking missed it; a plain `cmake --build` only relinked.
- Result: `make_unique` allocated the **old, smaller** `sizeof`, the freshly
  built constructor wrote `partialBank` past the old end → heap buffer overflow
  → corruption that later tripped the heap manager on an unrelated free
  (the original dump faulted in `PluginHost::loadScanCache`, a red-herring
  *victim* of the corruption, not the cause).

**Fix that was applied:** full clean rebuild
(`cmake --build build --config Release --target SEANCE --clean-first`) so every
TU shares one layout. Verified under Application Verifier full page heap: the
overflow faulted instantly at `terrain_synth.cpp:1300` before the rebuild, and
SEANCE ran cleanly (audio thread live, TerrainSynth node constructed) for 75 s
with page heap on after the rebuild.

**Why it's still tracked:** the *trigger* (MSBuild not recompiling a `.cpp`
when a core header it includes changes) can recur and is silent + catastrophic
(memory corruption, not a compile error). Mitigations / proper fix to consider:

- **Rule of thumb until understood:** after editing a widely-included core
  header (`terrain_synth.h`, `node_graph.h`, `warp.h`, `transport.h`, …) or after
  any interrupted build, do a `--clean-first` rebuild before trusting a run.
- Investigate why MSBuild's `.tlog` dependency tracking dropped
  `graph_processor.cpp`'s dependency on `terrain_synth.h` (possible corrupted
  incremental state from a build killed mid-flight).
- Consider a cheap guard: a `static_assert(sizeof(TerrainSynthProcessor) == …)`
  is *not* cross-TU safe, but adding a one-line `extern` size probe, or
  preferring out-of-line factory functions in a single TU, would localize
  allocation+construction so the two can't disagree. Lower priority than just
  remembering to clean-build after core-header edits.

---

## BUG (minor): pinned generator param leaks a dead pin when the layer changes shape

**Found:** 2026-06-16, adding live modulation for generator extra-params
(`shapeParam` / `shapeParam2`, layer-field codes 2 and 3) in the layered wave
editor.

The per-layer **Pin** checkbox for a generator's extra knob (Pulse *Duty*,
Sync/PhaseDist *Amount*, FM *Index* / *Ratio*) is only **visible** while the
layer is a generator shape (`morphModBtn`/`morph2ModBtn` visibility is gated on
`isGen`/`isFM` in `updateSourceControls`). If the user:

1. Picks a generator (e.g. FM), ticks **Pin** on *Index* (field 2) — this
   creates an on-demand layer-field `Param` + modulation pin, and
2. Switches the same layer back to a non-generator shape (Sine/Saw/…),

then the Pin checkbox hides but the **param + pin are not removed**. The pin
stays on the node face with nothing in the UI to untick it. It is **harmless to
the audio** — `sampleLayer` ignores `shapeParam` for static shapes, so the dead
param drives nothing — but its mere presence makes
`terrain_synth.cpp`'s `anyPerLayer` field-override gather see a pinned field and
keep the voice on the **live re-bake path** (`renderWithLiveOverrides`) every
block instead of the cheaper baked path, a small constant CPU cost until the
project is reloaded.

**Proper fix:** when a layer's shape changes away from a generator (or FM→non-FM
for field 3), call `setLayerFieldModulated(layer, 2/3, false)` for any pinned
generator field so the param + pin are torn down with the control that owned
them — mirror how switching shapes already forks `factoryRef`. Do it in the
shape-change handler in `WaveLayerEditor::showWaveSourceMenu`'s callback (and the
preset-apply path), routed through the owner so the node param is actually
removed. Low priority (audio-correct, only a perf/clutter nit).

---

## UX DECISION NEEDED: Summation Morph — "+ Add" vs the Library row, and the live-reference model

**Raised:** 2026-06-16 review of the layered-wave / morph editor.

The frame-scope **Summation Morph** editor (`WarpChainEditor` with a
`LibraryContext`) currently offers **two** ways to populate the morph stack, and
users find the relationship unclear:

- **"+ Add"** — appends ONE stage at a time, picked from the full domain-grouped
  method list (~21 Bucket-A methods). This is the "build your own" path.
- **Library row** (relabelled "Library:" from "Morph:" in this batch) — a combo
  that LOADS a whole ready-made chain. Built-in presets (`builtinMorphChains()`,
  8 curated combos) **copy** their ops in and detach to *(Independent)*; a
  user-**Saved** chain loads as a **live reference** (editing it propagates to
  every frame sharing the `warpAssetId`). "Save to Library" publishes the
  current stack.

This batch fixed the two concrete "it's broken" complaints (both were the same
relayout bug — `onStructureChanged` didn't re-run `resized()`, so a populated
stack was clipped invisible whether it came from "+ Add" or a built-in pick) and
did a non-destructive clarity pass on the labels/tooltips. **Left open** is the
deeper design question the user asked: *"I don't know what the Morph pulldown is
for — I'd think Add handles everything."*

Two candidate resolutions, both bigger than a tooltip and needing the user's
call (don't build a smaller version of the wrong thing — CLAUDE.md):

1. **Unify under "+ Add":** make "+ Add" the single entry point, its picker
   offering both individual *Methods* and ready-made *Presets* (built-in +
   saved). Drop the separate Library combo. Keeps Save-to-Library. The
   **live-reference** behaviour (edit-propagates-to-all-frames) would either be
   dropped (every load becomes a copy — simpler, matches "Add handles
   everything") or moved to an explicit "link" affordance.
2. **Keep both but make the split obvious:** "+ Add" = stages; Library = whole
   chains; surface the live-vs-copy distinction in the UI rather than only the
   tooltip.

Removing live-reference touches `warpAssetId` (serialization), `resolveWarpReferences`,
`writeBackReferencedWarp`, and the multi-frame "shared morph" feature — a real
architectural change, not a quick edit. Decide before implementing.

---

## BUG: asset library is never cleared on New Project / file load (assets merge/duplicate)

**Found:** 2026-06-15, while seeding the built-in morph chains into the library
(#3B). `NodeGraph::assets` (the project asset library) is **never cleared** on
either `File → New` or a real project load:

- `MainContentComponent::newProject()` (`main_window.cpp` ~2773) clears
  `graph.nodes/links/openEditors` but NOT `graph.assets` or `graph.contentStore`.
- `ProjectFile::readProject()` (`project_file.cpp` ~540) clears
  `nodes/links/openEditors` (and `load()` clears `contentStore` at ~530) but
  NEVER clears `graph.assets`; it only `insertRaw`s the file's `[AssetStore]`
  entries on top of whatever was already there.

Consequences:
- **New Project** keeps the previous project's user assets (waveforms,
  instruments, ADHSR curves, morph chains) in the "fresh" library.
- **Load project B after A** MERGES A's assets into B instead of replacing.
- **Undo restore** (`loadFromString` → `readProject`) re-`insertRaw`s the
  snapshot's assets without clearing, so an asset id can end up duplicated in the
  vector (two entries with the same id; `find` returns the first, `list` returns
  both).

The seeded built-in morphs are immune (seeding is idempotent by id), so this was
latent before content reliably lived in the library; it's now more user-visible.

**Proper fix** (mirror the `contentStore` pattern's real-load-vs-undo split):
- `newProject()`: `graph.assets.clear()` before `setupDefaultGraph()` (which
  re-seeds built-ins), and `graph.contentStore.clear()` too.
- Real file `load()`: `graph.assets.clear()` before `readProject` (same spot as
  the existing `contentStore.clear()` at ~530) — but NOT in `loadFromString`
  (undo restore), whose snapshots are authoritative and must replace cleanly.
  Cleanest is to clear `assets` inside `readProject` itself the way
  `nodes/links` are cleared, since undo snapshots DO carry the full asset set
  (writeProject writes `graph.assets.all()`), so clearing-then-repopulating is
  correct for both the file and undo paths. Verify a save→load→save round-trip
  and an undo/redo cycle don't grow or drop assets.

---

## Asset library + layered-wave / morph editor — bug batch (reported 2026-06-14)

A batch of issues the user found while exercising the asset library and the
layered-waveform editor. Feature requests and UI-refactor items from the same
report live in `agent-todo.md` ("Asset library + layered-wave editor — user
review 2026-06-14"); the items below are concrete BUGS. Several cluster around
the morph/warp UI and will likely be resolved together by the two-morph-type
redesign tracked in agent-todo.

- **PARTIALLY RESOLVED (library seeding — 2026-06-15): Morph Algorithms now seed;
  Waveforms + AHDSR still don't.** The original report ("Asset Library is empty")
  was *by design* — nothing seeded built-in content. **Morph Algorithms are now
  seeded** (`seedBuiltinMorphLibrary` runs on new project + load), so the Morph
  Algorithms tab and the Summation Morph picker are populated with the 8 curated
  built-in chains. **Still unseeded: Waveforms and AHDSR Curves** — the
  agent-todo "seed standard library content" item is only morph-complete. Built-in
  factory waveforms still live in `WaveformBank` (not the asset library) and
  AHDSR has no curated starter set. Keep this note until those two kinds seed too.

- **BUG (dialog self-dismiss): the waveform selection view closed by itself**
  while the user was picking waveforms to view — SEANCE did not crash/quit, just
  the picker dialog vanished. Likely an unintended close trigger (escape-key /
  focus-loss / a click handler that dismisses the dialog) in the waveform library
  browser. Repro: open the waveform picker, click through several waveforms to
  preview them. Investigate the browser's close/escape handling.

- **BUG (independent morph "add" does nothing visible — partly resolved): with
  "(Independent)" selected, clicking "add" repeatedly keeps adding input
  modulation pins to the node but never adds anything to a visible list of applied
  morphs.** The **"(Independent) is the ONLY morph choice"** half is **resolved**:
  the morph library is now seeded with the 8 built-in chains and the picker sources
  the list from it (`seedBuiltinMorphLibrary` + `rebuildLibraryCombo`). The
  **"+ Add does nothing visible"** half should also be resolved by the
  2026-06-15 fix that made **+ Add** open the method picker and append a visible
  op row (`warp_editor.cpp`), and by #3A removing the confusing per-layer chain —
  but **re-test on the frame-scope Summation Morph** to confirm an added op shows
  a row and the pin-add no longer happens without a visible stage.

- **BUG (mis-named waveform "FFT 52"): the layered-waveform dialog inside the
  wavetable editor edits a waveform auto-named "FFT 52" even though it is not an
  FFT/spectral waveform** (it's in the layered-waveform editor). Wrong default-
  name source — a layered frame is being labelled with the spectral/FFT naming
  scheme. Find where new frames get their "FFT N" name and make a layered frame
  get a layered-appropriate name.


---

## BUG: legacy Wavelet Pitch Tracker writes its signal to the wrong channel

**Found:** 2026-06-14, while building the new precise **Pitch Detector** node
(`__pitchdetector__`). The legacy **Wavelet Pitch Tracker** node
(`__pitchtracker__`, `WaveletPitchTrackerProcessor` in `builtin_effects.h`)
writes its normalized detected-pitch value to **channel 0** (`buf.clear();
auto* out = buf.getWritePointer(0); ...`) instead of the Signal-output channel
(`2 + outputPinIndex`). Every other signal-producing node writes to channel
`2 + index` (see `spectrum_tap.cpp`, the new `PitchDetectorProcessor`). Because
the node also exposes an "Audio Out" pin on channel 0, the pitch value leaks
onto the audio bus while the actual "Pitch Out" Signal pin (channel 2) carries
silence — so the node's signal output is effectively dead.

**Why not fixed now:** the node is superseded by the new Pitch Detector, which
is more accurate (true YIN/autocorrelation with parabolic interpolation vs.
octave-band wavelet resolution), wired correctly, and offers algorithm choice,
window/hop control, min/max band, and log/linear mapping. The legacy node is
kept only for backward-compatibility with old projects. **Proper fix (when
touched):** change the output write in `WaveletPitchTrackerProcessor::processBlock`
to target channel 2 (`if (buf.getNumChannels() > 2) { auto* out =
buf.getWritePointer(2); ... }`) and stop clearing the whole buffer, matching the
Pitch Detector. Audio passthrough on 0/1 can then stay intact. Consider also
adding a deprecation hint in the node's tooltip pointing users at the new node.

---

## PLANNED: generated-terrain grid export — EXR format + traversal/expression dim cap >8

**Found:** 2026-06-13, after shipping `.npz` / WAV / PNG grid export for
generated (`__generate__`) terrains (right-click node → **Export grid as ▸**).
Two parts of the originally-scoped "Export Source" task were deliberately left
for later because each is a substantial change with marginal value, and a
partial version would be a stop-gap (against the project's no-stop-gap rule).

**1. EXR export (HDR float image).** Currently 2D grids export as 8-bit
grayscale PNG (lossy) or full-precision `.npz`. A 32-bit float `.exr` would let
2D heightmaps go losslessly into VFX/image tools (Nuke, Photoshop, Blender)
that read EXR but not `.npz`. **Why deferred:** there is *no* OpenEXR/tinyexr
support anywhere in the tree, so this requires vendoring a new dependency
(tinyexr is a single-header, Apache-2.0 ~10k-line lib) — a dependency-addition
decision worth making explicitly rather than slipping in. Its value also
overlaps `.npz`, which already preserves full float precision. **Proper fix:**
vendor `tinyexr.h` under `cpp/third_party/`, add a `#include` + one `.cpp`
translation unit defining `TINYEXR_IMPLEMENTATION`, gate the export item on
rank==2 in `node_graph_component.cpp` (the `result == 196/197` block, add a
`198`), and map the float grid → single-channel (or RGB-replicated) EXR scanline.

**2. Traversal / expression axis cap >8.** Three places cap N-D terrains at 8
axes: the manual-terrain **+Dim** button (`main_window.cpp` `changeDimCount`,
`juce::jlimit(1, 8, ...)` ~line 1890, plus its `axisNames[8]` = X,Y,Z,W,V,U,S,T),
the **Lissajous** traversal (`terrain_synth.cpp` ~line 864 `d < 8` indexing
`std::array<AxisParams, 8> axes` in `terrain_synth.h` ~line 287), and the
**Builtin expression** evaluator (`terrain_synth.cpp` `fillFromExpression`,
`float vars[8]` ~line 192, 8 named axis letters, loop `d < std::min(nd, 8)`
~line 266). Note the *generate-script* path (Lua/Builtin per-cell `fillFromScript`)
is already **unbounded** — it binds `c0..c{nd-1}`, so high-rank *generated*
grids already work; only the manual-pin flow, Lissajous, and the legacy
expression fill are capped. **Why deferred:** the proper fix spans all three
subsystems at once (a partial raise — e.g. UI to 16 while Lissajous still
indexes an 8-array — would be an out-of-bounds bug), and the value is marginal
(>8-axis manual terrains are exotic; Lissajous per-axis params aren't even
exposed in any UI yet, and aren't persisted). **Proper fix:** change
`std::array<AxisParams, 8>` → `std::vector<AxisParams>` sized to `numDims`
(it isn't serialized, so no save/load impact), make the Lissajous loop run to
`numDims`, extend the Builtin parser to index `c0..cN` (or numeric var names)
past the 8 letters, and raise the `changeDimCount` cap. Also reconcile with the
*separate* "Max 8 axes" limit in the Layer/wavetable arrangement editor
(`layered_wave_editor.cpp` ~lines 6213/6520), which is its own subsystem.

---

## TECH DEBT (partially resolved): baked blobs copied into undo snapshots — video/wavetable still inline

**Found:** 2026-06-13, implementing "bake the generated terrain data into the
node" for the Terrain-Synth *Generate from program* feature.
**Generated-terrain case fixed:** 2026-06-13 via the `ContentStore`.

**Original problem:** baking a computed grid into the node's `__generate__`
script (and likewise `__video__` / `__wavetable*__`) meant
`NodeGraph::commitSnapshot` → `serializeForUndo(gp=nullptr)` copied every
node's multi-MB blob into a new undo snapshot string on every structural edit.
O(total cells) per snapshot per edit; heavy editing ballooned in-memory undo
history. Debt, not a live bug — snapshots are plain RAM strings and save/load
stayed correct.

**Fix shipped for generated terrain:** a content-addressed side store
(`content_store.{h,cpp}`, `NodeGraph::contentStore`). The canonical form is a
`.npy` payload hashed with a 128-bit content hash (two FNV-1a lanes +
splitmix64 avalanche → 32 hex); the stored form is a 4-byte-plane shuffle +
DEFLATE (JUCE gzip). `__generate__` scripts now carry `#<hash>` in their 4th
field instead of the inline blob; `TerrainSynthProcessor` resolves the grid
from the store at construction. `serializeForUndo` passes `includeBlobs=false`
so snapshots carry only the hash, never the bytes — the bytes live once in the
in-memory store across undo/redo. Real saves (`writeProject`,
`includeBlobs=true`) emit `[Blob]` sections; `readProject` loads them back.
Legacy projects with inline gzip+base64 blobs still decode (null-store path).
Identical grids dedup to one entry. Covered by the `store:` self-tests (all
161 pass).

**Still outstanding (the remaining debt):** **video (`__video__`) and
wavetable (`__wavetable*__`) blobs are NOT yet migrated** — they still inline
their bytes into the script and therefore still bloat undo snapshots. The
mechanism to fix them now exists; the work is to route their bake/parse paths
through `contentStore.putFloatGrid` / `getFloatGrid` (or a bytes-oriented
`insertRaw`/`get` for non-float PCM) and emit `#hash` refs the same way.
`ContentStore` is float-grid-oriented today; decoded video is `uint8` RGBA and
wavetable PCM may want a raw-bytes put/get pair — add those overloads when
migrating. Until then this entry stays open.

---

## TECH DEBT: Python can't be truly launch-optional (load-time-linked, not dynamic)

**Found:** 2026-06-13, making Python an optional dependency.

**What it is:** A build configured *with* Python links the interpreter at
**load time** (the normal `python3XY.lib` import library) and copies
`pythonXY.dll` next to the exe. This means that specific build needs that DLL
present at process start: if the bundled `pythonXY.dll` is *deleted*, SEANCE.exe
fails to launch with a Windows loader error. The `ScriptEngine::pythonAvailable()`
probe (`LoadLibrary`, cached) only protects against Python *features* being used
when the interpreter is stale/mismatched/broken — it cannot save a launch when
the load-time-linked DLL is gone. (A build configured *without* Python is
genuinely launch-optional — it compiles `scripting.cpp` to stubs and never
references the DLL.)

**Why not just delay-load it:** MSVC `/DELAYLOAD:pythonXY.dll` fails at link
time with `LNK1194` — CPython's public API imports *data* symbols
(`Py_None`→`__imp__Py_NoneStruct`, `PyFloat_Type`, the `PyExc_*` exception
objects), and the delay-load helper can only thunk *function* imports.

**Proper fix (substantial):** to make a Python-enabled build also launch when
its DLL is absent, resolve every Python symbol by hand via `GetProcAddress`
(including the data exports, which `GetProcAddress` *can* return) behind a thin
wrapper layer, OR split the interpreter into a separately-`LoadLibrary`-d
plugin DLL that the core never load-time-links. Both are a meaningful chunk of
work given how widely the Python C-API is used across `scripting.cpp`. Until
then the practical answer is "ship the DLL," which the build does.

---

## REQUIRED EXTERNAL PATCH: JUCE WASAPI plain-IEEE-float capture fix

**Found:** 2026-06-11, diagnosing a Logitech C615 webcam mic that recorded as
square-wave garbage under Windows Audio (WASAPI) while DirectSound was clean.

**What it is:** stock JUCE 8.0.12 misclassifies any capture device that reports
its shared-mode mix format with the **plain** `WAVE_FORMAT_IEEE_FLOAT` tag
(0x0003) — instead of the `WAVE_FORMAT_EXTENSIBLE` + IEEE_FLOAT subformat — as
integer PCM. It then decodes genuine 32-bit float samples through the Int32
converter, corrupting every sample (square-wave garbage that also bakes into a
frozen capture). `juce_WASAPI_windows.cpp` `tryInitialisingWithBufferSize`,
`isFloat` detection (~line 919).

**Status:** FIXED via patch, but the fix lives **outside** our repo (it edits
the shared JUCE install at `D:/JUCE-8.0.12`), so it is **lost on any JUCE
reinstall/upgrade and must be re-applied.** The patch and apply instructions are
in `patches/juce-wasapi-ieee-float-tag.diff` and `patches/README.md`. This entry
stays until the fix is upstreamed into JUCE itself (then delete it). Ideally
also submit the fix upstream to the JUCE project.

---

## FEATURE GAP: project tuning (microtuning) never reaches VST3/AU instruments

**Found:** 2026-06-10, while adding note-name support to MIDI scripting (the
scale/frequency discussion).

**What it is:** SEANCE has a project-global **tuning system** (`tuning.h`:
Equal12 / Pythagorean / JustIntonation / Meantone, each a per-pitch-class cents
table) plus concert pitch, surfaced via `Transport::noteToFreq`. The **built-in
terrain synth honors it** (`terrain_synth.cpp` ~1401 calls
`transport.noteToFreq(noteNumber)`, applying the cents). But **third-party
VST3/AU instruments** are fed plain `MidiMessage::noteOn` events and play
standard 12-TET — they never see the project's cents offsets. So selecting a
non-equal temperament has no effect on hosted plugins.

**Not to be confused with scales.** Scales/keys (Major/Minor/Dorian/…,
`music_theory.*`) are a *compositional* constraint (piano-roll snapping, degree
system, Change-Key) and correctly do NOT change frequencies. This entry is
strictly about *tuning/temperament* reaching instruments, not scales. A scale
selection working without instrument awareness is by design and is fine.

**Proper fix (not yet done):** a per-instrument tuning bridge that detunes each
note to the project tuning, preferring the cleanest path the target supports:
1. **VST3 native** — `NoteOnEvent::tuning` (cents) / note-expression tuning. No
   channel juggling; honored by plugins that implement it (Surge, Vital, …).
2. **MPE-style per-note pitch bend** — one note per MIDI channel + 14-bit bend
   (≈0.024 cents/step at the default ±2-semitone range, so precision is a
   non-issue; the real cost is channel allocation / 15-voice polyphony). Works
   with essentially any plugin that responds to pitch bend.
3. **MTS-ESP (ODDSound)** — broadcast tuning table for clients that support it.
No path is universally honored, so this is best-effort per plugin. Sizable
feature; deferred. The note-name/notefreq scripting work uses `noteToFreq`
directly and is unaffected by this gap.

---

## TECH DEBT: graph builder can leave audio connections on a node that doesn't reach Output

**Found:** 2026-06-10, while diagnosing the now-fixed "unconnected wavetable
plays on transport Play" bug (issues 1 & 2 in that session).

**What it is:** `GraphProcessor::rebuildGraph` builds JUCE
`AudioProcessorGraph` connections directly from `graph.links`, then separately
computes `Node::reachesOutput` via a backward audio-only BFS from Output nodes
(`graph_processor.cpp` ~1001-1046). These two are derived from the same data
but are not cross-checked. A diagnostic (`[CONN-DBG]`, since removed) captured a
wavetable node with `reachesOutput == false` that *still* had live audio
connections into an intermediate pan/gain node — i.e. the node fed an audio
branch that dead-ends before Output. That's internally consistent (a dead-end
branch legitimately has connections but no path to the sink), but it means the
node's `processBlock` still runs and its audio is still routed into the graph;
the only thing keeping it silent is that the branch never reaches the sink.

**Why it mattered (the original bug, now fixed):** when the count-delta rebuild
heuristic missed a connection change, the `AudioProcessorGraph` could hold
connections that no longer matched `graph.links` while `reachesOutput` was
stale, producing audio from a node the user believed was disconnected. The root
cause — rebuilds not firing on cable connect/disconnect and mod-pin add/remove —
was fixed by forcing an explicit `requestRebuild` on those paths
(`node_graph_component.cpp`). So this is no longer a live bug.

**Proper defensive fix (not yet done):** make the connection builder and
`reachesOutput` share a single source of truth, OR have the builder skip audio
output connections for any node whose `reachesOutput` is false (a node that
can't reach the sink has no reason to emit audio into the graph). Either removes
the class of bug where a routing inconsistency makes a "disconnected" node
audible, rather than relying on rebuild timing to keep the two views in sync.
Low priority — the timing fix closed the observed symptom; this is hardening.

**Where to look:** `graph_processor.cpp` `rebuildGraph` connection loop
(~860-999) and the `reachesOutput` BFS (~1001-1046); the rebuild trigger in
`processBlock` (~1080-1086).

---

## BUG (needs repro): mod-import loop not reverted after deleting the module

**Reported:** 2026-06-09. User loaded a tracker module, deleted it, and the
song was left "on loop" even though they never enabled looping. Expectation:
deleting the module reverts loop/repeat settings to the pre-import state, and
the user feels a module should express looping via Song Repeat (the "different
kind of loop"), never via the user's manual transport loop region.

**Code analysis (could not reproduce from inspection):**
- `mod_import.cpp` ~1851-1888: a genuine module loop is detected as either
  whole-song (`wholeSongLoopTarget == 0` → Song Repeat = Forever, does NOT touch
  the transport loop) or partial-section (`wholeSongLoopTarget > 0` → sets
  `graph.loopEnabled = true` + a loop region). Both paths call
  `stashPreImportSong()`, which records the pre-import song/loop settings on the
  import's root **group** node (`modImportSavedSong` + `modImportPrev*`).
- `node_graph_component.cpp` `deleteNodeAndDescendants` ~2992-2999: when the root
  group is deleted, it restores all stashed `modImportPrev*` values. The two
  general delete entry points (Delete key → `deleteSelectedNode`, context-menu)
  both funnel here.
- `project_file.cpp` round-trips all seven `modImport*` fields symmetrically.
- `main_window.cpp:905` syncs `transport.loopEnabled = graph.loopEnabled` every
  timer tick, so a restored `graph.loopEnabled = false` propagates to audio.

The restore path is correct in isolation. Likely culprits to confirm with the
user: (a) the module was removed by deleting individual child nodes rather than
the **group root** (only the group root carries `modImportSavedSong`, so
deleting children leaves song settings untouched — by design); or (b) the
module hit the partial-section branch and the real objection is design intent
(#2): module imports should never enable the transport loop at all.

**Possible proper fix (pending decision):** stop using the transport loop region
for partial-section module loops — express looping only via Song Repeat. Since
Song Repeat Forever can't represent "play intro once, then loop from beat X,"
this needs either a loop-start beat added to the Song Repeat system or an
accepted approximation (partial loops degrade to play-once). Do NOT rip out the
current behavior blindly; confirm repro + desired semantics with the user first.

## FUTURE WORK: shared master-VCA so own-envelope synths can honor node AHDSR

**Context:** 2026-06-09, AHDSR consolidation. The shared envelope runtime
(`AHDSREnvelopeRuntime` + `AHDSRCurveTables`, `adsr_envelope.{h,cpp}`) is now
the single amplitude-envelope source for Terrain Synth (incl. wavetable /
frequency-domain / wavelet-space variants), Additive, Phase Distortion, and
Spectral Grain. Those nodes read `node.ahdsrEnvelope` directly and have **no**
A/D/S/R params on the node.

The remaining synths — FM (four per-operator AHDSR envelopes — see the
2026-06-23 update below), Particle Cloud (per-grain mini-envelopes), Drum
(per-sound envelopes), and the sample/region players
(SoundFont, SFZ, Sfizz, MultiSampler) — generate their own amplitude shaping
internally, so the node-level *Envelope (AHDSR)…* editor is deliberately **not**
offered for them (gating at `node_graph_component.cpp` ~line 2513-2534). Editing
it would be inert, which the "no silent lies" rule forbids.

**Proper fix:** add an optional shared *master-VCA* stage driven by
`node.ahdsrEnvelope` that multiplies the final per-voice output of any synth,
applied **after** the synth's own internal envelope. Then the editor can be
offered universally, with a note that it stacks on top of the engine's native
shaping. Until then, those synths intentionally show no AHDSR editor.

**Update 2026-06-22 (framesynth instruments):** the six new standalone
single-frame instruments (`__framesynth__:` — Layered / Frequency Domain /
Wavelet Space / Inharmonic / Sample / Granular) route through the wavetable
render path, so they already honor `node.ahdsrEnvelope` and correctly get the
*Envelope (AHDSR)…* editor (right-click item 180 + the focused-editor toolbar
"Envelope…" button). That fully satisfies "envelope on all six new instruments."

The user also asked for "the envelope feature for every other existing
instrument that doesn't already have it." Audit result (2026-06-22):
- **MultiSampler** already exposes a full A/D/S/R editor in
  `MultiSamplerEditorComponent` (its own per-voice envelope).
- **FM** (four per-operator AHDSR envelopes as of 2026-06-23 — see update
  below; previously per-operator linear A/D/S/R params), **Particle** (grain
  attack/release), and **Drum** (per-sound decay) expose their native envelope
  shaping on the node.
- **SF2 / SFZ / Sfizz** have envelopes baked into the loaded soundfont/.sfz
  file — there is no SEANCE-synthesized envelope to edit; a node AHDSR could
  only act as a master-VCA on top.

So the *only* way to give these synths a SEANCE-level AHDSR is the shared
master-VCA above. That is a cross-cutting DSP change (per-voice access doesn't
even exist for the library synths SF2/Sfizz, whose voices live inside
tinysoundfont/libsfizz — a node AHDSR there can only be a node-global VCA on the
summed output, which retriggers wrongly under overlapping notes). It also has a
real musical-behavior decision (does a master VCA at non-default settings gate a
held kick drum? is that desired?). Because it's not a clear mechanical fix and
the default-settings transparency vs. per-voice-vs-node-global tradeoff needs a
product call, it stays tracked here rather than being built blind. **Build the
master-VCA properly (per-voice where voices exist, documented node-global
fallback for library synths) when picked up — do not add a partial stop-gap.**

**Update 2026-06-23 (FM operators upgraded to the shared AHDSR):** the FM
synth's four operators no longer use the old per-operator linear `Op{i} A/D/S/R`
params. Each operator now carries a full `AHDSREnvelope` (Attack / Hold / Decay
/ Sustain / Release + per-segment curves + tension + velocity sensitivity), held
in the new `Node::opEnvelopes` vector (4 entries for FM). The new data model:
`std::vector<AHDSREnvelope> opEnvelopes` on `Node` (general-purpose multi-
envelope container; empty for every non-FM node), serialized as
`opEnvelope0..N` in project files, migrated from the legacy params on load via
`ensureFmOpEnvelopes()` (`node_graph.cpp`). `FMSynthProcessor` runs 4×
`AHDSRCurveTables` (one per operator, baked once per block) + 4×
`AHDSREnvelopeRuntime` per voice. Edited via the tabbed *Operator Envelopes
(AHDSR)…* dialog (`launchOpEnvelopesDialog`, right-click item 182) which hosts
one `AHDSREnvelopeComponent` per operator tab — a reusable multi-envelope editor
available to any future multi-envelope instrument. This does **not** change the
master-VCA decision above (FM still has its own per-operator envelopes and is
not a candidate for a node-global master VCA); it just replaces FM's crude
linear ADSR with the good shared one. Velocity is applied once at the FM master
output, so the operator envelopes default to `velocitySensitivity = 0` (raising
it per operator opts into FM-style velocity→brightness).

## FIXED 2026-06-09: Terrain Pan param off-by-one

**Was:** the full Terrain node (`makeTerrainNode`) created params in the order
Attack/Decay/Sustain/Release/Volume/Pan/Speed/…, but the processor's
index-based `getParam(idx)` reads treated index 5 as **Speed**, not **Pan** —
a pre-existing off-by-one where Pan was created but never read and Speed was
read from the Pan slot. Converting all Terrain reads to name-based
`getParamByName` (terrain_synth.cpp) and removing the A/D/S/R params during the
AHDSR consolidation fixed this: every traversal/LFO/grain param is now matched
by name, so creation order and read order can no longer drift.

---

## TECH DEBT: unify the single-curve and layer-stack editors (two-tier plan)

**Observed:** 2026-06-09, while adding Lua/Python authoring to the wavetable
**Formula** layer (`WaveLayer` in `layered_wave_editor`) and the **spectral
curve** (`SpectralCurve`/`SpectralCurvePanel` in `curve_editor`). Several
features reimplement the same two ideas with divergent code/representations:

- **A single authored 1-D curve** — Equation (Built-in / Lua / Python) or Drawn
  (points / freehand), baked to samples. Used by: spectral magnitude/phase
  curves, ADSR segment curves, and the *shape part* of each wavetable/LFO/
  MIDI-gen layer. `SpectralCurve` already is this object; `WaveLayer` reinvents
  it with a `Shape` enum (named primitives baked in) + freehand-only Drawn.
- **A stack of those curves with positioning metadata** — `Layer = curve +
  {harmonic, phase, amplitude}`, summed + normalized. Shared (conceptually)
  between the layered waveform editor and the LFO node (and MIDI generation).
  ADSR and spectral curves do NOT use this tier — they're a single curve.

**Why it matters:** Lua/Python (via the already-shared `bakeShapeExpr`) would
come "for free" to ADSR curves once they sit on the shared single-curve widget.
Right now the per-feature forks mean each new authoring capability has to be
wired N times.

**Proper fix (staged):**
- *Stage A (self-contained, do first):* generalize `SpectralCurve`/
  `SpectralCurvePanel` into a context-configured `ShapeCurve`/`ShapeCurvePanel`
  driven by a `CurveContext { domain (periodic radians vs normalized [0,1]),
  valueRange + clampMode, primitivePalette, defaultExpr }`. The
  `domainRadians` flag on `bakeShapeExpr` already is the periodicity axis.
  Point the ADSR editor at it — gets Lua/Python in ADSR at no extra cost. The
  primitive-button difference the user noted is just `primitivePalette` being
  empty/easing-only for ADSR (Linear/Exp/Log/S-curve) vs waveforms for
  periodic uses.
- *Stage B (invasive, touches audio path):* re-express `WaveLayer` on top of
  `ShapeCurve`, then factor sum/normalize into a `LayerStack` shared by the
  layered waveform, LFO node, and MIDI-gen. Requires reconciling the two
  divergent curve representations (pick `SpectralCurve`'s cleaner Equation/
  Drawn-points/Drawn-freehand model; express named primitives as palette
  presets) — this is the real refactor cost.

**Axes that must become CurveContext config (not code forks):** domain/
periodicity (periodic radians `[0,2π)` wrap vs non-periodic `[0,1]` once),
output range/clamp (`[-1,1]` bipolar audio vs `[0,1]` unipolar ADSR/spectral),
primitive palette, and (Tier-2-only) stacking metadata + normalization.

**Status:** current Lua/Python work (wavetable Formula + spectral curves) is
shipped as the Tier-1 proof. The AHDSR editor (`adsr_envelope_component.cpp`)
already embeds `SpectralCurvePanel` directly, so Built-in/Lua/Python authoring
is *already live* on every AHDSR segment curve (Attack/Hold/Decay/Release) —
the "ADSR gets Lua/Python for free" goal is effectively met without the rename.
A **Hold** segment curve (a `0..1` multiplier on peak, default flat `"1"`) was
added 2026-06-09 alongside this. Stage A (the `CurveContext` struct +
`SpectralCurve`→`ShapeCurve` rename) remains deliberately deferred: it is a
mostly-cosmetic generalization whose value is preventing future per-site forks,
not fixing a current bug, and the rename touches ~16 files with byte-identical
serialization constraints. Do it as one deliberate unit, not slipped in
piecemeal. Stage B is a tracked refactor, not to be slipped in as a partial
(no-stop-gap rule).

## Lua / Wasm script load errors aren't shown in the editor

**Observed:** 2026-06-09, while adding the Language dropdown to MIDI Script /
Signal Shape. If a Lua program has a syntax error or omits the required
`loop()` function (e.g. right after switching the language from Built-in,
when the text is still a built-in expression), or a chosen `.wasm` file
fails to load/link, the runtime captures the message via
`IScriptRuntime::getError()` but the **editor never surfaces it** — the node
just goes silent. The user gets no feedback about *why*.

**Where:** `MidiScriptEditorComponent` (`midi_script_editor.cpp`) and
`SignalShapeEditorComponent` (`signal_shape_node.cpp`).

**Status (2026-06-24): MIDI Script half RESOLVED (Stage 1 of the script-error
feature).** `MidiScriptEditorComponent` now shows a red **error strip** at the
bottom, fed by a message-thread *linter* (`validateScript()`): every edit,
debounced ~300 ms, compiles the program with a throwaway runtime via
`makeScriptRuntime(...)->load()` and shows/hides the strip. This is cleaner
than the originally-proposed "poll the live processor" approach — the lint is
immediate, side-effect-free (throwaway state), and reuses the exact load path.
In the node graph, a node whose **live** script failed to compile is drawn with
a red border + "!" badge: `MidiScriptProcessor` now latches an
`std::atomic<bool> scriptHasError` after each audio-thread `load()`, exposed via
`hasScriptError()` and wired to `NodeGraphComponent::getNodeScriptError`
(`getProcessorForNode` + `dynamic_cast<MidiScriptProcessor*>`, mirroring the
`onSignalShapeManualTrigger` pattern). Failed loads are also logged to
`seance.log`.

**Signal Shape half RESOLVED too (2026-06-24, same Stage 1 work).**
`SignalShapeEditorComponent` got the identical `validateScript()` linter +
error strip, and `SignalShapeProcessor` got the `scriptHasError` atomic +
`hasScriptError()`; `NodeGraphComponent::getNodeScriptError` now tries both
`MidiScriptProcessor` and `SignalShapeProcessor`. Both real-time script
editors are fully covered for Lua/Wasm compile errors. This whole section is
now closed except for the Built-in language gap (Stage 2 below).

**Stage 2 RESOLVED (2026-06-24): Built-in language structural checking.**
`BuiltinExprRuntime::load()` now runs `WaveExprParser::validate()` — a cheap,
side-effect-free structural pass — and returns `false` + a message on a
malformed program, so the existing Stage 1 strip/badge automatically light up
for the Built-in language too. The evaluator stays intentionally tolerant
(unknown identifier → 0, missing close paren ignored mid-render); rather than
retrofit error-reporting into the ~50 tolerant parse sites (per-sample audio
overhead, big risk), `validate()` does a separate structural scan reporting,
with a 1-based line number: unbalanced parentheses, an unterminated string
literal, and out-of-grammar characters (`%`, `@`, `#`, `\`, `~`, `[`, …). It
deliberately does **not** flag unknown identifiers (those legitimately read as
0). It is quote-aware (parens/illegal chars inside `"…"`/`'…'` are ignored).
**Also fixed a latent hang:** `ExprParser::runProgram` could spin forever on a
non-advancing token (e.g. a stray `#` that no parse rule consumes, leaving
`pos` un-advanced on the audio thread); it now steps over any such character so
the program always terminates even if `validate()` is bypassed.

**Stage 3 RESOLVED (2026-06-24): Python bake error reporting with line
remapping.** A Python shape/terrain bake wraps the user's source in generated
scaffolding (imports, helper defs, a `def __shape(x):` / `def __cell(...)`
header, a driver loop), so a raw Python line number points at machine-generated
text the user never sees. The old `fetchPythonError()` returned just
`Type: message` with no line at all. Replaced it with
`formatPythonError(userLineOffset, userLineCount)` (`scripting.cpp`), which:
- Handles **SyntaxError** (read via the exception's `lineno` attribute) AND
  **runtime tracebacks** (walked via `tb_lineno`/`tb_next` attribute access —
  ABI-stable across CPython versions), mapping the generated line back to the
  user's own 1-based line: `g` is the user's iff
  `userLineOffset < g <= userLineOffset + userLineCount`, giving
  `g - userLineOffset`. A no separate `Py_CompileString` pre-check was needed —
  `PyRun_String` already raises SyntaxError through the same failed-run path, so
  folding syntax handling into the formatter is the non-duplicative fix.
- Writes the **full** `traceback.format_exception` stack to `seance.log` (the
  inline label shows the concise remapped headline `Type: message (line N)`).
Each bake site computes its offset/count by counting newlines in the generated
program just before the user's source is appended (`countNewlines`), so the
mapping is robust to future preamble changes. 8 self-test assertions cover it
(bare-expr → line 1, multi-line → line 2, SyntaxError, clean program). Self-test
678/678.

**Stage 4 RESOLVED (2026-06-24): GLSL bake error reporting with line
remapping.** GLSL shape/terrain bakes already compiled on demand through the
headless GL 4.3 compute context and surfaced `glGetShaderInfoLog` /
`glGetProgramInfoLog` text via `GlslDispatchResult::error` (gated to
GL-available — when no 4.3 context can be created the bake fails with a
human-readable reason and the language is greyed out in the dropdown). The
remaining gap was the same one Stage 3 closed for Python: the driver info-log
line numbers point at the *generated* compute shader (the `shapeValue` /
`cellValue` / `main` wrapper + `waveform()` helper), not the user's body. Added
`remapGlslErrorLog(log, userLineOffset, userLineCount)` in `glsl_compute.cpp`
(pure string processing, platform-independent) which rewrites the line numbers
in both the NVIDIA `0(L)` and AMD/Intel/Mesa `0:L:` log formats back to the
user's 1-based source line, leaving wrapper/out-of-range numbers untouched.
`bakeGlsl` (`shape_expr.cpp`) and `Terrain::fillFromGlsl` (`terrain_synth.cpp`,
both per-cell and whole-grid branches) now build the scaffolding prefix as a
separate string, count its newlines for `userLineOffset`, remap `res.error`
before returning it, and log the raw un-remapped driver log to `seance.log` via
`juce::Logger`. Self-test adds 4 deterministic `remapGlslErrorLog` unit checks
(run even with no GL) plus an end-to-end "broken GLSL body → compile error, not
silent" bake check. Self-test 683/683.

**The staged script-error feature is now complete (Stages 1–4 all resolved):**
Lua/Wasm (Stage 1) and Built-in (Stage 2) errors light up the editor strip +
node badge; Python (Stage 3) and GLSL (Stage 4) bake errors show a remapped
`Script error: …` overlay with the user's own source line, full detail logged to
`seance.log`.

## Control Bank editor may not push undo steps / mark dirty (audit) — RESOLVED

**Observed:** 2026-06-09, noted while fixing the same gap in the Signal Shape
editor (now resolved via a destructor `commitSnapshot`). Other modeless
script-style editors that followed the old SignalShape pattern — **Control
Bank** in particular — may still lack a close-time `graph.commitSnapshot()`,
so edits there leave no undo step and don't set `graph.dirty` (silent loss on
quit). Audit each; the MIDI Script and Signal Shape editor destructors are the
reference for the correct one-snapshot-per-session pattern.

**Status (2026-06-30): RESOLVED — audited, Control Bank is fully covered.**
`control_bank.cpp` now calls `graph.commitSnapshot(...)` on every mutating
operation rather than relying on a single close-time snapshot, which is
stricter than the destructor pattern: orientation toggle
(`"Control bank orientation"`), add slider (`"Add control slider"`), remove
slider (`"Remove control slider"`), rename slider (`"Rename control slider"`),
and value changes (`"Set control value"`). The value path is drag-aware —
slider-drag gestures snapshot once on `onDragEnd`, while non-drag edits
(wheel / typed value) snapshot immediately in `onValueChange` (guarded by
`!isMouseButtonDown()`), so there's no per-tick snapshot spam. Since
`commitSnapshot` both pushes an undo step and marks the graph dirty, edits are
recoverable and prompt-on-quit works. No silent-loss gap remains.

---

## MOD import: suspiciously short (2-beat) transport loop on a long module

**Observed:** 2026-06-08. A user imported a module much longer than two beats
but ended up with a transport A-B loop only ~2 beats long, sitting near the end
of the song. The piano-roll loop overlay made it visible; playback wrapped
inside that tiny region.

**Where:** `mod_import.cpp` loop-target detection. When the order-list walk
revisits an order (`orderVisited[order]` true at ~L1493), it records
`wholeSongLoopTarget = order`, and for `wholeSongLoopTarget > 0` sets the
transport loop to `[orderStartBeat[wholeSongLoopTarget], currentBeat]`
(~L1872-1887). A 2-beat loop means the detected loop-back target started only
~2 beats before the song end — i.e. the walk decided the module loops back to
its final, very short order/pattern.

**Hypotheses (need the actual module to confirm):**
1. *Genuine:* the module really does end by looping a tiny trailing
   pattern/outro (some modules do). Then the import is faithful and this is not
   a bug — just surprising.
2. *Bug:* `wholeSongLoopTarget` resolves to a trailing order it shouldn't
   (e.g. a `Bxx`/pattern-break path that lands on the last order, or an
   `orderStartBeat` recorded at the wrong point), producing a degenerate loop
   instead of the module's real loop point — or instead of "no loop / play
   once."

**Repro need:** the specific imported module file (.mod/.xm/.it/.s3m). Without
it we can't tell hypothesis 1 from 2. **Next step when a repro arrives:** log
`wholeSongLoopTarget`, `orderStartBeat[...]`, and `currentBeat` at the loop-set
site and compare against the module's order list / `Bxx` targets.

**Mitigation already in place:** the loop is now clearly drawn (framed overlay,
not a wash), editable (drag the loop edges), and removable (right-click →
*Disable Loop Region*, or Clip → *Clear Loop Region*). Deleting the import's
root group node also restores the exact pre-import loop state (stashed in
`mod_import.cpp` `stashPreImportSong()` → restored in
`node_graph_component.cpp` `deleteNodeAndDescendants()`).

---

## Python interpreter run on the audio thread → `python314.dll` crash — FIXED

**Observed:** 2026-06-12. Two crash dumps (`SEANCE.exe.1776.dmp`,
`SEANCE.exe.56136.dmp`) with the faulting RIP inside `python314.dll` and an
access violation **reading `0x10`** (a near-null `PyObject*` dereference =
CPython interpreter-state corruption). These were the most recent of nine
recovery-prompt-triggering crashes the user hit "lately." The older dumps in
that batch are the separate `node.script` / `graph.nodes` data races documented
below (`-1` sentinel reads, heap ops); this entry is the *new* signature.

**Root cause:** the embedded CPython interpreter is a single process-global,
GIL-held resource that may **only** be touched from the message thread (see
`scripting.h`). But a Terrain Synth (or Signal Shape) whose source is a layered
waveform containing a **Lua/Python/GLSL Formula layer** re-ran the interpreter
to bake that layer's one-cycle buffer **on the audio thread**:

- `GraphProcessor::rebuildGraph` runs **inside** the audio callback
  (`processBlock`, graph_processor.cpp:1380), so the `TerrainSynthProcessor`
  **constructor** (terrain_synth.cpp:1358) and `reloadIfScriptChanged`
  (terrain_synth.cpp:1211) both run on the audio thread.
- Both call `LayeredWaveform::decode → parseLayer → WaveLayer::rebakeFormula →
  bakeShapeExpr → ScriptEngine::bakeShapeExpr`, which executes CPython.
- Running Python off the message thread (no GIL/thread-state handoff) corrupts
  interpreter state; the next interpreter access faults near-null inside
  `python314.dll`. Timing matched the user's "recovery dialog often lately"
  with no consistent repro — it only bit when a graph rebuild coincided with a
  Terrain/Signal node that had a non-Built-in Formula layer.

**Fix shipped (2026-06-14):** the audio thread no longer runs an interpreter to
reconstruct a formula cycle. Three coordinated changes (all in
`layered_wave_editor.{h,cpp}`, the shared layered-waveform codec used by Terrain
Synth, Signal Shape, and wavetable frames):

1. **Embed the baked cycle in the script.** `encodeLayer` now writes a
   `bake=<count>;<s0>;<s1>;…` field for non-Built-in Formula layers (`;`-
   separated so the comma-split field parser keeps it as one field). `parseLayer`
   loads it straight into `formulaSamples`, so the audio thread renders the
   formula from data — never the interpreter. This mirrors the `__generate__`
   terrain path, which already embeds baked grid data for the same reason.
2. **Thread-guard the baker.** `WaveLayer::rebakeFormula()` checks
   `juce::MessageManager` and, when called off the message thread, refuses to
   bake (leaves `formulaSamples` untouched) instead of running Python. This is
   the crash-safety net: any unmigrated/edge-case script that reaches the audio
   thread renders silent rather than corrupting the interpreter.
3. **Migrate old projects on load.** Projects saved before the embed have no
   `bake=` field. `decode()` sets `decodedNeedsBakeEmbed`, and
   `migrateLayeredScriptEmbedBake` (plus a Signal-Shape sibling loop) re-bakes on
   the message thread and re-encodes in `openProjectFile` (under `mutationLock`,
   audio thread parked) so the embed is present before the graph goes live. The
   undo baseline snapshot is taken after this, so undo/redo also carry the embed.

Covered by six self-tests (`bake:` prefix in `--self-test`). **Residual gap:**
the migration only walks `__layered__` (Terrain Synth) and `__signalshape__`
nodes; a Python/Lua Formula layer embedded in an old **wavetable frame**
(`__wavetable*__`) is crash-safe via the thread-guard but renders silent until
re-saved. Low risk (rare combination, no crash), documented here as a follow-up.

---

## `node.script` live-edit data race — fixed for Terrain Synth, still raw elsewhere

**Observed:** 2026-06-08. A crash (`SEANCE.exe.3596.dmp`) ~1 second after
editing a waveform and closing the wavetable editor. The fault is an access
violation inside a `std::string` copy: `memcpy` of ~1.4 MB (`R8=0x150176`)
from a garbage source pointer (`Rdx=0xfffffffffcbfa000`). The 1.4 MB matches a
granular wavetable node's `script` (PCM-as-text, format `__wavetable4__:…`).

**Root cause:** `TerrainSynthProcessor::reloadIfScriptChanged()` runs on the
audio thread every block and reads/copies `node.script`, while UI-thread
editors rewrite `node.script` in place (the #23 "commit to node.script without
a full rebuild" path). A `std::string` assignment is not atomic, so the audio
thread can observe the new size with a stale/freed data pointer and `memcpy`
from garbage — guaranteed to crash for a multi-MB granular script. This is
*not* the same as the `mutationLock` issue below: that guards `graph.nodes`
reallocation; this is concurrent mutation of a single stable node's string.

**Fix shipped:** added `setNodeScriptSynced(Node&, std::string)` in
`node_graph.h`, which locks the node's existing `auditionMutex` around the
assignment. `reloadIfScriptChanged()` now takes a locked snapshot under the
same mutex and works off the audio-thread-owned `cachedScript` (also used at
the `classifySynthSource` call later in `processBlock`, instead of re-reading
`node.script`). All live writers to Terrain-Synth-classified nodes route
through the helper: `layered_wave_editor.cpp` (wavetable/layered/granular),
`spectral_editor.cpp` (standalone spectral), `wavelet_painter.cpp` (standalone
wavelet paint), and `scripting.cpp` (Python `set_script`). Release builds also
now emit PDBs (`/Zi` + `/DEBUG /OPT:REF /OPT:ICF` in `cpp/CMakeLists.txt`) so a

**Second audio-thread reader was missed — fixed 2026-06-11
(`SEANCE.exe.17316.dmp`):** the same crash recurred while dragging the freeze
*selection band* in the granular editor (grain size and selection both at
minimum). The fault was an access violation in `AudioCacheManager::hashString`
copying `node.script`, reached via `computeNodeHash` → `isCacheValid` →
`GraphProcessor::rebuildGraph`, which runs **on the audio thread** inside the
audio callback. So besides `reloadIfScriptChanged`, the cache hasher is a
*second* audio-thread reader of `node.script`, and it had never been paired
with `auditionMutex` — the band drag fires `onLayerChanged → commitToNode →
setNodeScriptSynced` on every drag tick, and a rebuild landing inside that
write window read a torn size/pointer of the multi-MB granular script. Fixed by
wrapping the `hashString(node.script)` read in `computeNodeHash`
(`audio_cache.cpp` ~L117) in a `std::lock_guard<std::mutex>` on
`node.auditionMutex`, pairing with the writer. The lock is released before the
recursive upstream-hash calls, so no two per-node mutexes are ever co-held (no
deadlock), and the writer never takes `mutationLock`, so there's no cycle with
the audio callback's `mutationLock → auditionMutex` order.
future crash dump is symbolizable.

**What's still unprotected:** the identical poll-`node.script`-on-the-audio-
thread pattern exists in other processors — `midi_mod_node.cpp` (~L88),
`trigger_node.cpp` (~L294), `signal_shape_node.cpp` (~L128),
`multi_sampler.cpp` (~L342), `soundfont_processor.cpp`, and the
`spectrum_tap.cpp` write path (~L440). Their editors still do raw
`nd->script = …` (`midi_mod_node.cpp:412`, `trigger_node.cpp:812`,
`signal_shape_node.cpp:721`, `multi_sampler_editor.cpp:264`,
`convolution_editor.cpp:135`, `spectrum_tap.cpp:441`). These scripts are small
(bytes to KB), so the realloc/copy window is orders of magnitude smaller than
the granular case and the race is effectively never observed — but it is the
same bug class. **Proper finishing fix:** route every one of those writers
through `setNodeScriptSynced`, and wrap each processor's `node.script`
compare/copy in a `std::lock_guard` on that node's `auditionMutex` (snapshot
into the processor's `cachedScript` exactly as Terrain Synth now does).

---

## Audio-thread graph access is only partially synchronised

**Observed:** 2026-06-07. A tracker-import crash
(`SEANCE.exe.63000.dmp`) was traced to the audio callback iterating
`graph.nodes` (both for per-event MIDI routing and inside
`GraphProcessor::rebuildGraph` when it observes a node-count change)
concurrently with `mod_import.cpp` calling `graph.addNode()`. A
`std::vector` reallocation mid-iteration produced torn reads of
`Node::id` — the post-rebuild `nodeMap` contained entries with `id=0`
and `id=1132382734` (an obvious freed-memory read), and the JUCE graph
wiring then crashed downstream.

**Partial fix shipped:** added `NodeGraph::mutationLock` (a
`std::mutex`). The audio callback takes a non-blocking try-lock at the
top of `AudioEngine::audioDeviceIOCallbackWithContext` and outputs
silence if it can't acquire it. The batch-mutation entry points hold a
`std::lock_guard` for the duration of the batch: MOD import
(`importModFile` callback in `main_window.cpp`), project file load
(`openProjectFile` + the constructor-time auto-load), autosave recovery,
and undo snapshot restore (`onLoadSnapshot`). `Node::id` and `Pin::id`
also got `= -1` default initialisers so an uninitialised read is
recognisable rather than wild memory.

**Node-deletion paths now guarded (2026-06-09):** the predicted "nonzero
chance" crash actually fired — `SEANCE.exe.80308.dmp`, a crash on deleting
the wavetable node. The UI-thread `graph.nodes` / `graph.links` erase in
`NodeGraphComponent::deleteNodeAndDescendants` ran concurrently with the
audio callback iterating those vectors, so the multi-element `erase`
(which moves/frees elements) produced a torn read / use-after-free —
exactly the `.63000.dmp` mechanism at a delete site instead of an add
site. Fixed by holding `graph.mutationLock` across the structural edit in:
`deleteNodeAndDescendants` (lock to end of function, covers links+nodes
erase and `commitSnapshot`'s serialization read),
the "Merge convolutions" context-menu erase (`node_graph_component.cpp`),
`MainContentComponent::newProject` (clear+rebuild), and the Python
`remove_node` scripting API (`scripting.cpp`). The same edits also removed
two dangling `Node* defaultTrack` pointers held across `addNode()`
(constructor + `newProject` MIDI-input auto-wire) by resolving a stable
MIDI input pin ID up front instead of caching a `Node*`.

**What's still unprotected:** single-node *additive* menu actions (Add
Node, drag-link to create a Link, inline rename) still mutate
`graph.nodes` / `graph.links` from the UI thread without taking the lock.
Each of these is one `push_back` (or a single small field write), so the
race window is tiny relative to the audio block period and the chance of
hitting it is correspondingly low — but it's nonzero and the failure mode
is the same crash class. The proper finishing fix is to hold the lock
around every UI-side graph mutation. Best place to do it is at the
boundary in `NodeGraphComponent` (the context-menu / drag handlers in
`node_graph_component.cpp` that call `graph.addNode`, `graph.addLink`,
etc.). A single audit pass + RAII guard at each remaining call site
should be enough.

---

## Sample-frame captures still have no in-editor view

**Observed:** 2026-06-07. After capturing a single-cycle SampleFrame via the
capture flow, the right-pane editor falls through to the "no editor for this
frame type yet" placeholder (added when the granular editor was built).
The user can see what they captured and replace it via + Waveform, but
there's no way to view the captured cycle, re-do the FFT round-trip
conditioning, or display the embedded pitch readout.

**Where to look:** `updateFrameEditorEmbed()` in `layered_wave_editor.cpp`.
The proper fix mirrors `GranularFrameEditorComponent` (added in the same
session): a `SampleFrameEditorComponent` that shows the single cycle as a
waveform display, a pitch readout, and a "Re-capture..." button that
replaces in place via `showCapturePanelInline(0, /*replaceCurrent=*/true)`.
Add a branch in `updateFrameEditorEmbed()` for `tid == "sample"` that
instantiates it. The plumbing for in-place replacement is already there
(`replaceCurrentEntryWithCapturedFrame`); only the editor component is
missing.

---

## MOD re-import is non-deterministic: sometimes replaces, sometimes duplicates

**Observed:** 2026-06-07. Re-importing the same MOD file via File → Import
MOD on an existing project sometimes ADDS a second set of MidiTimeline +
MultiSampler nodes (one per channel) and sometimes REPLACES the existing
set. Surfaced while diagnosing the (now-fixed) silent-playback-on-restart
bug — re-import was the workaround, and the non-determinism made the
workaround unreliable. Once a user noticed two sets in the graph; other
times only one.

**Why this matters:** when re-import duplicates, the new nodes may be
unwired (not connected to Output) and the old broken ones still hold the
audio path, so the user sees "I re-imported" but hears no change. When
re-import replaces, the new nodes inherit the wiring and the song plays.

**Where to look:** `ModImporter::import` in `mod_import.cpp`. The decision
of whether to clear / overwrite existing imported nodes should be either
"always replace by source-file fingerprint" or "always append + leave it
to the user to delete the old ones". Current behaviour is neither.


---

## Incremental build silently leaves ABI-skewed object files (crashed editor Play)

**Observed:** 2026-06-09. Pressing Play on a previously-created granular
waveform in the layered wave editor crashed SEANCE with an access violation
in `TerrainSynthProcessor::processBlock` at `terrain_synth.cpp:1477`
(`!ev.granularFrame->source->empty()` — reading a garbage `shared_ptr`).

**Root cause:** a structural ABI skew, NOT a source bug. The
`AuditionEvent` struct in `node_graph.h` had a 5th field
(`std::shared_ptr<AuditionGranularFrame> granularFrame`) added at the end,
growing `sizeof(AuditionEvent)` from 40 → 56 bytes. The incremental build
recompiled only a handful of TUs (terrain_synth, layered_wave_editor,
capture_from_playback, graph_processor, audio_engine) against the new
header, but left **65 of 86** SEANCE object files — including
`node_graph.obj` and `piano_roll_component.obj`, both of which construct /
push to `node.pendingAudition` (a `std::vector<AuditionEvent>`) — compiled
against the OLD 40-byte layout. With producers/managers using a 40-byte
element stride and the consumer reading a 56-byte stride, the consumer's
read of the trailing `granularFrame` field landed in the next element's
bytes (interpreted-as-float-data garbage), so the first 0x28 bytes
(isNoteOn / pitch / velocity / position) were valid but `granularFrame`
was corrupt. Classic "first fields fine, last field garbage" skew
signature.

**Fix applied:** deleted all 68 of our own app `.obj` files under
`build/SEANCE.dir/Release` (kept the JUCE / third-party objs, which don't
include `node_graph.h`) and did a full recompile so the whole binary
agrees on every `node_graph.h` layout. The source is correct — the
3-field aggregate inits like `{true, pitch, 100}` in the piano-roll
producers correctly zero-fill `position` and `granularFrame`.

**Recurred 2026-06-13.** Adding `ContentStore contentStore;` as a member of
`NodeGraph` in `node_graph.h` grew `sizeof(NodeGraph)`, but the incremental
build left `main_window.obj` compiled against the old layout. This time the
skew surfaced as a **crash at destruction time**: a stack-local `NodeGraph`
constructed in the self-test printed its "constructed" marker, then
segfaulted when it went out of scope — the stale TU's view of the object's
member offsets put the destructor's frees on the wrong addresses, corrupting
the heap. Deleting `main_window.obj` (then a full `--clean-first` rebuild)
fixed it; all self-tests pass. **Signature to recognise:** a destructor-
time / free-time crash that appears right after adding or reordering a member
in a widely-included header is almost always this, not a logic bug in the new
code. Reach for a clean rebuild *first* before auditing the new code.

**Root cause corrected 2026-06-13 (was previously misattributed to a broken
dependency tracker).** An earlier version of this entry claimed "MSBuild's
incremental dependency tracking did not reliably recompile header dependents."
That is **wrong** — it was disproven empirically:

- **The tracker records the dependency.** `build/SEANCE.dir/Release/SEANCE.tlog/CL.read.1.tlog`
  (UTF-16; decode with Python, not `iconv`) lists `NODE_GRAPH.H` under
  `MAIN_WINDOW.CPP`'s 946-entry read group (and `CONTENT_STORE.H`,
  `TERRAIN_SYNTH.H`). MSBuild's FileTracker knows `main_window.cpp` depends on
  `node_graph.h`.
- **An ordinary incremental build honors it.** `touch src/node_graph.h` →
  `cmake --build build --config Release --target SEANCE` recompiled
  `main_window.cpp` (build log shows it; `main_window.obj` mtime advanced).
  So a clean, uninterrupted incremental build DOES rebuild dependents on a
  header change.

So the dependency tracker is sound. The stale objects came from a **desynced
incremental-build state**, not a tracker defect. The realistic triggers in
this workflow:

1. **Interrupted / killed builds.** These full rebuilds take minutes; in the
   agentic loop they're frequently run under a Bash timeout, `run_in_background`,
   or get cancelled. A long MSBuild killed mid-flight can leave a subset of
   objs recompiled and others stale, with `.lastbuildstate`/tlog and obj
   timestamps no longer in agreement — and the project can still *link* against
   the mixed set on a later partial build.
2. **Timestamp anomalies that defeat the mtime up-to-date check.** MSBuild
   decides "up to date" by comparing mtimes. Anything that makes the edited
   header's mtime NOT advance past an obj — a git checkout/stash/restore that
   resets mtimes, a file-sync or editor that preserves mtime, or clock skew —
   makes MSBuild skip a TU that genuinely needs recompiling. (The reported
   "`node_graph.obj` older than `node_graph.h`" from the 2026-06-09 incident is
   exactly this class of anomaly.)

**How to avoid (unchanged, still correct):** after any change to a struct
layout in a widely-included header (`node_graph.h` especially), do a clean
rebuild rather than trusting the incremental build — `cmake --build build
--config Release --clean-first` (or delete the app `.obj` files under
`build/SEANCE.dir/Release`). At minimum, **let the incremental build run to
completion uninterrupted**, and if anything looks off, verify no
`SEANCE.dir/Release/*.obj` is older than the edited header. The mitigation
targets the desynced-state symptom; the build tooling itself is not broken,
so this is a rare/transient hazard, not a guaranteed recurrence on every
layout-changing header edit.

## Stereoscopic viewport geometry (IPD / viewing distance / depth) has no UI

**Where:** `cpp/src/layered_wave_editor.cpp` — `ScatterView` members `ipdMm`
(63), `viewingDistMm` (600), `sceneDepthMm` (60), consumed by
`applyParallax()`. `dpi` is the only one actually written at runtime
(`LayeredWaveEditorComponent` auto-detects it from the JUCE display, ~line
9487).

The four stereoscopic View modes (Flat 2D / Anaglyph / Cross-eyed / Parallel)
compute per-eye disparity from real viewing geometry: a point at depth *z* is
offset by `(IPD/2) · z/(D+z)` mm, converted to pixels via the display DPI.
Three of those four inputs are hardcoded defaults that no control can change.
The comment above the members claims "The parent editor pushes these in from
its sliders so the parallax matches the user's actual eyes / screen" — those
sliders do not exist, and never did. (`grep ipdMm` finds only the declaration
and the one use site.)

This matters more than a normal missing-control gap because stereo fusion is
physiological, not cosmetic. Someone whose IPD is well off 63 mm, or who sits
much closer/farther than 600 mm, gets disparity that is wrong rather than
merely suboptimal — at best the image is uncomfortable to fuse, at worst it
won't fuse at all and the mode looks broken. Scene depth is the "how dramatic"
control and is the one most likely to be wanted routinely: 60 mm is a
conservative setting chosen to fuse easily, so users who fuse well have no way
to ask for more depth.

**Proper fix:** three sliders in the arrangement view's sidebar, shown only
when the View combo is on a 3D mode (and greyed with an explaining tooltip
otherwise, per the grayed-out-controls rule). Ranges roughly IPD 50–75 mm,
viewing distance 300–1200 mm, scene depth 0–200 mm. They should persist with
the editor's view preferences rather than the wavetable document — they
describe the *user's eyes and desk*, not the patch, so they must not travel
with a shared `.ssp`/wavetable file. DPI stays auto-detected with no control.

Two doc surfaces described these sliders as if they existed
(`REFERENCE.md` § "3D anaglyph viewport" and `docs/wavetables.html`); both were
corrected to describe the shipping UI, and REFERENCE.md now points here.
