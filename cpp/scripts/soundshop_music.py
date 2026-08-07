"""
SoundShop Music Theory Module

Note spelling and note objects, on top of SEANCE's own music theory.

WHERE THE THEORY COMES FROM. Every scale table, key detection and change-key
result in this module is read out of the built-in `soundshop` module, which
exposes the exact `MusicTheory` tables the piano roll's Key / Mode / Scale
controls, the Analyze button and Change Key use. This file used to carry its own
hand-written copy of all of that, and it had drifted: a different scale list,
different names, and a key-detection ranking that disagreed with the app's - so
a script and the piano roll could tell you different things about the same
melody. There is now one table and one answer.

WHAT THIS MODULE STILL ADDS, and why it isn't just `import soundshop`:

  * Enharmonic SPELLING. `soundshop` speaks MIDI numbers and one canonical name
    per pitch class ("C#", never "Db"). This module knows that the third of Eb
    major is spelled G and the seventh is D, that "Cb4" is a real note one
    semitone below C4, and that a key signature picks letters in order - the
    stuff you need to print or parse notation rather than to compute pitches.
  * The `Note` / `Notes` objects, which carry timing (beats / samples /
    seconds), velocity and detune alongside the pitch, so a generated phrase can
    be handed straight to soundshop_tools.add_notes().

If you only need pitches, prefer `soundshop` directly - it's the source these
tables are built from.
"""

import re

# The built-in module. This file lives on sys.path only inside SEANCE's embedded
# interpreter, so a failure here means someone ran it in a plain CPython; say so
# rather than silently falling back to a private copy of the tables, which is the
# exact divergence this module was rewritten to remove.
try:
    import soundshop as _ss
except ImportError as e:      # pragma: no cover - only reachable outside SEANCE
    raise ImportError(
        "soundshop_music requires SEANCE's built-in 'soundshop' module "
        "(it reads the app's music-theory tables from it). Run this from the "
        "Script Console, not from a standalone Python."
    ) from e

use_unicode_accidentals = False

# Accidental alternatives are longest-first. Python's `|` takes the FIRST branch
# that matches, not the longest, so listing "#" before "##" made the double
# accidentals unreachable: "Fbb5" parsed as F-flat with no octave (F4-1 = 64)
# instead of F-double-flat in octave 5 (75), and Note(61) printed as "B##3"
# because "B##3" appeared to carry a single sharp.
note_re = re.compile(r"([a-zA-Z])(♯♯|♯|♭♭|♭|##|#|bb|b|)(-1|[0-9]|)")
noteoro_re = re.compile(r"([a-zA-Z](?:♯♯|♯|♭♭|♭|##|#|bb|b|)(?:-1|[0-9]|))|[Oo](-1|[0-9])")

letters = "CDEFGAB"

# The letter ladder: how many semitones from each letter to the next. Derived
# from the app's major scale rather than written out, so the two can't disagree.
# C(2)D(2)E(1)F(2)G(2)A(2)B(1)C = 12 semitones.
_major = _ss.scale_intervals("Major")
intervals = [b - a for a, b in zip(_major, _major[1:] + [12])]

start_dict = {
    'A': '', 'Ab': 'b', 'B': '', 'Bb': 'b', 'C': '', 'C#': '#', 'Cb': 'b',
    'D': '', 'D#': '#', 'Db': 'b', 'E': '', 'Eb': 'b', 'F': '', 'F#': '#',
    'G': '', 'Gb': 'b'
}

# The seven modes, in the app's rotation order. The app labels index 0
# "Ionian (Major)"; the bare "Ionian" is what scripts have always written here,
# so strip the parenthetical and keep both spellings working.
mode_names = [n.split(' (')[0] for n in _ss.scale_names("mode")]
modes_dict = dict(zip((m.lower() for m in mode_names), range(len(mode_names))))

notes_dict = {}      # "C4" -> midi number
semitones_dict = {}  # midi number -> [list of note name strings]
key_tables = {}      # key_name -> [7 mode dicts]


