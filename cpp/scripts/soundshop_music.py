"""
SoundShop Music Theory Module
Cleaned up from notes_manipulation.py — preserves full versatility,
fixes bugs, integrates with SoundShop's C API.
"""

import re

use_unicode_accidentals = False

note_re = re.compile(r"([a-zA-Z])(♯|♯♯|♭|♭♭|#|##|b|bb|)(-1|[0-9]|)")
noteoro_re = re.compile(r"([a-zA-Z](?:♯|♯♯|♭|♭♭|#|##|b|bb|)(?:-1|[0-9]|))|[Oo](-1|[0-9])")

letters = "CDEFGAB"
intervals = [2, 2, 1, 2, 2, 1, 1]  # W W H W W W H (major scale)
# Fix: last interval should be 1 to complete the octave
# C(2)D(2)E(1)F(2)G(2)A(2)B(1)C = 12 semitones
intervals = [2, 2, 1, 2, 2, 2, 1]

start_dict = {
    'A': '', 'Ab': 'b', 'B': '', 'Bb': 'b', 'C': '', 'C#': '#', 'Cb': 'b',
    'D': '', 'D#': '#', 'Db': 'b', 'E': '', 'Eb': 'b', 'F': '', 'F#': '#',
    'G': '', 'Gb': 'b'
}

