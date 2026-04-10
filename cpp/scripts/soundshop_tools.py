"""
SoundShop Tools — high-level helpers for populating timelines with notes.
Combines the soundshop C API with soundshop_music for easy music creation.

Usage:
    import soundshop_tools as tools
    tools.add_scale(0, 0, "C", "major", octave=4, start_beat=0)
    tools.add_chord_progression(0, 0, "C", [("I", 4), ("V", 4), ("vi", 4), ("IV", 4)])
"""

import soundshop as ss
from soundshop_music import (
    Note, Notes, notes_dict, build_table, mode_names, modes_dict,
    extra_scales, detect_keys, change_key, get_notes
)


def find_midi_node(name=None):
    """Find a MIDI timeline node by name, or return the first one.
    Returns (node_index, node_dict) or (None, None)."""
    names = ss.get_node_names()
    for i, n in enumerate(names):
        node = ss.get_node(i)
        if node['type'] == 1:  # MidiTimeline
            if name is None or n == name:
                return i, node
    return None, None


def ensure_clip(node_idx, clip_idx=0):
    """Make sure a clip exists at the given index."""
    node = ss.get_node(node_idx)
    if clip_idx < node['num_clips']:
        return clip_idx
    # Can't create clips from Python yet, use existing
    return 0 if node['num_clips'] > 0 else None


def add_scale(node_idx, clip_idx, root, scale_type="major", octave=4,
              start_beat=0, note_duration=0.5, ascending=True, descending=False):
    """Add a scale to a clip.

    Args:
        root: Note name like "C", "D#", "Eb"
        scale_type: "major", "minor", mode name, or extra scale name
        octave: Starting octave
        start_beat: Where to start placing notes
        note_duration: Duration of each note in beats
        ascending: Include ascending run
        descending: Include descending run
    """
    # Get scale intervals
    scale_type_lower = scale_type.lower()
    if scale_type_lower in modes_dict:
        intervals = build_table(root, modes_dict[scale_type_lower])
    elif scale_type_lower in extra_scales:
        root_midi = notes_dict.get(root, 0)
        intervals = [(s + root_midi) % 12 for s in extra_scales[scale_type_lower]]
    elif scale_type_lower in ('major', 'natural minor', 'harmonic minor', 'melodic minor'):
        mode = {'major': 0, 'natural minor': 5, 'harmonic minor': 5, 'melodic minor': 5}
        intervals = build_table(root, mode.get(scale_type_lower, 0))
    else:
        print(f"Unknown scale type: {scale_type}")
        return

    # Convert to MIDI pitches in the given octave
    root_midi = notes_dict.get(root, 0) % 12
    pitches = sorted(set((s % 12) + (octave + 1) * 12 for s in intervals))
    # Add the octave note at the top
    pitches.append(pitches[0] + 12)

    beat = start_beat
    notes_added = 0

    if ascending:
        for p in pitches:
            ss.add_note(node_idx, clip_idx, p, beat, note_duration)
            beat += note_duration
            notes_added += 1

    if descending:
        for p in reversed(pitches[:-1]):  # skip the top note (already played)
            ss.add_note(node_idx, clip_idx, p, beat, note_duration)
            beat += note_duration
            notes_added += 1

    print(f"Added {notes_added} notes ({root} {scale_type}, octave {octave})")
    return notes_added


def add_chord(node_idx, clip_idx, pitches, start_beat=0, duration=1.0):
    """Add a chord (multiple simultaneous notes).

    Args:
        pitches: List of MIDI note numbers, or a string like "C4 E4 G4"
    """
    if isinstance(pitches, str):
        notes = get_notes(pitches)
        pitches = [n.midi for n in notes]

    for p in pitches:
        ss.add_note(node_idx, clip_idx, p, start_beat, duration)
    return len(pitches)


# Chord degree to semitone offset map (relative to major scale)
_DEGREE_MAP = {
    'I': 0, 'II': 1, 'III': 2, 'IV': 3, 'V': 4, 'VI': 5, 'VII': 6,
    'i': 0, 'ii': 1, 'iii': 2, 'iv': 3, 'v': 4, 'vi': 5, 'vii': 6,
}