def _build_scale_index():
    """Every scale SEANCE knows, keyed by lowercase name -> semitone offsets.

    Built from the app's three tables (keys, modes, fixed scales) at import
    time, so adding a scale in music_theory.cpp makes it available here with no
    edit to this file.
    """
    table = {}
    for name in _ss.scale_names():
        table[name.lower()] = _ss.scale_intervals(name)
    # Aliases for spellings older scripts used before this module was wired to
    # the app's tables. Same lists, different key.
    for alias, real in (('pentatonic major', 'Major Pentatonic'),
                        ('pentatonic minor', 'Minor Pentatonic'),
                        ('ionian', 'Ionian (Major)'),
                        ('minor', 'Natural Minor')):
        table[alias] = _ss.scale_intervals(real)
    return table


# Named `extra_scales` for backwards compatibility; it is no longer "extra" -
# it is now the app's complete scale list, modes and parent keys included.
extra_scales = _build_scale_index()


def convert_accidental(accidental, use_unicode=use_unicode_accidentals):
    """Convert between # and ♯, b and ♭ notation."""
    if accidental is None:
        return ''
    if isinstance(accidental, Note):
        if use_unicode:
            accidental.accidental = accidental.accidental.replace("#", "♯").replace("b", "♭")
        else:
            accidental.accidental = accidental.accidental.replace("♯", "#").replace("♭", "b")
        return accidental
    if use_unicode:
        return accidental.replace("#", "♯").replace("b", "♭")
    else:
        return accidental.replace("♯", "#").replace("♭", "b")


