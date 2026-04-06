"""
SoundShop2 Feature Test Script
Run this in the Script Console (View > Script Console)
It will set up a test project exercising all major features.

WHAT TO EXPECT:
1. Creates a MIDI track with a C major scale melody
2. Creates a second MIDI track with a chord progression
3. Both connect to the built-in test synth -> Master Out
4. Click Play to hear the melody and chords
5. Click piano keys on the left of each editor to audition notes
6. Test the toolbar buttons (transpose, duration, etc.)
7. Right-click notes for context menus
8. Try Ctrl+Z to undo, Ctrl+Y to redo
9. Save with Ctrl+S, reload with Ctrl+O

After running this script, also try:
- Zoom: Ctrl+scroll in the piano roll
- Pan: Shift+scroll in the piano roll
- Scale highlighting: change Scale dropdown to "Blues" or "Pentatonic"
- Detect Key: right-click empty space > Detect Key
- Performance mode: uncomment the line at the bottom and re-run
"""

import soundshop as ss
import soundshop_tools as tools

print("=== SoundShop2 Feature Test ===")
print()

# Show current state
tools.print_project()
print()

# --- FEATURE 1: Add notes to the MIDI track ---
# Find or identify the MIDI track (node index 0 should be Master Out after clean start)
names = ss.get_node_names()
print(f"Current nodes: {names}")
print()

# We need to create nodes via the graph UI first.
# This script assumes you have:
#   - A MIDI Track (created via "+ MIDI Track" button)
#   - Connected to an instrument (right-click > Instruments > any)
#   - Connected to Master Out
#
# If you haven't set that up yet, do it now and re-run this script.

# Find MIDI tracks
midi_tracks = []
for i, name in enumerate(names):
    node = ss.get_node(i)
    if node['type'] == 1:  # MIDI Timeline
        midi_tracks.append((i, name))

if not midi_tracks:
    print("No MIDI tracks found!")
    print("Please create a MIDI track first:")
    print("  1. Click '+ MIDI Track' in the toolbar")
    print("  2. Right-click the graph > Instruments > pick one (or use built-in)")
    print("  3. Connect: MIDI Track -> Instrument -> Master Out")
    print("  4. Double-click the MIDI track to open its editor")
    print("  5. Re-run this script")
else:
    idx, name = midi_tracks[0]
    print(f"Using MIDI track: [{idx}] {name}")
    print()

    # --- FEATURE: Add a scale ---
    print("Adding C major scale...")
    tools.add_scale(idx, 0, 'C', 'ionian', octave=4, start_beat=0, note_duration=0.5)
    print()

    # --- FEATURE: Add a melody from degrees ---
    print("Adding melody from scale degrees...")
    tools.add_melody_from_degrees(idx, 0, 'C', 'ionian',
        [(0, 1), (2, 0.5), (4, 0.5), (5, 1), (4, 0.5), (2, 0.5), (0, 2)],
        octave=4, start_beat=8)
    print()

    # --- FEATURE: Add an arpeggio ---
    print("Adding arpeggio...")
    tools.add_arpeggio(idx, 0, 'C4 E4 G4 C5',
        start_beat=16, note_duration=0.25, pattern='updown', repeats=2)
    print()

    # --- FEATURE: MIDI CC events ---
    print("Adding MIDI CC modulation sweep...")
    for i in range(16):
        ss.add_cc(idx, 0, 1, i * 1.0, int(i * 8))  # CC1 (mod wheel) sweep
    print("  Added 16 CC events")
    print()

    # --- FEATURE: Detect key ---
    node = ss.get_node(idx)
    pitches = [n['pitch'] for c in node['clips'] for n in c['notes']]
    if pitches:
        from soundshop_music import detect_keys
        keys = detect_keys(pitches)
        print("Key detection results (top 5):")
        for root, scale_name, coverage in keys[:5]:
            print(f"  {root} {scale_name}: {coverage:.0%}")
    print()

    # --- FEATURE: BPM ---
    print(f"Current BPM: {ss.get_bpm()}")
    ss.set_bpm(120)
    print(f"Set BPM to 120")
    print()

    # --- FEATURE: Tuning ---
    print(f"Current tuning: A={ss.get_tuning()} Hz")
    # ss.set_tuning_verdi()  # Uncomment to try A=432
    print()

    # --- FEATURE: Performance mode ---
    # Uncomment these lines to enable performance mode:
    # ss.set_performance_mode(idx, 1)       # enable
    # ss.set_performance_release(idx, 1)    # legato mode
    # ss.set_performance_velocity(idx, 1)   # velocity sensitive
    # print("Performance mode ENABLED - press any key to advance through melody")

    if len(midi_tracks) > 1:
        idx2, name2 = midi_tracks[1]
        print(f"Also found second MIDI track: [{idx2}] {name2}")
        print("Adding chord progression to it...")
        tools.add_chord_progression(idx2, 0, 'C',
            [('I', 4), ('V', 4), ('vi', 4), ('IV', 4)],
            octave=3, start_beat=0)

print()
print("=== Test setup complete! ===")
print()
print("Now try:")
print("  - Press PLAY to hear the melody")
print("  - Click piano keys on the left to audition")
print("  - Drag notes to move them")
print("  - Drag note edges to resize")
print("  - Right-click for full context menu")
print("  - Ctrl+scroll to zoom horizontally")
print("  - Change Scale dropdown to see highlighting")
print("  - Ctrl+Z / Ctrl+Y for undo/redo")
print("  - Ctrl+S to save, Ctrl+O to open")
print("  - Right-click plugin nodes > Show Plugin UI")