def _degree_to_chord(root, degree_str, octave=4):
    """Convert a Roman numeral degree to a list of MIDI pitches (triad)."""
    degree_str = degree_str.strip()
    is_minor = degree_str[0].islower() if degree_str else False
    degree_upper = degree_str.upper()

    if degree_upper not in _DEGREE_MAP:
        print(f"Unknown degree: {degree_str}")
        return []

    scale = build_table(root, 0)  # major scale
    degree = _DEGREE_MAP[degree_upper]

    root_pitch = scale[degree] + (octave + 1) * 12
    third = scale[(degree + 2) % 7]
    fifth = scale[(degree + 4) % 7]

    # Adjust octave for wrapping
    third_pitch = third + (octave + 1) * 12
    if third_pitch <= root_pitch:
        third_pitch += 12
    fifth_pitch = fifth + (octave + 1) * 12
    if fifth_pitch <= third_pitch:
        fifth_pitch += 12

    # For minor chords (lowercase), flatten the third
    if is_minor:
        third_pitch = root_pitch + 3  # minor third
        fifth_pitch = root_pitch + 7  # perfect fifth
    else:
        third_pitch = root_pitch + 4  # major third
        fifth_pitch = root_pitch + 7  # perfect fifth

    return [root_pitch, third_pitch, fifth_pitch]


def add_chord_progression(node_idx, clip_idx, key, progression, octave=4,
                           start_beat=0):
    """Add a chord progression.

    Args:
        key: Root key like "C"
        progression: List of (degree, duration) tuples, e.g.:
            [("I", 4), ("V", 4), ("vi", 4), ("IV", 4)]
            Degrees: I II III IV V VI VII (uppercase=major, lowercase=minor)
    """
    beat = start_beat
    total_notes = 0
    for degree, duration in progression:
        pitches = _degree_to_chord(key, degree, octave)
        for p in pitches:
            ss.add_note(node_idx, clip_idx, p, beat, duration)
            total_notes += 1
        beat += duration

    print(f"Added {total_notes} notes ({len(progression)} chords in {key})")
    return total_notes


def add_arpeggio(node_idx, clip_idx, pitches, start_beat=0, note_duration=0.25,
                  pattern="up", repeats=1):
    """Add an arpeggiated pattern.

    Args:
        pitches: MIDI pitches or string like "C4 E4 G4"
        pattern: "up", "down", "updown", "random"
        repeats: Number of times to repeat
    """
    if isinstance(pitches, str):
        notes = get_notes(pitches)
        pitches = [n.midi for n in notes]

    import random
    beat = start_beat
    total = 0
    for _ in range(repeats):
        if pattern == "up":
            seq = pitches
        elif pattern == "down":
            seq = list(reversed(pitches))
        elif pattern == "updown":
            seq = pitches + list(reversed(pitches[1:-1]))
        elif pattern == "random":
            seq = pitches[:]
            random.shuffle(seq)
        else:
            seq = pitches

        for p in seq:
            ss.add_note(node_idx, clip_idx, p, beat, note_duration)
            beat += note_duration
            total += 1

    print(f"Added {total} notes (arpeggio, pattern={pattern})")
    return total


def add_rhythm(node_idx, clip_idx, pitch, pattern, start_beat=0, beat_value=0.25):
    """Add a rhythmic pattern on a single pitch.

    Args:
        pitch: MIDI pitch
        pattern: String of 'x' (hit) and '.' (rest), e.g. "x..x..x."
        beat_value: Duration of each character in beats
    """
    beat = start_beat
    total = 0
    for ch in pattern:
        if ch.lower() == 'x':
            ss.add_note(node_idx, clip_idx, pitch, beat, beat_value)
            total += 1
        beat += beat_value
    print(f"Added {total} hits")
    return total