class Note:
    """
    A versatile note representation. Can be created from:
    - A string: Note("C4"), Note("Eb5"), Note("F#3")
    - A MIDI number: Note(60), Note(72, octave=5)
    - A degree in a key: Note(key="C", mode=0, degree=2)  # -> E
    - Another Note: Note(existing_note, octave=5)

    Timing can be specified in three mutually exclusive ways:
    - beat_number / beat_duration (relative to a timeline)
    - sample_number / sample_duration (absolute samples)
    - time_offset / time_duration (absolute seconds)

    Only one position type and one duration type can be set.
    """
    def __init__(self, note=None, accidental=None, octave=None, key=None, mode=0,
                 degree=None, velocity=127,
                 beat_number=None, beat_duration=1.0, beat_interval=None,
                 sample_number=None, sample_duration=None,
                 time_offset=None, time_duration=None,
                 play_order=None, detune=0.0):

        # Timing fields
        self.beat_number = beat_number
        self.beat_duration = beat_duration
        self.beat_interval = beat_interval
        self.sample_number = sample_number
        self.sample_duration = sample_duration
        self.time_offset = time_offset
        self.time_duration = time_duration
        self.play_order = play_order
        self.velocity = velocity
        self.detune = detune

        # Validate: at most one position type, one duration type
        positions = sum(x is not None for x in (beat_number, sample_number, time_offset))
        durations = sum(x is not None for x in (sample_duration, time_duration))
        # beat_duration has a default so don't count it unless explicitly set
        assert positions <= 1, "Only one of beat_number, sample_number, time_offset can be set"
        assert durations <= 1, "Only one of sample_duration, time_duration can be set"
        assert not (beat_interval and beat_number), "Can't set both beat_interval and beat_number"

        # Note identity fields
        self.letter = None
        self.accidental = convert_accidental(accidental) or ''
        self.octave = octave
        self.midi = None
        self.key = None
        self.mode = None
        self.degree = degree
        self.pitch_class = None

        # Parse mode
        if isinstance(mode, str):
            self.mode = modes_dict.get(mode.lower(), 0)
        else:
            self.mode = mode

        # Parse key
        if isinstance(key, str):
            self.key = key
        elif isinstance(key, Note):
            self.key = key.pitch_class

        # Build note from input
        if isinstance(note, str):
            m = note_re.match(note)
            if not m:
                raise ValueError(f"Can't parse note string: {note}")
            self.letter, acc, oct_str = m.group(1, 2, 3)
            self.accidental = convert_accidental(acc) or ''
            if octave is not None:
                self.octave = octave
            elif oct_str:
                self.octave = int(oct_str)
            else:
                self.octave = 4

        elif isinstance(note, Note):
            self.octave = note.octave if octave is None else octave
            self.midi = note.midi
            self.letter = note.letter
            self.accidental = note.accidental
            self.key = key if key else note.key
            # Copy timing from source note if not overridden
            if beat_number is None: self.beat_number = note.beat_number
            if beat_duration == 1.0: self.beat_duration = note.beat_duration
            if sample_number is None: self.sample_number = note.sample_number
            if sample_duration is None: self.sample_duration = note.sample_duration
            if time_offset is None: self.time_offset = note.time_offset
            if time_duration is None: self.time_duration = note.time_duration
            if velocity == 127: self.velocity = note.velocity

        elif isinstance(note, int):
            self.midi = note if octave is None else (note % 12 + octave * 12)

        elif note is None:
            if self.key is not None and degree is not None:
                # Construct from key + degree
                key_str = convert_accidental(self.key, False)
                if key_str in key_tables and self.mode < len(key_tables[key_str]):
                    table = key_tables[key_str][self.mode]
                    if self.octave is None:
                        self.octave = 4
                    # Look up the semitone for this degree
                    scale = build_table(self.key, self.mode)
                    if degree < len(scale):
                        self.midi = scale[degree] + self.octave * 12
                else:
                    raise ValueError(f"Can't find key table for {self.key} mode {self.mode}")
            else:
                raise ValueError("Can't create Note: need note string, MIDI number, or key+degree")

        # Resolve octave if not set
        if self.octave is None:
            if self.letter:
                self.octave = 4 if self.letter >= "C" else 5
            else:
                self.octave = 4

        # Resolve MIDI number from letter + accidental + octave
        if self.midi is None and self.letter is not None:
            note_str = self.letter + self.accidental + str(self.octave)
            if note_str in notes_dict:
                self.midi = notes_dict[note_str]
            else:
                raise ValueError(f"Unknown note: {note_str}")

        # Resolve letter from MIDI number.
        #
        # A pitch has several legal spellings (60 is C4, B#3 and Dbb4) and
        # semitones_dict lists them in ladder order, which puts B#3 first. Pick
        # by how ordinary the accidental is instead: natural, then a single
        # sharp, then a single flat, then the doubles. Without the ranking,
        # Note(60) printed "B#3".
        if self.letter is None and self.midi is not None:
            _ACC_RANK = {'': 0, '#': 1, 'b': 2, '##': 3, 'bb': 4}
            best, best_rank = None, None
            for c in semitones_dict.get(self.midi, []):
                if not isinstance(c, str):
                    continue
                m = note_re.match(c)
                if not m:
                    continue
                rank = _ACC_RANK.get(m.group(2), 5)
                if best_rank is None or rank < best_rank:
                    best, best_rank = m, rank
            if best is not None:
                self.letter = best.group(1)
                self.accidental = best.group(2) or ''
                if best.group(3):
                    self.octave = int(best.group(3))

        # Build full note string
        self.note = (self.letter or '?') + (self.accidental or '') + str(self.octave)
        self.pitch_class = (self.letter or '?') + (self.accidental or '')

        # Compute degree if we have a key
        if self.key is not None and self.midi is not None and self.degree is None:
            try:
                table = build_table(self.key, self.mode or 0)
                semi = self.midi % 12
                if semi in table:
                    self.degree = table.index(semi)
            except (ValueError, KeyError):
                pass

    def __str__(self):
        return self.note

    def __repr__(self):
        return f"Note('{self.note}', midi={self.midi})"

    def transpose(self, semitones):
        """Return a new Note transposed by the given number of semitones."""
        return Note(self.midi + semitones)

    def in_key(self, key, mode=0):
        """Return a copy of this note with key/degree information."""
        return Note(self, key=key, mode=mode)