mode_names = ["Ionian", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Aeolian", "Locrian"]
modes_dict = dict(zip((m.lower() for m in mode_names), range(7)))

notes_dict = {}      # "C4" -> midi number
semitones_dict = {}  # midi number -> [list of note name strings]
key_tables = {}      # key_name -> [7 mode dicts]

extra_scales = {
    'acoustic': [0, 2, 4, 6, 7, 9, 10],
    'algerian': [0, 2, 3, 6, 7, 8, 11],
    'augmented': [0, 3, 4, 7, 8, 11],
    'bebop dominant': [0, 2, 4, 5, 7, 9, 10, 11],
    'blues': [0, 3, 5, 6, 7, 10],
    'chromatic': list(range(12)),
    'double harmonic': [0, 1, 4, 5, 7, 8, 11],
    'enigmatic': [0, 1, 4, 6, 8, 10, 11],
    'flamenco': [0, 1, 4, 5, 7, 8, 11],
    'half-diminished': [0, 2, 3, 5, 6, 8, 10],
    'harmonic major': [0, 2, 4, 5, 7, 8, 11],
    'harmonic minor': [0, 2, 3, 5, 7, 8, 11],
    'harmonics': [0, 3, 4, 5, 7, 9],
    'hirajoshi': [0, 4, 6, 7, 11],
    'hungarian major': [0, 3, 4, 6, 7, 9, 10],
    'hungarian minor': [0, 2, 3, 6, 7, 8, 11],
    'in': [0, 1, 5, 7, 8],
    'insen': [0, 1, 5, 7, 10],
    'iwato': [0, 1, 5, 6, 10],
    'melodic minor': [0, 2, 3, 5, 7, 9, 11],
    'neapolitan major': [0, 1, 3, 5, 7, 9, 11],
    'neapolitan minor': [0, 1, 3, 5, 7, 8, 11],
    'octatonic (w-h)': [0, 2, 3, 5, 6, 8, 9, 11],
    'octatonic (h-w)': [0, 1, 3, 4, 6, 7, 9, 10],
    'pentatonic major': [0, 2, 4, 7, 9],
    'pentatonic minor': [0, 3, 5, 7, 10],
    'persian': [0, 1, 4, 5, 6, 8, 11],
    'phrygian dominant': [0, 1, 4, 5, 7, 8, 10],
    'prometheus': [0, 2, 4, 6, 9, 10],
    'romani': [0, 2, 3, 6, 7, 8, 10],
    'super locrian': [0, 1, 3, 4, 6, 8, 10],
    'tritone': [0, 1, 4, 6, 7, 10],
    'two-semitone tritone': [0, 1, 2, 6, 7, 8],
    'ukrainian dorian': [0, 2, 3, 6, 7, 9, 10],
    'whole tone': [0, 2, 4, 6, 8, 10],
    'yo': [0, 3, 5, 7, 10],
}


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

        # Resolve letter from MIDI number
        if self.letter is None and self.midi is not None:
            # Find the most common enharmonic spelling
            if self.midi in semitones_dict:
                candidates = semitones_dict[self.midi]
                # Prefer natural notes, then sharps, then flats
                for c in candidates:
                    if isinstance(c, str):
                        m = note_re.match(c)
                        if m:
                            self.letter = m.group(1)
                            self.accidental = m.group(2) or ''
                            oct_str = m.group(3)
                            if oct_str:
                                self.octave = int(oct_str)
                            break

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

    semi = -2
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


def build_table(key, mode=0):
    """Build a list of semitone values for a key and mode."""
    if isinstance(key, str):
        key_midi = notes_dict.get(key, 0)
    elif isinstance(key, Note):
        key_midi = key.midi
    else:
        key_midi = key

    if isinstance(mode, str):
        mode = modes_dict.get(mode.lower(), 0)

    table = []
    semi = key_midi % 12
    for i in (intervals[mode:] + intervals[:mode]):
        table.append(semi)
        semi = (semi + i) % 12
    return table


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


def change_key(notes, key1=None, mode1=0, key2=None, mode2=0):
    """Transpose notes from one key to another, preserving scale degrees."""
    if isinstance(notes, str):
        notes = notes.split()
    if isinstance(mode1, str):
        mode1 = modes_dict.get(mode1.lower(), 0)
    if isinstance(mode2, str):
        mode2 = modes_dict.get(mode2.lower(), 0)

    table1 = build_table(key1, mode1)
    table2 = build_table(key2, mode2)

    result = []
    for note in notes:
        if isinstance(note, Note):
            semi = note.midi
        elif isinstance(note, str):
            semi = notes_dict.get(note, 0)
        elif isinstance(note, int):
            semi = note
        else:
            continue

        octave, semi_mod = divmod(semi, 12)
        if semi_mod in table1:
            idx = table1.index(semi_mod)
            if idx < len(table2):
                result.append(octave * 12 + table2[idx])
            else:
                result.append(semi)  # can't map, keep original
        else:
            result.append(semi)  # not in source scale, keep original

    return result


def shift_semitones(notes, x):
    """Shift all notes by x semitones."""
    result = []
    if isinstance(notes, str):
        notes = notes.split()
    for note in notes:
        if isinstance(note, str):
            semi = notes_dict.get(note, 0)
        elif isinstance(note, Note):
            semi = note.midi
        elif isinstance(note, int):
            semi = note
        else:
            continue
        result.append(semi + x)
    return result


def shift_octaves(notes, octaves):
    """Shift all notes by the given number of octaves."""
    return shift_semitones(notes, octaves * 12)


def detect_keys(pitches):
    """
    Detect possible keys/modes for a set of MIDI pitches.
    Returns list of (root_name, scale_name, coverage) sorted by best fit.
    """
    if not pitches:
        return []

    pitch_classes = set(p % 12 for p in pitches)
    num_input = len(pitch_classes)
    results = []

    note_names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']

    # Check major scale modes
    for root in range(12):
        for mode_idx, mode_name in enumerate(mode_names):
            scale_semis = set()
            semi = root
            for iv in (intervals[mode_idx:] + intervals[:mode_idx]):
                scale_semis.add(semi % 12)
                semi += iv
            if pitch_classes.issubset(scale_semis):
                coverage = num_input / len(scale_semis)
                results.append((note_names[root], mode_name, coverage))

    # Check extra scales
    for scale_name, scale_intervals in extra_scales.items():
        if len(scale_intervals) >= 12:
            continue  # skip chromatic
        for root in range(12):
            scale_semis = set((s + root) % 12 for s in scale_intervals)
            if pitch_classes.issubset(scale_semis):
                coverage = num_input / len(scale_semis)
                results.append((note_names[root], scale_name, coverage))

    # Sort by coverage (best fit first), then scale size
    results.sort(key=lambda x: (-x[2], x[1]))
    return results


def merge_notes(*note_lists):
    """Merge multiple Notes lists, sorted by beat position."""
    all_notes = []
    for nl in note_lists:
        if isinstance(nl, (list, Notes)):
            all_notes.extend(nl)
    return Notes(sorted(all_notes, key=lambda n: (n.beat_number or 0, n.play_order or 0)))


# Initialize tables on import
make_tables()