def add_melody_from_degrees(node_idx, clip_idx, key, mode, degrees,
                             octave=4, start_beat=0, default_duration=0.5):
    """Add a melody specified as scale degrees.

    Args:
        degrees: List of (degree, duration) tuples or just degrees.
            Degree 0 = root, 1 = 2nd, etc. Negative = below root.
            Example: [(0, 1), (2, 0.5), (4, 0.5), (7, 2)]  # root, third, fifth, octave
            Or just: [0, 2, 4, 5, 7]  # all same duration
    """
    if isinstance(mode, str):
        mode = modes_dict.get(mode.lower(), 0)

    scale = build_table(key, mode)
    beat = start_beat
    total = 0

    for item in degrees:
        if isinstance(item, tuple):
            deg, dur = item
        else:
            deg, dur = item, default_duration

        # Compute pitch from degree
        oct_offset = 0
        d = deg
        while d < 0:
            d += len(scale)
            oct_offset -= 1
        while d >= len(scale):
            d -= len(scale)
            oct_offset += 1

        pitch = scale[d] + (octave + 1 + oct_offset) * 12
        ss.add_note(node_idx, clip_idx, pitch, beat, dur)
        beat += dur
        total += 1

    print(f"Added {total} notes (melody from degrees in {key})")
    return total


def add_notes(node_idx, clip_idx, notes, start_beat=None):
    """Add a Notes collection (or list of Note objects) to a clip.

    Each note's beat_number or offset is used for positioning.
    If start_beat is provided, notes are offset from that beat.
    If notes have beat_interval set, they're placed sequentially.

    Args:
        node_idx: Node index
        clip_idx: Clip index
        notes: Notes object, list of Note objects, or a string like "C4 E4 G4"
        start_beat: Optional starting beat (overrides note positions)
    """
    if isinstance(notes, str):
        notes = Notes(notes)

    beat = start_beat if start_beat is not None else 0
    total = 0

    for note in notes:
        # Determine position
        if note.beat_number is not None and start_beat is None:
            pos = note.beat_number
        elif note.beat_interval is not None:
            if total > 0:
                beat += note.beat_interval
            pos = beat
        else:
            pos = beat

        # Determine duration
        dur = note.beat_duration if note.beat_duration else 1.0

        # Determine velocity (not used in add_note yet, but stored)
        pitch = note.midi
        if pitch is None:
            print(f"Warning: skipping note with no MIDI number: {note}")
            continue

        detune = note.detune or 0.0

        ss.add_note(node_idx, clip_idx, pitch, pos, dur)

        # If notes don't have explicit positions, advance by duration
        if note.beat_number is None and note.beat_interval is None:
            beat = pos + dur

        total += 1

    print(f"Added {total} notes")
    return total


def notes_to_timeline(node_name, notes, clip_idx=0, start_beat=None):
    """Convenience: find a MIDI node by name and add notes to it.

    Args:
        node_name: Name of the MIDI track, or None for first one
        notes: Notes object, list, or string
        clip_idx: Which clip (default 0)
        start_beat: Optional starting beat
    """
    idx, node = find_midi_node(node_name)
    if idx is None:
        print(f"No MIDI track found{' named ' + node_name if node_name else ''}")
        return 0
    return add_notes(idx, clip_idx, notes, start_beat)


def print_project():
    """Print a summary of the project."""
    names = ss.get_node_names()
    print(f"BPM: {ss.get_bpm()}")
    print(f"Nodes: {len(names)}")
    for i, name in enumerate(names):
        node = ss.get_node(i)
        type_names = {0: 'Audio Track', 1: 'MIDI Track', 2: 'Instrument',
                      3: 'Effect', 4: 'Mixer', 5: 'Output', 6: 'Script', 7: 'Group'}
        t = type_names.get(node['type'], '?')
        print(f"  [{i}] {name} ({t})")
        for ci, clip in enumerate(node['clips']):
            print(f"       Clip {ci}: '{clip['name']}' "
                  f"{clip['start_beat']}-{clip['start_beat']+clip['length_beats']} beats, "
                  f"{clip['num_notes']} notes")