class Notes(list):
    """
    A collection of Notes with shared properties (key, bpm, timing).
    Can be created from a string: Notes("C4 E4 G4", key="C")
    Or from a list: Notes([Note(60), Note(64), Note(67)])
    """
    def __init__(self, notes=None, key=None, bpm=None, mode=0, accidental=None,
                 sample_offset=None, beat_number=None, time_offset=None,
                 sample_duration=None, beat_duration=None, time_duration=None,
                 order_offset=0):

        positions = sum(x is not None for x in (beat_number, time_offset, sample_offset))
        durations = sum(x is not None for x in (sample_duration, beat_duration, time_duration))
        assert positions <= 1, "Only one position type allowed"
        assert durations <= 1, "Only one duration type allowed"

        self.key = key
        self.mode = mode
        self.bpm = bpm
        self.accidental = accidental
        self.beat_number = beat_number
        self.time_offset = time_offset
        self.sample_offset = sample_offset
        self.sample_duration = sample_duration
        self.beat_duration = beat_duration
        self.time_duration = time_duration

        parsed = []
        if isinstance(notes, str):
            octave = None
            for m in noteoro_re.findall(notes):
                if m[0]:
                    parsed.append(Note(m[0], octave=octave, key=key, mode=mode, accidental=accidental))
                else:
                    octave = int(m[1])
        elif isinstance(notes, (list, Notes)):
            for note in notes:
                if isinstance(note, Note):
                    if note.play_order is not None:
                        note.play_order += order_offset
                    if sample_offset is not None and note.sample_number is not None:
                        note.sample_number += sample_offset
                    if key is not None:
                        note.key = key
                    parsed.append(note)
                else:
                    parsed.append(Note(note, key=key, mode=mode))

        super().__init__(parsed)

    def __repr__(self):
        return f"Notes({list.__repr__(self)})"

    def transpose(self, semitones):
        """Return a new Notes with all notes transposed."""
        return Notes([Note(n.midi + semitones) for n in self])

    def in_key(self, key, mode=0):
        """Return a copy with key/degree assigned to all notes."""
        return Notes([n.in_key(key, mode) for n in self])


# ==============================================================================
# Table building
# ==============================================================================

def make_tables():
    """Build the global lookup tables."""
    global notes_dict, semitones_dict, key_tables

    # Walk the letter ladder from below MIDI 0 up past 127, naming every
    # spelling of every pitch on the way.
    #
    # The ladder starts on A two octaves below C-1, which is MIDI -3: from there
    # B is -1 and C is 0, i.e. C-1 = 0 and C4 = 60, the standard MIDI mapping
    # that soundshop.notenum() and the piano roll use. It used to start at -2,
    # which put every named note one semitone sharp - Note("C4").midi was 61 and
    # Note(60) printed as "B4". Nothing caught it because this table had no test
    # and nothing else in the app agreed with it to disagree with.
    semi = -3
    i = -2
    while semi < 129:
        interval = intervals[i % 7]
        oct_str = str((semi - 12) // 12)
        letter = letters[i % 7]
        for n, acc in enumerate(("bb", "b", "", "#", "##")):
            notes_dict[letter + acc + oct_str] = semi + n - 2
            semitones_dict.setdefault(semi + n - 2, []).append(letter + acc + oct_str)
        for n, acc in enumerate(("♭♭", "♭", "", "♯", "♯♯")):
            notes_dict[letter + acc + oct_str] = semi + n - 2
        semi += interval
        i += 1

    # Also without octave
    semi = 0
    for interval, letter in zip(intervals, letters):
        for n, acc in enumerate(("bb", "b", "", "#", "##")):
            notes_dict[letter + acc] = semi + n - 2
        for n, acc in enumerate(("♭♭", "♭", "", "♯", "♯♯")):
            notes_dict[letter + acc] = semi + n - 2
        semi += interval

    # Build key tables
    for key in start_dict:
        mode_tables = []
        for mode in range(7):
            mode_tables.append(build_notes(key, mode, start_dict[key]))
        key_tables[key] = mode_tables


def pitch_class(key):
    """Pitch class 0-11 of a key given as a name, a Note or a MIDI number.

    Names go through this module's spelling table, so "Cb" is 11 and "B#" is 0.
    """
    if isinstance(key, str):
        key_midi = notes_dict.get(key, 0)
    elif isinstance(key, Note):
        key_midi = key.midi
    else:
        key_midi = key
    return (key_midi or 0) % 12


def build_table(key, mode=0, scale="Major"):
    """Pitch classes of a key + mode, in scale order starting at the root.

    `mode` is a rotation degree (or a mode name); it is applied to `scale` the
    same way the piano roll's Mode control applies to its Key control - see
    soundshop.rotate_scale(). So build_table("D", 1) is D Dorian, and
    build_table("D", 5, "Harmonic Minor") is the fifth mode of D harmonic minor.

    Any scale SEANCE knows may be named: build_table("C", 0, "Blues").
    """
    if isinstance(mode, str):
        mode = modes_dict.get(mode.lower(), 0)
    root = pitch_class(key)
    return [(root + s) % 12 for s in _ss.rotate_scale(scale, mode)]


def build_notes(key, mode, accidental):
    """Build a dict mapping semitones to Note objects for a key/mode."""
    notes = {}
    key_note = Note(key)
    first_letter = key_note.letter
    table = build_table(key, mode)

    lifl = letters.index(first_letter)
    for semi, letter in zip(table, letters[lifl:] + letters[:lifl]):
        if semi in semitones_dict:
            for note_str in semitones_dict[semi]:
                if isinstance(note_str, str) and note_str[0] == letter:
                    notes[semi] = Note(note_str)
                    break
    return notes


# ==============================================================================
# Utility functions
# ==============================================================================

def get_notes(notes_str, key=None, mode=0, accidental=None):
    """Parse a space-separated string of notes."""
    if isinstance(notes_str, str):
        notes_str = notes_str.split()
    return [Note(n, key=key, mode=mode, accidental=accidental) for n in notes_str]


def change_key(notes, key1=None, mode1=0, key2=None, mode2=0,
               scale1="Major", scale2="Major"):
    """Re-derive notes from their scale degree in a new key.

    This is the piano roll's Change Key button: C major -> D natural minor keeps
    the SHAPE of the melody (each note keeps its degree and its accidental
    relative to that degree) instead of sliding every pitch up two semitones.

    Notes may be MIDI numbers, note-name strings or Note objects; the result is
    a list of MIDI numbers. `mode1` / `mode2` are rotation degrees or mode names
    applied to `scale1` / `scale2`, so any of SEANCE's scales can be either end
    of the move.

    Out-of-scale notes are carried across by their chromatic offset from the
    nearest degree rather than left where they were - which is what the app
    does, and what makes a chromatic passing tone survive the change.
    """
    pitches = shift_semitones(notes, 0)   # normalise everything to MIDI numbers
    if not pitches:
        return []
    if isinstance(mode1, str):
        mode1 = modes_dict.get(mode1.lower(), 0)
    if isinstance(mode2, str):
        mode2 = modes_dict.get(mode2.lower(), 0)
    return _ss.change_key(pitches,
                          pitch_class(key1), _ss.rotate_scale(scale1, mode1),
                          pitch_class(key2), _ss.rotate_scale(scale2, mode2))


def shift_semitones(notes, x):
    """Shift notes by x semitones, returning a list of MIDI numbers.

    Accepts a space-separated string ("C4 E4 G4"), or any iterable of note-name
    strings, Note objects and numbers. With x = 0 this is the module's
    "give me plain MIDI numbers" normaliser, which is how change_key() and
    detect_keys() accept all three spellings.
    """
    result = []
    if isinstance(notes, str):
        notes = notes.split()
    for note in notes:
        if isinstance(note, str):
            semi = notes_dict.get(note, 0)
        elif isinstance(note, Note):
            semi = note.midi
        elif isinstance(note, (int, float)):
            semi = int(note)
        else:
            continue
        result.append(semi + x)
    return result


def shift_octaves(notes, octaves):
    """Shift all notes by the given number of octaves."""
    return shift_semitones(notes, octaves * 12)


def detect_keys(pitches):
    """Candidate keys for a set of notes, best fit first.

    Returns a list of (root_name, scale_name, coverage) tuples. This is the same
    ranking the piano roll's key detection shows - tightest fit first, then
    simpler (smaller) scales - because it IS that ranking; see
    soundshop.detect_key(), which returns the same matches as dicts with more
    detail (category, scale size, notes matched).

    Scale names are the app's own labels, so the major scale comes back as
    "Major" (from the Key table) and "Ionian (Major)" (from the Mode table)
    rather than the bare "Ionian" this module used to invent.
    """
    pitches = shift_semitones(pitches, 0)
    if not pitches:
        return []
    return [(m["root_name"], m["scale"], m["coverage"])
            for m in _ss.detect_key(pitches, -1)]


def merge_notes(*note_lists):
    """Merge multiple Notes lists, sorted by beat position."""
    all_notes = []
    for nl in note_lists:
        if isinstance(nl, (list, Notes)):
            all_notes.extend(nl)
    return Notes(sorted(all_notes, key=lambda n: (n.beat_number or 0, n.play_order or 0)))


# Initialize tables on import
make_tables()
