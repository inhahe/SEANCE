"""SoundShop2 - Node-based DAW prototype"""

from imgui_bundle import imgui, imgui_node_editor as ed, immapp
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Optional
import math


class PinKind(Enum):
    AUDIO = auto()
    MIDI = auto()
    PARAM = auto()


class NodeType(Enum):
    AUDIO_TIMELINE = auto()
    MIDI_TIMELINE = auto()
    INSTRUMENT = auto()
    EFFECT = auto()
    MIXER = auto()
    OUTPUT = auto()
    SCRIPT = auto()


CHANNEL_LABELS = {
    1: "mono",
    2: "stereo",
    6: "5.1",
    8: "7.1",
}


@dataclass
class Pin:
    id: int
    name: str
    kind: PinKind
    is_input: bool
    channels: int = 2  # default stereo


@dataclass
class Param:
    name: str
    value: float
    min_val: float
    max_val: float
    format: str = "%.2f"


# Preset parameter sets for different effect types
EFFECT_PARAMS = {
    "Reverb": [
        Param("Room Size", 0.5, 0.0, 1.0),
        Param("Damping", 0.5, 0.0, 1.0),
        Param("Wet/Dry", 0.3, 0.0, 1.0),
    ],
    "Compressor": [
        Param("Threshold", -20.0, -60.0, 0.0, "%.1f dB"),
        Param("Ratio", 4.0, 1.0, 20.0, "%.1f:1"),
        Param("Attack", 10.0, 0.1, 100.0, "%.1f ms"),
        Param("Release", 100.0, 10.0, 1000.0, "%.0f ms"),
    ],
    "EQ": [
        Param("Low", 0.0, -12.0, 12.0, "%.1f dB"),
        Param("Mid", 0.0, -12.0, 12.0, "%.1f dB"),
        Param("High", 0.0, -12.0, 12.0, "%.1f dB"),
        Param("Mid Freq", 1000.0, 200.0, 8000.0, "%.0f Hz"),
    ],
    "Delay": [
        Param("Time", 250.0, 1.0, 2000.0, "%.0f ms"),
        Param("Feedback", 0.3, 0.0, 0.95),
        Param("Wet/Dry", 0.3, 0.0, 1.0),
    ],
    "Distortion": [
        Param("Drive", 0.5, 0.0, 1.0),
        Param("Tone", 0.5, 0.0, 1.0),
        Param("Level", 0.7, 0.0, 1.0),
    ],
    "Chorus": [
        Param("Rate", 1.0, 0.1, 10.0, "%.1f Hz"),
        Param("Depth", 0.5, 0.0, 1.0),
        Param("Wet/Dry", 0.5, 0.0, 1.0),
    ],
}


INSTRUMENT_PARAMS = {
    "Piano": [
        Param("Volume", 0.8, 0.0, 1.0),
        Param("Brightness", 0.5, 0.0, 1.0),
        Param("Reverb Send", 0.3, 0.0, 1.0),
    ],
    "Synth": [
        Param("Oscillator", 0.5, 0.0, 1.0),
        Param("Cutoff", 0.7, 0.0, 1.0),
        Param("Resonance", 0.3, 0.0, 1.0),
        Param("Envelope", 0.5, 0.0, 1.0),
    ],
    "Sampler": [
        Param("Volume", 0.8, 0.0, 1.0),
        Param("Pitch", 0.0, -12.0, 12.0, "%.1f st"),
        Param("Start", 0.0, 0.0, 1.0),
    ],
    "Drum Machine": [
        Param("Volume", 0.8, 0.0, 1.0),
        Param("Punch", 0.5, 0.0, 1.0),
        Param("Tone", 0.5, 0.0, 1.0),
    ],
}


def _default_params(name: str, node_type: NodeType) -> list[Param]:
    """Get default parameters for a named node."""
    lookup = {
        NodeType.EFFECT: EFFECT_PARAMS,
        NodeType.INSTRUMENT: INSTRUMENT_PARAMS,
    }
    table = lookup.get(node_type, {})
    if name in table:
        return [Param(p.name, p.value, p.min_val, p.max_val, p.format)
                for p in table[name]]
    return [Param("Mix", 0.5, 0.0, 1.0)]


class WaveformView(Enum):
    LR = auto()       # L/R stacked (default)
    MID_SIDE = auto()  # Mid/Side overlay


@dataclass
class MidiNote:
    offset: float       # beat offset within clip
    pitch: int          # MIDI pitch 0-127
    duration: float     # length in beats
    degree: int = 0     # scale degree (0-based: 0=1st, 1=2nd, ..., 6=7th)
    octave: int = 4     # octave for the degree
    chromatic_offset: int = 0  # semitones from the scale degree (for non-scale tones)
    detune: float = 0.0  # cents offset from standard pitch (-100 to +100)


@dataclass
class Clip:
    name: str
    start_beat: float   # where the clip starts on the timeline
    length_beats: float  # how long the clip is
    color: tuple[int, int, int] = (100, 150, 255)
    channels: int = 2   # 1=mono, 2=stereo
    notes: list[MidiNote] = field(default_factory=list)
    waveform_view: WaveformView = WaveformView.LR


@dataclass
class Node:
    id: int
    name: str
    node_type: NodeType
    pins_in: list[Pin] = field(default_factory=list)
    pins_out: list[Pin] = field(default_factory=list)
    pos: tuple[float, float] = (0.0, 0.0)
    pos_set: bool = False
    params: list[Param] = field(default_factory=list)
    clips: list[Clip] = field(default_factory=list)
    script: str = ""  # for script nodes


@dataclass
class Link:
    id: int
    start_pin: int  # output pin id
    end_pin: int    # input pin id


class SoundShop:
    def __init__(self):
        self._next_id = 1
        self.nodes: list[Node] = []
        self.links: list[Link] = []
        self._first_frame = True
        self.bpm = 120.0
        self.open_editors: list[Node] = []  # stack of open timeline editors
        self.editor_panel_height = 250.0  # resizable
        self._dragging_splitter = False
        # Piano roll state per node: {node_id: {editing_clip_idx, scroll_pitch, ...}}
        self._piano_roll_state: dict[int, dict] = {}
        self.show_script_console = False
        self.script_console_text = ("# SoundShop Script Console\n"
                                     "# Access the project via 'project'\n"
                                     "# Example:\n"
                                     "#   for clip in project.selected_timeline.clips:\n"
                                     "#     for note in clip.notes:\n"
                                     "#       note.pitch += 12\n")
        self.script_console_output = ""
        self._setup_default_graph()

    def _new_id(self) -> int:
        id = self._next_id
        self._next_id += 1
        return id

    def _make_node(self, name: str, node_type: NodeType,
                   inputs: list[tuple],
                   outputs: list[tuple],
                   pos: tuple[float, float] = (0, 0)) -> Node:
        node_id = self._new_id()
        # Tuples can be (name, kind) or (name, kind, channels)
        pins_in = [Pin(self._new_id(), t[0], t[1], True, t[2] if len(t) > 2 else 2)
                   for t in inputs]
        pins_out = [Pin(self._new_id(), t[0], t[1], False, t[2] if len(t) > 2 else 2)
                    for t in outputs]
        has_params = node_type in (NodeType.EFFECT, NodeType.INSTRUMENT)
        params = _default_params(name, node_type) if has_params else []
        node = Node(node_id, name, node_type, pins_in, pins_out, pos, params=params)
        self.nodes.append(node)
        return node

    def _link(self, out_pin: int, in_pin: int):
        self.links.append(Link(self._new_id(), out_pin, in_pin))

    def _setup_default_graph(self):
        # Column x positions
        col_timeline = 50
        col_instrument = 450
        col_effect = 750
        col_mixer = 1100
        col_output = 1350
        spacing = 110

        # MIDI timelines (need instruments to produce audio)
        piano_tl = self._make_node("Piano", NodeType.MIDI_TIMELINE,
                                   [], [("MIDI", PinKind.MIDI)],
                                   (col_timeline, 50))
        def N(offset, pitch, dur):
            """Shorthand to create a MidiNote with auto-analyzed degree."""
            return MidiNote(offset, pitch, dur)

        piano_tl.clips = [
            Clip("Intro", 0, 4, (80, 120, 200),
                 notes=[N(0, 60, 1), N(1, 64, 1), N(2, 67, 1), N(3, 72, 0.5)]),
            Clip("Verse", 4, 8, (100, 140, 220),
                 notes=[N(0, 60, 0.5), N(0.5, 62, 0.5), N(1, 64, 1), N(2, 60, 2),
                        N(4, 67, 1), N(5, 65, 1), N(6, 64, 2)]),
        ]

        drums_tl = self._make_node("Drums", NodeType.MIDI_TIMELINE,
                                   [], [("MIDI", PinKind.MIDI)],
                                   (col_timeline, 50 + spacing * 2))
        drums_tl.clips = [
            Clip("Beat A", 0, 4, (180, 100, 80),
                 notes=[N(0, 36, 0.25), N(1, 38, 0.25), N(2, 36, 0.25), N(3, 38, 0.25)]),
            Clip("Beat A", 4, 4, (180, 100, 80),
                 notes=[N(0, 36, 0.25), N(1, 38, 0.25), N(2, 36, 0.25), N(3, 38, 0.25)]),
            Clip("Fill", 8, 2, (200, 120, 80),
                 notes=[N(0, 38, 0.25), N(0.5, 38, 0.25), N(1, 45, 0.25), N(1.5, 45, 0.25)]),
            Clip("Beat B", 10, 6, (180, 100, 80),
                 notes=[N(0, 36, 0.25), N(1, 38, 0.25), N(2, 36, 0.25), N(2.5, 36, 0.25),
                        N(3, 38, 0.25), N(4, 36, 0.25), N(5, 38, 0.5)]),
        ]

        # Audio timeline (outputs audio directly)
        vocals_tl = self._make_node("Vocals", NodeType.AUDIO_TIMELINE,
                                    [], [("Audio", PinKind.AUDIO, 1)],
                                    (col_timeline, 50 + spacing * 4))
        vocals_tl.clips = [
            Clip("Vocal Take 1", 2, 6, (120, 180, 100), channels=1),
            Clip("Vocal Take 2", 10, 4, (130, 190, 110), channels=1),
        ]

        guitar_tl = self._make_node("Guitar", NodeType.AUDIO_TIMELINE,
                                    [], [("Audio", PinKind.AUDIO)],
                                    (col_timeline, 50 + spacing * 6))
        guitar_tl.clips = [
            Clip("Guitar - Verse", 0, 8, (200, 160, 80)),
            Clip("Guitar - Chorus", 8, 8, (220, 180, 90)),
        ]

        # Instruments (MIDI in → Audio out)
        piano_inst = self._make_node("Piano", NodeType.INSTRUMENT,
                                     [("MIDI", PinKind.MIDI)],
                                     [("Audio", PinKind.AUDIO)],
                                     (col_instrument, 0))
        drums_inst = self._make_node("Drum Machine", NodeType.INSTRUMENT,
                                     [("MIDI", PinKind.MIDI)],
                                     [("Audio", PinKind.AUDIO)],
                                     (col_instrument, 300))

        # Effects
        reverb = self._make_node("Reverb", NodeType.EFFECT,
                                 [("In", PinKind.AUDIO)],
                                 [("Out", PinKind.AUDIO)],
                                 (col_effect, 0))
        comp = self._make_node("Compressor", NodeType.EFFECT,
                               [("In", PinKind.AUDIO)],
                               [("Out", PinKind.AUDIO)],
                               (col_effect, 300))
        eq = self._make_node("EQ", NodeType.EFFECT,
                             [("In", PinKind.AUDIO)],
                             [("Out", PinKind.AUDIO)],
                             (col_effect, 650))

        # Mixer
        mixer = self._make_node("Mixer", NodeType.MIXER,
                                [("In 1", PinKind.AUDIO),
                                 ("In 2", PinKind.AUDIO),
                                 ("In 3", PinKind.AUDIO),
                                 ("In 4", PinKind.AUDIO)],
                                [("Out", PinKind.AUDIO)],
                                (col_mixer, 300))

        output = self._make_node("Master Out", NodeType.OUTPUT,
                                 [("In", PinKind.AUDIO)], [],
                                 (col_output, 300))

        # MIDI timeline → instrument → effect → mixer
        self._link(piano_tl.pins_out[0].id, piano_inst.pins_in[0].id)
        self._link(drums_tl.pins_out[0].id, drums_inst.pins_in[0].id)
        self._link(piano_inst.pins_out[0].id, reverb.pins_in[0].id)
        self._link(drums_inst.pins_out[0].id, comp.pins_in[0].id)
        # Audio timeline → effect → mixer
        self._link(vocals_tl.pins_out[0].id, eq.pins_in[0].id)
        # Guitar straight to mixer
        self._link(guitar_tl.pins_out[0].id, mixer.pins_in[3].id)
        # Effects → mixer
        self._link(reverb.pins_out[0].id, mixer.pins_in[0].id)
        self._link(comp.pins_out[0].id, mixer.pins_in[1].id)
        self._link(eq.pins_out[0].id, mixer.pins_in[2].id)
        # Mixer → output
        self._link(mixer.pins_out[0].id, output.pins_in[0].id)

        # Analyze degrees for demo clips (C Major)
        c_major = self.KEYS['Major']
        for node in self.nodes:
            if node.node_type == NodeType.MIDI_TIMELINE:
                for clip in node.clips:
                    self._analyze_notes(clip, 0, c_major)

    def _pin_color(self, kind: PinKind) -> int:
        colors = {
            PinKind.AUDIO: imgui.IM_COL32(100, 180, 255, 255),
            PinKind.MIDI: imgui.IM_COL32(100, 255, 150, 255),
            PinKind.PARAM: imgui.IM_COL32(255, 200, 100, 255),
        }
        return colors.get(kind, imgui.IM_COL32(200, 200, 200, 255))

    def _node_color(self, node_type: NodeType) -> tuple:
        colors = {
            NodeType.AUDIO_TIMELINE: (80, 40, 120),
            NodeType.MIDI_TIMELINE: (40, 60, 120),
            NodeType.INSTRUMENT: (100, 60, 40),
            NodeType.EFFECT: (40, 80, 120),
            NodeType.MIXER: (80, 100, 40),
            NodeType.OUTPUT: (120, 50, 50),
            NodeType.SCRIPT: (60, 100, 80),
        }
        return colors.get(node_type, (80, 80, 80))

    def _draw_pin(self, pin: Pin):
        ch_label = CHANNEL_LABELS.get(pin.channels, f"{pin.channels}ch")
        if pin.kind == PinKind.AUDIO:
            label = f"{pin.name} ({ch_label})"
        else:
            label = pin.name

        if pin.is_input:
            ed.begin_pin(ed.PinId(pin.id), ed.PinKind.input)
            imgui.text_colored(imgui.ImVec4(0.6, 0.8, 1.0, 1.0), f"> {label}")
        else:
            ed.begin_pin(ed.PinId(pin.id), ed.PinKind.output)
            imgui.text_colored(imgui.ImVec4(0.6, 0.8, 1.0, 1.0), f"{label} >")
        ed.end_pin()

    def _get_timeline_beats(self, node: Node) -> float:
        """Auto-calculate timeline length from clips, rounded up to next bar."""
        if not node.clips:
            return 4.0
        end = max(c.start_beat + c.length_beats for c in node.clips)
        # Round up to next multiple of 4 (bar)
        import math
        return max(4.0, math.ceil(end / 4) * 4)

    def _draw_audio_waveform(self, draw_list, clip: Clip,
                             x1: float, x2: float, y: float, h: float):
        """Draw an audio waveform for a clip. Handles mono, stereo L/R, and mid/side."""
        cr, cg, cb = clip.color
        steps = max(int(x2 - x1), 1)
        seed = clip.start_beat * 7.3  # deterministic per-clip variation

        def _fake_wave(t, offset):
            """Generate a fake but plausible waveform value (-1 to 1)."""
            return (math.sin(t * 3.0 + offset) * 0.6
                    + math.sin(t * 7.1 + offset * 1.3) * 0.25
                    + math.sin(t * 13.3 + offset * 0.7) * 0.15) \
                   * (0.4 + 0.6 * abs(math.sin(t * 0.4 + offset * 0.5)))

        if clip.channels == 1:
            # Mono: single waveform, full height
            mid_y = y + h / 2
            for s in range(steps):
                t = (s / steps) * clip.length_beats * 4
                val = _fake_wave(t, seed)
                amp = h * 0.4
                draw_list.add_line(
                    imgui.ImVec2(x1 + s, mid_y - val * amp),
                    imgui.ImVec2(x1 + s, mid_y + val * amp),
                    imgui.IM_COL32(cr, cg, cb, 180)
                )

        elif clip.waveform_view == WaveformView.MID_SIDE:
            # Mid/Side: mid waveform solid, side as faint shadow
            mid_y = y + h / 2
            for s in range(steps):
                t = (s / steps) * clip.length_beats * 4
                l_val = _fake_wave(t, seed)
                r_val = _fake_wave(t, seed + 2.5)
                mid_val = (l_val + r_val) / 2
                side_val = (l_val - r_val) / 2
                amp = h * 0.4

                # Side (stereo width) — faint background
                draw_list.add_line(
                    imgui.ImVec2(x1 + s, mid_y - side_val * amp * 1.5),
                    imgui.ImVec2(x1 + s, mid_y + side_val * amp * 1.5),
                    imgui.IM_COL32(cr, cg, cb, 50)
                )
                # Mid (mono sum) — solid foreground
                draw_list.add_line(
                    imgui.ImVec2(x1 + s, mid_y - mid_val * amp),
                    imgui.ImVec2(x1 + s, mid_y + mid_val * amp),
                    imgui.IM_COL32(cr, cg, cb, 180)
                )

        else:
            # L/R stacked: top half = left, bottom half = right
            half = h / 2
            divider_y = y + half

            # Divider line
            draw_list.add_line(
                imgui.ImVec2(x1, divider_y),
                imgui.ImVec2(x2, divider_y),
                imgui.IM_COL32(255, 255, 255, 40)
            )

            for s in range(steps):
                t = (s / steps) * clip.length_beats * 4
                l_val = _fake_wave(t, seed)
                r_val = _fake_wave(t, seed + 2.5)
                amp = half * 0.7

                # Left channel (top half)
                l_mid = y + half / 2
                draw_list.add_line(
                    imgui.ImVec2(x1 + s, l_mid - l_val * amp),
                    imgui.ImVec2(x1 + s, l_mid + l_val * amp),
                    imgui.IM_COL32(cr, cg, cb, 160)
                )
                # Right channel (bottom half)
                r_mid = divider_y + half / 2
                draw_list.add_line(
                    imgui.ImVec2(x1 + s, r_mid - r_val * amp),
                    imgui.ImVec2(x1 + s, r_mid + r_val * amp),
                    imgui.IM_COL32(cr, cg, cb, 160)
                )

    def _draw_timeline_content(self, node: Node):
        """Draw the mini timeline view inside a timeline node."""
        tl_width = 300
        tl_height = 40 if node.node_type == NodeType.AUDIO_TIMELINE else 50
        total_beats = self._get_timeline_beats(node)

        # Reserve space and get draw position
        cursor = imgui.get_cursor_screen_pos()
        draw_list = imgui.get_window_draw_list()

        # Background
        draw_list.add_rect_filled(
            imgui.ImVec2(cursor.x, cursor.y),
            imgui.ImVec2(cursor.x + tl_width, cursor.y + tl_height),
            imgui.IM_COL32(20, 20, 30, 200), 3.0
        )

        # Beat grid lines
        for beat in range(int(total_beats) + 1):
            x = cursor.x + (beat / total_beats) * tl_width
            alpha = 80 if beat % 4 == 0 else 30
            draw_list.add_line(
                imgui.ImVec2(x, cursor.y),
                imgui.ImVec2(x, cursor.y + tl_height),
                imgui.IM_COL32(255, 255, 255, alpha)
            )

        # Draw clips
        for clip in node.clips:
            x1 = cursor.x + (clip.start_beat / total_beats) * tl_width
            x2 = cursor.x + ((clip.start_beat + clip.length_beats) / total_beats) * tl_width
            cr, cg, cb = clip.color

            if node.node_type == NodeType.MIDI_TIMELINE:
                # MIDI clip: draw note rectangles inside
                clip_y = cursor.y + 2
                clip_h = tl_height - 4

                # Clip background
                draw_list.add_rect_filled(
                    imgui.ImVec2(x1, clip_y),
                    imgui.ImVec2(x2, clip_y + clip_h),
                    imgui.IM_COL32(cr, cg, cb, 60), 2.0
                )
                draw_list.add_rect(
                    imgui.ImVec2(x1, clip_y),
                    imgui.ImVec2(x2, clip_y + clip_h),
                    imgui.IM_COL32(cr, cg, cb, 180), 2.0
                )

                # Draw individual notes
                if clip.notes:
                    pitches = [n.pitch for n in clip.notes]
                    min_p = min(pitches) - 2
                    max_p = max(pitches) + 2
                    pitch_range = max(max_p - min_p, 1)
                    beat_width = (x2 - x1) / clip.length_beats

                    for note in clip.notes:
                        nx1 = x1 + note.offset * beat_width
                        nx2 = x1 + (note.offset + note.duration) * beat_width
                        ny = clip_y + clip_h - ((note.pitch - min_p) / pitch_range) * clip_h
                        nh = max(clip_h / pitch_range, 2)
                        draw_list.add_rect_filled(
                            imgui.ImVec2(nx1, ny - nh / 2),
                            imgui.ImVec2(nx2, ny + nh / 2),
                            imgui.IM_COL32(cr, cg, cb, 220), 1.0
                        )
            else:
                # Audio clip: draw waveform
                clip_y = cursor.y + 2
                clip_h = tl_height - 4

                draw_list.add_rect_filled(
                    imgui.ImVec2(x1, clip_y),
                    imgui.ImVec2(x2, clip_y + clip_h),
                    imgui.IM_COL32(cr, cg, cb, 100), 2.0
                )
                draw_list.add_rect(
                    imgui.ImVec2(x1, clip_y),
                    imgui.ImVec2(x2, clip_y + clip_h),
                    imgui.IM_COL32(cr, cg, cb, 200), 2.0
                )

                self._draw_audio_waveform(draw_list, clip, x1, x2, clip_y, clip_h)

                draw_list.add_text(
                    imgui.ImVec2(x1 + 3, clip_y + 2),
                    imgui.IM_COL32(255, 255, 255, 200),
                    clip.name
                )

        # Advance cursor past the timeline area
        imgui.dummy(imgui.ImVec2(tl_width, tl_height))

    def _draw_node(self, node: Node):
        r, g, b = self._node_color(node.node_type)

        ed.push_style_color(ed.StyleColor.node_bg,
                           imgui.ImVec4(r/255, g/255, b/255, 0.8))
        ed.push_style_color(ed.StyleColor.node_border,
                           imgui.ImVec4(r/255 + 0.2, g/255 + 0.2, b/255 + 0.2, 1.0))
        ed.push_style_var(ed.StyleVar.node_padding, imgui.ImVec4(4, 2, 4, 2))

        ed.begin_node(ed.NodeId(node.id))

        if not node.pos_set:
            ed.set_node_position(ed.NodeId(node.id),
                                imgui.ImVec2(node.pos[0], node.pos[1]))
            node.pos_set = True

        # Title
        imgui.text(node.name)

        # Timeline content for timeline nodes
        if node.node_type in (NodeType.AUDIO_TIMELINE, NodeType.MIDI_TIMELINE):
            self._draw_timeline_content(node)

        # Pins — input and output side by side where possible
        max_pins = max(len(node.pins_in), len(node.pins_out))
        for i in range(max_pins):
            if i < len(node.pins_in):
                self._draw_pin(node.pins_in[i])
            if i < len(node.pins_out):
                if i < len(node.pins_in):
                    imgui.same_line(100)
                self._draw_pin(node.pins_out[i])

        # Parameters — for effects and instruments
        if node.node_type in (NodeType.EFFECT, NodeType.INSTRUMENT) and node.params:
            imgui.spacing()
            for j, param in enumerate(node.params):
                imgui.set_next_item_width(150)
                changed, value = imgui.slider_float(
                    f"{param.name}##{node.id}_{j}",
                    param.value, param.min_val, param.max_val, param.format
                )
                if changed:
                    param.value = value

        # Mixer gain sliders
        if node.node_type == NodeType.MIXER:
            imgui.spacing()
            if not node.params:
                node.params = [Param(f"Ch {i+1}", 0.8, 0.0, 1.0)
                               for i in range(len(node.pins_in))]
            for j, param in enumerate(node.params):
                imgui.set_next_item_width(100)
                changed, value = imgui.slider_float(
                    f"{param.name}##{node.id}_{j}",
                    param.value, param.min_val, param.max_val
                )
                if changed:
                    param.value = value

        # Script node content
        if node.node_type == NodeType.SCRIPT:
            imgui.spacing()
            imgui.set_next_item_width(200)
            changed, new_text = imgui.input_text_multiline(
                f"##script_{node.id}",
                node.script,
                imgui.ImVec2(200, 60)
            )
            if changed:
                node.script = new_text
            imgui.text_colored(imgui.ImVec4(0.5, 0.5, 0.5, 1.0), "# Python scripting (TODO)")

        ed.end_node()
        ed.pop_style_var()
        ed.pop_style_color(2)

    def gui(self):
        # Transport bar
        imgui.button("Play")
        imgui.same_line()
        imgui.button("Stop")
        imgui.same_line()
        imgui.button("Play & Record")
        imgui.same_line()
        imgui.text(" | ")
        imgui.same_line()
        imgui.text("BPM:")
        imgui.same_line()
        imgui.set_next_item_width(80)
        changed, value = imgui.drag_float("##bpm", self.bpm, 1.0, 20.0, 999.0, "%.0f")
        if changed:
            self.bpm = value
        imgui.same_line()
        imgui.text(" | ")
        imgui.same_line()
        if imgui.button("+ MIDI Track"):
            node = self._make_node("MIDI Track", NodeType.MIDI_TIMELINE,
                                   [], [("MIDI", PinKind.MIDI)], (50, 50))
            node.clips = [Clip("Clip 1", 0, 4, (80, 120, 200))]
        imgui.same_line()
        if imgui.button("+ Audio Track"):
            node = self._make_node("Audio Track", NodeType.AUDIO_TIMELINE,
                                   [], [("Audio", PinKind.AUDIO)], (50, 50))
            node.clips = [Clip("Clip 1", 0, 4, (120, 180, 100))]
        imgui.same_line()
        imgui.text(" | ")
        imgui.same_line()
        imgui.button("Fit All")  # TODO: wire up in production version
        imgui.same_line()
        if imgui.button("Script Console"):
            self.show_script_console = not self.show_script_console
        imgui.same_line()
        imgui.text_colored(imgui.ImVec4(0.5, 0.5, 0.5, 1.0), "Scroll=zoom  Middle-drag=pan")

        imgui.separator()

        # Split: node editor on top, timeline editor on bottom
        avail = imgui.get_content_region_avail()
        editor_height = avail.y
        if self.open_editors:
            editor_height = avail.y - self.editor_panel_height - 8
            editor_height = max(editor_height, 100)

        imgui.begin_child("node_editor_area", imgui.ImVec2(0, editor_height))
        ed.begin("SoundShop Node Editor")

        for node in self.nodes:
            self._draw_node(node)

        for link in self.links:
            # Find source pin for color and thickness
            color = imgui.ImVec4(0.4, 0.7, 1.0, 1.0)  # default: audio (blue)
            thickness = 2.0
            for node in self.nodes:
                for pin in node.pins_out:
                    if pin.id == link.start_pin:
                        if pin.kind == PinKind.MIDI:
                            color = imgui.ImVec4(0.4, 1.0, 0.6, 1.0)
                            thickness = 1.5
                        else:
                            thickness = 1.0 + pin.channels * 0.75
            ed.link(ed.LinkId(link.id),
                   ed.PinId(link.start_pin),
                   ed.PinId(link.end_pin),
                   color, thickness)

        # Handle new links
        if ed.begin_create():
            start_pin_id = ed.PinId()
            end_pin_id = ed.PinId()
            if ed.query_new_link(start_pin_id, end_pin_id):
                if ed.accept_new_item():
                    self.links.append(Link(
                        self._new_id(),
                        start_pin_id.id(),
                        end_pin_id.id()
                    ))
            ed.end_create()

        # Handle deleted links
        if ed.begin_delete():
            link_id = ed.LinkId()
            while ed.query_deleted_link(link_id):
                if ed.accept_deleted_item():
                    self.links = [l for l in self.links if l.id != link_id.id()]
            node_id = ed.NodeId()
            while ed.query_deleted_node(node_id):
                if ed.accept_deleted_item():
                    # Remove links connected to this node's pins
                    node = next((n for n in self.nodes if n.id == node_id.id()), None)
                    if node:
                        pin_ids = {p.id for p in node.pins_in + node.pins_out}
                        self.links = [l for l in self.links
                                     if l.start_pin not in pin_ids
                                     and l.end_pin not in pin_ids]
                        self.nodes.remove(node)
            ed.end_delete()

        # Right-click context menu
        ed.suspend()
        if ed.show_background_context_menu():
            imgui.open_popup("add_node")

        if imgui.begin_popup("add_node"):
            mouse_pos = imgui.get_mouse_pos_on_opening_current_popup()
            if imgui.menu_item_simple("MIDI Timeline"):
                self._make_node("MIDI Track", NodeType.MIDI_TIMELINE,
                               [], [("MIDI", PinKind.MIDI)],
                               (mouse_pos.x, mouse_pos.y))
            if imgui.menu_item_simple("Audio Timeline"):
                self._make_node("Audio Track", NodeType.AUDIO_TIMELINE,
                               [], [("Audio", PinKind.AUDIO)],
                               (mouse_pos.x, mouse_pos.y))
            imgui.separator()
            if imgui.begin_menu("Instruments"):
                for inst_name in INSTRUMENT_PARAMS:
                    if imgui.menu_item_simple(inst_name):
                        self._make_node(inst_name, NodeType.INSTRUMENT,
                                       [("MIDI", PinKind.MIDI)],
                                       [("Audio", PinKind.AUDIO)],
                                       (mouse_pos.x, mouse_pos.y))
                imgui.end_menu()
            if imgui.begin_menu("Effects"):
                for effect_name in EFFECT_PARAMS:
                    if imgui.menu_item_simple(effect_name):
                        self._make_node(effect_name, NodeType.EFFECT,
                                       [("In", PinKind.AUDIO)],
                                       [("Out", PinKind.AUDIO)],
                                       (mouse_pos.x, mouse_pos.y))
                imgui.end_menu()
            imgui.separator()
            if imgui.menu_item_simple("Mixer"):
                self._make_node("Mixer", NodeType.MIXER,
                               [("In 1", PinKind.AUDIO),
                                ("In 2", PinKind.AUDIO)],
                               [("Out", PinKind.AUDIO)],
                               (mouse_pos.x, mouse_pos.y))
            if imgui.menu_item_simple("Output"):
                self._make_node("Output", NodeType.OUTPUT,
                               [("In", PinKind.AUDIO)], [],
                               (mouse_pos.x, mouse_pos.y))
            imgui.separator()
            if imgui.begin_menu("Script"):
                if imgui.menu_item_simple("Audio Script"):
                    n = self._make_node("Script", NodeType.SCRIPT,
                                   [("In", PinKind.AUDIO)],
                                   [("Out", PinKind.AUDIO)],
                                   (mouse_pos.x, mouse_pos.y))
                    n.script = "# Process audio\n# input → output\n"
                if imgui.menu_item_simple("MIDI Script"):
                    n = self._make_node("MIDI Script", NodeType.SCRIPT,
                                   [("In", PinKind.MIDI)],
                                   [("Out", PinKind.MIDI)],
                                   (mouse_pos.x, mouse_pos.y))
                    n.script = "# Process MIDI\n# input → output\n"
                if imgui.menu_item_simple("Generator Script"):
                    n = self._make_node("Generator", NodeType.SCRIPT,
                                   [],
                                   [("Out", PinKind.AUDIO)],
                                   (mouse_pos.x, mouse_pos.y))
                    n.script = "# Generate audio\n# output signal\n"
                if imgui.menu_item_simple("MIDI Generator Script"):
                    n = self._make_node("MIDI Generator", NodeType.SCRIPT,
                                   [],
                                   [("Out", PinKind.MIDI)],
                                   (mouse_pos.x, mouse_pos.y))
                    n.script = "# Generate MIDI\n# output events\n"
                imgui.end_menu()
            imgui.end_popup()
        ed.resume()

        # Detect double-clicked timeline node → open editor
        dbl_node_id = ed.get_double_clicked_node()
        if dbl_node_id.id() != 0:
            node = next((n for n in self.nodes
                        if n.id == dbl_node_id.id()
                        and n.node_type in (NodeType.AUDIO_TIMELINE, NodeType.MIDI_TIMELINE)),
                       None)
            if node and node not in self.open_editors:
                self.open_editors.insert(0, node)

        ed.end()
        imgui.end_child()

        # Splitter drag handle
        if self.open_editors:
            self._draw_splitter()

        # Bottom panel: stacked timeline editors
        if self.open_editors:
            self._draw_editor_panel()

        # Script console window
        if self.show_script_console:
            self._draw_script_console()

    def _draw_script_console(self):
        """Draw the script console as a floating window."""
        imgui.set_next_window_size(imgui.ImVec2(500, 350), imgui.Cond_.first_use_ever.value)
        expanded, opened = imgui.begin("Script Console", True)
        if not opened:
            self.show_script_console = False
            imgui.end()
            return

        # Language selector
        imgui.text("Language:")
        imgui.same_line()
        imgui.set_next_item_width(100)
        imgui.combo("##lang", 0, ["Python", "Lua", "JS"])  # TODO: implement
        imgui.same_line()
        if imgui.button("Run"):
            self.script_console_output = "# Script execution not implemented in prototype\n# This will run in the C++ version"
        imgui.same_line()
        if imgui.button("Clear Output"):
            self.script_console_output = ""
        imgui.same_line()
        imgui.text_colored(imgui.ImVec4(0.5, 0.5, 0.5, 1.0), "(TODO: actual execution)")

        imgui.separator()

        # Script editor
        avail = imgui.get_content_region_avail()
        editor_h = avail.y * 0.6
        output_h = avail.y - editor_h - 25

        imgui.text("Script:")
        changed, new_text = imgui.input_text_multiline(
            "##console_script",
            self.script_console_text,
            imgui.ImVec2(-1, editor_h)
        )
        if changed:
            self.script_console_text = new_text

        imgui.separator()
        imgui.text("Output:")
        imgui.input_text_multiline(
            "##console_output",
            self.script_console_output,
            imgui.ImVec2(-1, output_h),
            imgui.InputTextFlags_.read_only.value
        )

        imgui.end()

    def _draw_splitter(self):
        """Draw a draggable horizontal splitter bar."""
        cursor = imgui.get_cursor_screen_pos()
        width = imgui.get_content_region_avail().x
        splitter_h = 6

        # Invisible button for drag interaction
        imgui.invisible_button("##splitter", imgui.ImVec2(width, splitter_h))
        is_hovered = imgui.is_item_hovered()
        is_active = imgui.is_item_active()

        if is_hovered or is_active:
            imgui.set_mouse_cursor(imgui.MouseCursor_.resize_ns)

        if is_active:
            delta = imgui.get_io().mouse_delta.y
            self.editor_panel_height = max(100, self.editor_panel_height - delta)

        # Draw the bar
        color = imgui.IM_COL32(120, 120, 120, 255) if (is_hovered or is_active) \
            else imgui.IM_COL32(60, 60, 60, 255)
        draw_list = imgui.get_window_draw_list()
        draw_list.add_rect_filled(
            imgui.ImVec2(cursor.x, cursor.y),
            imgui.ImVec2(cursor.x + width, cursor.y + splitter_h),
            color, 2.0
        )

    def _draw_editor_panel(self):
        """Draw the stacked timeline editors panel."""
        avail = imgui.get_content_region_avail()
        panel_height = max(avail.y, 100)
        num = len(self.open_editors)
        per_editor = max((panel_height / num) - 4, 60) if num else panel_height

        if imgui.begin_child("editor_panel", imgui.ImVec2(0, panel_height),
                             window_flags=imgui.WindowFlags_.no_scrollbar.value):
            to_close = []

            for idx, node in enumerate(self.open_editors):
                self._draw_single_editor(node, idx, per_editor, to_close)

            for node in to_close:
                self.open_editors.remove(node)

        imgui.end_child()

    def _draw_single_editor(self, node: Node, idx: int, height: float, to_close: list):
        """Draw one timeline editor."""
        is_midi = node.node_type == NodeType.MIDI_TIMELINE
        kind_label = "MIDI" if is_midi else "Audio"
        kind_color = imgui.ImVec4(0.4, 1.0, 0.6, 1.0) if is_midi \
            else imgui.ImVec4(0.4, 0.7, 1.0, 1.0)

        if imgui.begin_child(f"editor_{node.id}", imgui.ImVec2(0, height),
                             child_flags=imgui.ChildFlags_.borders.value,
                             window_flags=imgui.WindowFlags_.no_scrollbar.value):
            # Header line with X button on the right
            imgui.text(node.name)
            imgui.same_line()
            imgui.text_colored(kind_color, f"[{kind_label}]")

            # Waveform view toggle for audio timelines
            if not is_midi and node.clips:
                imgui.same_line()
                current_view = node.clips[0].waveform_view
                if current_view == WaveformView.LR:
                    label = "View: Left/Right"
                    tooltip = "Showing left and right channels stacked.\nClick to switch to Mid/Side view (center vs stereo width)."
                else:
                    label = "View: Mid/Side"
                    tooltip = "Showing center (mid) as solid, stereo spread (side) as faint.\nClick to switch to Left/Right view."
                if imgui.small_button(f"{label}##{node.id}"):
                    new_view = WaveformView.MID_SIDE if current_view == WaveformView.LR \
                        else WaveformView.LR
                    for clip in node.clips:
                        clip.waveform_view = new_view
                if imgui.is_item_hovered():
                    imgui.set_tooltip(tooltip)

            # X button on far right
            imgui.same_line(imgui.get_content_region_avail().x - 20)
            if imgui.small_button(f"X##{node.id}"):
                to_close.append(node)
                imgui.end_child()
                return

            # Draw the appropriate editor
            if is_midi:
                toolbar_h = self._draw_midi_toolbar(node)
                self._draw_piano_roll(node, height - 40 - toolbar_h)
            else:
                self._draw_audio_editor(node, height - 40)

        imgui.end_child()


    NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']

    # Keys: major, minor, and minor variants
    KEYS = {
        'Major': [0, 2, 4, 5, 7, 9, 11],
        'Natural Minor': [0, 2, 3, 5, 7, 8, 10],
        'Harmonic Minor': [0, 2, 3, 5, 7, 8, 11],
        'Harmonic Major': [0, 2, 4, 5, 7, 8, 11],
        'Melodic Minor': [0, 2, 3, 5, 7, 9, 11],
        'Neapolitan Major': [0, 1, 3, 5, 7, 9, 11],
        'Neapolitan Minor': [0, 1, 3, 5, 7, 8, 11],
        'Double Harmonic': [0, 1, 4, 5, 7, 8, 11],
        'Hungarian Minor': [0, 2, 3, 6, 7, 8, 11],
        'Hungarian Major': [0, 3, 4, 6, 7, 9, 10],
    }

    # Modes: rotations of the major scale + others
    MODES = {
        'Ionian': [0, 2, 4, 5, 7, 9, 11],
        'Dorian': [0, 2, 3, 5, 7, 9, 10],
        'Phrygian': [0, 1, 3, 5, 7, 8, 10],
        'Lydian': [0, 2, 4, 6, 7, 9, 11],
        'Lydian Augmented': [0, 2, 4, 6, 8, 9, 11],
        'Mixolydian': [0, 2, 4, 5, 7, 9, 10],
        'Aeolian': [0, 2, 3, 5, 7, 8, 10],
        'Locrian': [0, 1, 3, 5, 6, 8, 10],
        'Locrian Major': [0, 2, 4, 5, 6, 8, 10],
        'Super Locrian': [0, 1, 3, 4, 6, 8, 10],
        'Phrygian Dominant': [0, 1, 4, 5, 7, 8, 10],
        'Acoustic': [0, 2, 4, 6, 7, 9, 10],
        'Ukrainian Dorian': [0, 2, 3, 6, 7, 9, 10],
    }

    # Scales: other systems
    SCALES = {
        'Major Pentatonic': [0, 2, 4, 7, 9],
        'Minor Pentatonic': [0, 3, 5, 7, 10],
        'Blues': [0, 3, 5, 6, 7, 10],
        'Chromatic': list(range(12)),
        'Whole Tone': [0, 2, 4, 6, 8, 10],
        'Augmented': [0, 3, 4, 7, 8, 11],
        'Bebop Dominant': [0, 2, 4, 5, 7, 9, 10, 11],
        'Octatonic (W-H)': [0, 2, 3, 5, 6, 8, 9, 11],
        'Octatonic (H-W)': [0, 1, 3, 4, 6, 7, 9, 10],
        'Prometheus': [0, 2, 4, 6, 9, 10],
        'Tritone': [0, 1, 4, 6, 7, 10],
        'Two-Semitone Tritone': [0, 1, 2, 6, 7, 8],
        'Enigmatic': [0, 1, 4, 6, 8, 10, 11],
        'Persian': [0, 1, 4, 5, 6, 8, 11],
        'Algerian': [0, 2, 3, 6, 7, 8, 11],
        'Flamenco': [0, 1, 4, 5, 7, 8, 11],
        'Romani': [0, 2, 3, 6, 7, 8, 10],
        'Half-Diminished': [0, 2, 3, 5, 6, 8, 10],
        'Harmonics': [0, 3, 4, 5, 7, 9],
        'Hirajoshi': [0, 4, 6, 7, 11],
        'In': [0, 1, 5, 7, 8],
        'Insen': [0, 1, 5, 7, 10],
        'Iwato': [0, 1, 5, 6, 10],
        'Yo': [0, 3, 5, 7, 10],
    }

    def _get_active_intervals(self, state: dict) -> list[int]:
        """Get the interval pattern based on the active category."""
        cat = state.get('active_category', 'key')
        name = state.get('active_name', 'Major')
        lookup = {'key': self.KEYS, 'mode': self.MODES, 'scale': self.SCALES}
        table = lookup.get(cat, self.KEYS)
        return table.get(name, [0, 2, 4, 5, 7, 9, 11])

    def _note_name(self, pitch: int) -> str:
        return f"{self.NOTE_NAMES[pitch % 12]}{pitch // 12 - 1}"

    def _is_black_key(self, pitch: int) -> bool:
        return (pitch % 12) in (1, 3, 6, 8, 10)

    def _get_piano_state(self, node_id: int) -> dict:
        if node_id not in self._piano_roll_state:
            self._piano_roll_state[node_id] = {
                'scroll_pitch': 60,  # middle C centered
                'visible_range': 24,  # 2 octaves visible
                'snap': 0.25,  # snap to 16th notes
                'dragging': None,  # (clip_idx, note_idx, mode)
                'drag_start': None,
                'selected': set(),  # set of (clip_idx, note_idx)
                'selecting_box': None,  # (start_beat, start_pitch, end_beat, end_pitch)
                'key_root': 0,  # 0=C, 1=C#, etc
                'active_category': 'key',  # 'key', 'mode', or 'scale'
                'active_name': 'Major',
            }
        return self._piano_roll_state[node_id]

    def _get_all_selected_notes(self, node: Node, state: dict) -> list[tuple[int, int]]:
        """Return list of (clip_idx, note_idx) for selected notes that still exist."""
        valid = []
        for ci, ni in state['selected']:
            if ci < len(node.clips) and ni < len(node.clips[ci].notes):
                valid.append((ci, ni))
        return valid

    def _apply_to_selected(self, node: Node, state: dict, fn):
        """Apply a function to all selected notes. fn(note) -> modified note (in place)."""
        for ci, ni in sorted(state['selected'], reverse=True):
            if ci < len(node.clips) and ni < len(node.clips[ci].notes):
                fn(node.clips[ci].notes[ni])

    def _snap_to_scale(self, pitch: int, root: int, scale: list[int]) -> int:
        """Snap a pitch to the nearest note in the given scale."""
        note = pitch % 12
        rel = (note - root) % 12
        best = min(scale, key=lambda s: min(abs(rel - s), 12 - abs(rel - s)))
        return pitch - rel + best if (rel - best) % 12 <= (best - rel) % 12 \
            else pitch - rel + best

    def _pitch_to_degree(self, pitch: int, root: int, scale: list[int]) -> tuple[int, int, int]:
        """Convert a MIDI pitch to (degree, octave, chromatic_offset).
        degree is 0-based index into the scale.
        chromatic_offset is semitones from the nearest scale tone."""
        note = (pitch - root) % 12
        oct = (pitch - root) // 12

        # Find nearest scale degree
        best_deg = 0
        best_dist = 999
        for i, s in enumerate(scale):
            dist = note - s
            if abs(dist) < abs(best_dist):
                best_dist = dist
                best_deg = i
            # Also check wrapping
            dist_wrap = note - (s + 12)
            if abs(dist_wrap) < abs(best_dist):
                best_dist = dist_wrap
                best_deg = i

        return best_deg, oct, best_dist

    def _degree_to_pitch(self, degree: int, octave: int, chromatic_offset: int,
                          root: int, scale: list[int]) -> int:
        """Convert (degree, octave, chromatic_offset) back to MIDI pitch."""
        degree_in_scale = degree % len(scale)
        extra_octaves = degree // len(scale)
        semitone = scale[degree_in_scale]
        return root + (octave + extra_octaves) * 12 + semitone + chromatic_offset

    def _analyze_notes(self, clip: Clip, root: int, scale: list[int]):
        """Assign scale degrees to all notes in a clip based on key/scale."""
        for note in clip.notes:
            deg, oct, chrom = self._pitch_to_degree(note.pitch, root, scale)
            note.degree = deg
            note.octave = oct
            note.chromatic_offset = chrom

    def _rekey_notes(self, clip: Clip, new_root: int, new_scale: list[int]):
        """Recompute pitches from stored degrees using a new key/scale."""
        for note in clip.notes:
            note.pitch = self._degree_to_pitch(
                note.degree, note.octave, note.chromatic_offset,
                new_root, new_scale
            )
            note.pitch = max(0, min(127, note.pitch))

    def _draw_midi_toolbar(self, node: Node) -> float:
        """Draw the MIDI editor toolbar. Returns height used."""
        state = self._get_piano_state(node.id)
        num_sel = len(self._get_all_selected_notes(node, state))
        start_y = imgui.get_cursor_pos().y

        # Row 1: Selection info + transpose
        if num_sel > 0:
            imgui.text(f"{num_sel} selected")
        else:
            imgui.text_colored(imgui.ImVec4(0.5, 0.5, 0.5, 1.0),
                "Click=place  Drag=select  Drag note=move  Edges=resize  Alt=no snap  Right-click=delete")
        imgui.same_line(200)

        # Transpose buttons
        if imgui.small_button(f"-Oct##{node.id}"):
            self._apply_to_selected(node, state,
                lambda n: setattr(n, 'pitch', max(0, n.pitch - 12)))
        if imgui.is_item_hovered():
            imgui.set_tooltip("Transpose down 1 octave")
        imgui.same_line()
        if imgui.small_button(f"-Semi##{node.id}"):
            self._apply_to_selected(node, state,
                lambda n: setattr(n, 'pitch', max(0, n.pitch - 1)))
        if imgui.is_item_hovered():
            imgui.set_tooltip("Transpose down 1 semitone")
        imgui.same_line()
        if imgui.small_button(f"+Semi##{node.id}"):
            self._apply_to_selected(node, state,
                lambda n: setattr(n, 'pitch', min(127, n.pitch + 1)))
        if imgui.is_item_hovered():
            imgui.set_tooltip("Transpose up 1 semitone")
        imgui.same_line()
        if imgui.small_button(f"+Oct##{node.id}"):
            self._apply_to_selected(node, state,
                lambda n: setattr(n, 'pitch', min(127, n.pitch + 12)))
        if imgui.is_item_hovered():
            imgui.set_tooltip("Transpose up 1 octave")

        # Time shift
        imgui.same_line()
        imgui.text(" | ")
        imgui.same_line()
        snap = state['snap']
        if imgui.small_button(f"<< Time##{node.id}"):
            self._apply_to_selected(node, state,
                lambda n: setattr(n, 'offset', max(0, n.offset - snap)))
        if imgui.is_item_hovered():
            imgui.set_tooltip(f"Shift left by {snap} beat")
        imgui.same_line()
        if imgui.small_button(f"Time >>##{node.id}"):
            self._apply_to_selected(node, state,
                lambda n: setattr(n, 'offset', n.offset + snap))
        if imgui.is_item_hovered():
            imgui.set_tooltip(f"Shift right by {snap} beat")

        # Select all / deselect
        imgui.same_line()
        imgui.text(" | ")
        imgui.same_line()
        if imgui.small_button(f"Select All##{node.id}"):
            state['selected'] = set()
            for ci, clip in enumerate(node.clips):
                for ni in range(len(clip.notes)):
                    state['selected'].add((ci, ni))
        imgui.same_line()
        if imgui.small_button(f"Deselect##{node.id}"):
            state['selected'] = set()

        # Row 2: Root, Key/Mode/Scale dropdowns
        # Root note
        imgui.text("Root:")
        imgui.same_line()
        imgui.set_next_item_width(50)
        changed, new_idx = imgui.combo(
            f"##root_{node.id}",
            state['key_root'],
            self.NOTE_NAMES
        )
        if changed:
            state['key_root'] = new_idx
        imgui.same_line()

        active_cat = state['active_category']
        active_name = state['active_name']

        # Determine which dropdowns should be dimmed
        # Scale is never dimmed. Key/Mode are dimmed only when a non-Chromatic scale is active.
        scale_overrides = active_cat == 'scale' and active_name != 'Chromatic'

        def _cat_dropdown(label, cat_name, names_dict, width, nid, dimmed=False):
            names = list(names_dict.keys())
            is_active = active_cat == cat_name
            cur_idx = names.index(active_name) if is_active and active_name in names else -1

            if dimmed and not is_active:
                imgui.text_colored(imgui.ImVec4(0.4, 0.4, 0.4, 1.0), f"{label}:")
            else:
                imgui.text(f"{label}:")
            imgui.same_line()

            if is_active:
                imgui.push_style_color(imgui.Col_.frame_bg.value, imgui.ImVec4(0.2, 0.35, 0.5, 1.0))
                n_colors = 1
            elif dimmed:
                imgui.push_style_color(imgui.Col_.frame_bg.value, imgui.ImVec4(0.15, 0.15, 0.15, 1.0))
                imgui.push_style_color(imgui.Col_.text.value, imgui.ImVec4(0.4, 0.4, 0.4, 1.0))
                n_colors = 2
            else:
                n_colors = 0

            imgui.set_next_item_width(width)
            changed, new_idx = imgui.combo(f"##{cat_name}_{nid}", max(cur_idx, 0), names)
            if changed:
                state['active_category'] = cat_name
                state['active_name'] = names[new_idx]

            if n_colors:
                imgui.pop_style_color(n_colors)

        prev_cat = active_cat
        prev_name = active_name
        prev_root = state['key_root']

        _cat_dropdown("Key", "key", self.KEYS, 130, node.id, dimmed=scale_overrides)
        imgui.same_line()
        _cat_dropdown("Mode", "mode", self.MODES, 130, node.id, dimmed=scale_overrides)
        imgui.same_line()
        _cat_dropdown("Scale", "scale", self.SCALES, 140, node.id, dimmed=False)
        imgui.same_line()

        # Auto-analyze when root, category, or name changes
        if (state['key_root'] != prev_root
                or state['active_category'] != prev_cat
                or state['active_name'] != prev_name):
            root = state['key_root']
            intervals = self._get_active_intervals(state)
            for clip in node.clips:
                self._analyze_notes(clip, root, intervals)

        # Snap to scale button
        if imgui.small_button(f"Snap to Scale##{node.id}"):
            root = state['key_root']
            scale = self._get_active_intervals(state)
            self._apply_to_selected(node, state,
                lambda n: setattr(n, 'pitch', self._snap_to_scale(n.pitch, root, scale)))
        if imgui.is_item_hovered():
            imgui.set_tooltip("Move selected notes to the nearest note in the chosen scale")

        # Change Key (recompute pitches from degrees)
        imgui.same_line()
        if imgui.small_button(f"Change Key##{node.id}"):
            root = state['key_root']
            scale = self._get_active_intervals(state)
            for clip in node.clips:
                self._rekey_notes(clip, root, scale)
        if imgui.is_item_hovered():
            imgui.set_tooltip("Recompute all pitches from their stored scale degrees\n"
                              "using the current key/scale.\n\n"
                              "Workflow: Analyze in original key, change key/scale, then Change Key.")

        # Snap grid
        imgui.same_line()
        imgui.text(" | Snap:")
        imgui.same_line()
        snap_options = [("1/4", 0.25), ("1/2", 0.5), ("1", 1.0), ("Off", 0.0)]
        for label, val in snap_options:
            is_current = abs(state['snap'] - val) < 0.01
            if is_current:
                imgui.push_style_color(imgui.Col_.button.value,
                                       imgui.ImVec4(0.3, 0.5, 0.7, 1.0))
            if imgui.small_button(f"{label}##{node.id}_snap"):
                state['snap'] = val
            if is_current:
                imgui.pop_style_color()
            imgui.same_line()

        # Double/halve duration
        imgui.text(" | ")
        imgui.same_line()
        if imgui.small_button(f"x2 Dur##{node.id}"):
            self._apply_to_selected(node, state,
                lambda n: setattr(n, 'duration', n.duration * 2))
        if imgui.is_item_hovered():
            imgui.set_tooltip("Double the duration of selected notes")
        imgui.same_line()
        if imgui.small_button(f"/2 Dur##{node.id}"):
            self._apply_to_selected(node, state,
                lambda n: setattr(n, 'duration', max(0.125, n.duration / 2)))
        if imgui.is_item_hovered():
            imgui.set_tooltip("Halve the duration of selected notes")

        # Reverse selected
        imgui.same_line()
        if imgui.small_button(f"Reverse##{node.id}"):
            sel = self._get_all_selected_notes(node, state)
            if sel:
                notes_data = [(ci, ni, node.clips[ci].notes[ni]) for ci, ni in sel]
                offsets = [node.clips[ci].start_beat + n.offset for ci, ni, n in notes_data]
                min_off = min(offsets)
                max_off = max(o + n.duration for o, (_, _, n) in zip(offsets, notes_data))
                for (ci, ni, note), abs_off in zip(notes_data, offsets):
                    new_abs = max_off - (abs_off - min_off) - note.duration
                    note.offset = max(0, new_abs - node.clips[ci].start_beat)
        if imgui.is_item_hovered():
            imgui.set_tooltip("Reverse the time positions of selected notes")

        # Detune controls
        imgui.same_line()
        imgui.text(" | Detune:")
        imgui.same_line()
        imgui.set_next_item_width(80)
        # Show average detune of selected notes
        sel_notes = self._get_all_selected_notes(node, state)
        avg_detune = 0.0
        if sel_notes:
            avg_detune = sum(node.clips[ci].notes[ni].detune for ci, ni in sel_notes) / len(sel_notes)
        changed, new_val = imgui.slider_float(
            f"##detune_{node.id}", avg_detune, -100.0, 100.0, "%.0fc"
        )
        if changed and sel_notes:
            delta = new_val - avg_detune
            self._apply_to_selected(node, state,
                lambda n: setattr(n, 'detune', max(-100, min(100, n.detune + delta))))
        if imgui.is_item_hovered():
            imgui.set_tooltip("Fine-tune selected notes in cents (-100 to +100)\nCtrl+drag a note vertically (TODO: C++ version)")
        imgui.same_line()
        if imgui.small_button(f"Reset##{node.id}_det"):
            self._apply_to_selected(node, state,
                lambda n: setattr(n, 'detune', 0.0))
        if imgui.is_item_hovered():
            imgui.set_tooltip("Reset detune to 0 for selected notes")

        end_y = imgui.get_cursor_pos().y
        return end_y - start_y

    def _draw_piano_roll(self, node: Node, area_height: float):
        """Draw a full piano roll editor for a MIDI timeline."""
        state = self._get_piano_state(node.id)
        cursor = imgui.get_cursor_screen_pos()
        draw_list = imgui.get_window_draw_list()
        avail_width = imgui.get_content_region_avail().x

        if area_height < 30:
            imgui.dummy(imgui.ImVec2(avail_width, area_height))
            return

        # Layout
        key_width = 40  # piano key labels on left
        grid_x = cursor.x + key_width
        grid_w = avail_width - key_width
        grid_y = cursor.y
        grid_h = area_height

        total_beats = self._get_timeline_beats(node)
        scroll_pitch = state['scroll_pitch']
        visible_range = state['visible_range']
        pitch_lo = scroll_pitch - visible_range // 2
        pitch_hi = scroll_pitch + visible_range // 2
        row_h = grid_h / max(visible_range, 1)

        # Background
        draw_list.add_rect_filled(
            imgui.ImVec2(cursor.x, grid_y),
            imgui.ImVec2(cursor.x + avail_width, grid_y + grid_h),
            imgui.IM_COL32(20, 20, 30, 255), 3.0
        )

        # Piano keys and horizontal pitch lines
        for i in range(visible_range + 1):
            pitch = pitch_hi - i
            y = grid_y + i * row_h

            if pitch < 0 or pitch > 127:
                continue

            is_black = self._is_black_key(pitch)

            # Row background — alternate for black/white keys
            if i < visible_range:
                row_color = imgui.IM_COL32(25, 25, 35, 255) if is_black \
                    else imgui.IM_COL32(30, 30, 42, 255)
                draw_list.add_rect_filled(
                    imgui.ImVec2(grid_x, y),
                    imgui.ImVec2(grid_x + grid_w, y + row_h),
                    row_color
                )

            # Horizontal line
            alpha = 60 if pitch % 12 == 0 else 20  # brighter at C
            draw_list.add_line(
                imgui.ImVec2(grid_x, y),
                imgui.ImVec2(grid_x + grid_w, y),
                imgui.IM_COL32(255, 255, 255, alpha)
            )

            # Key label
            if i < visible_range:
                key_color = imgui.IM_COL32(40, 40, 50, 255) if is_black \
                    else imgui.IM_COL32(60, 60, 70, 255)
                draw_list.add_rect_filled(
                    imgui.ImVec2(cursor.x, y),
                    imgui.ImVec2(cursor.x + key_width - 2, y + row_h),
                    key_color
                )
                name = self._note_name(pitch)
                text_color = imgui.IM_COL32(180, 180, 180, 255) if pitch % 12 == 0 \
                    else imgui.IM_COL32(120, 120, 120, 255)
                draw_list.add_text(
                    imgui.ImVec2(cursor.x + 3, y + 1),
                    text_color, name
                )

        # Vertical beat grid
        for beat in range(int(total_beats) + 1):
            x = grid_x + (beat / total_beats) * grid_w
            is_bar = beat % 4 == 0
            alpha = 100 if is_bar else 25
            draw_list.add_line(
                imgui.ImVec2(x, grid_y),
                imgui.ImVec2(x, grid_y + grid_h),
                imgui.IM_COL32(255, 255, 255, alpha)
            )
            if is_bar:
                draw_list.add_text(
                    imgui.ImVec2(x + 2, grid_y + 1),
                    imgui.IM_COL32(200, 200, 200, 120),
                    str(beat // 4 + 1)
                )

        # Draw clip boundaries as colored regions
        for clip in node.clips:
            cx1 = grid_x + (clip.start_beat / total_beats) * grid_w
            cx2 = grid_x + ((clip.start_beat + clip.length_beats) / total_beats) * grid_w
            cr, cg, cb = clip.color
            draw_list.add_rect_filled(
                imgui.ImVec2(cx1, grid_y),
                imgui.ImVec2(cx2, grid_y + grid_h),
                imgui.IM_COL32(cr, cg, cb, 15)
            )
            # Clip boundary lines
            draw_list.add_line(
                imgui.ImVec2(cx1, grid_y),
                imgui.ImVec2(cx1, grid_y + grid_h),
                imgui.IM_COL32(cr, cg, cb, 80)
            )

        # Draw notes from all clips
        beat_to_x = lambda b: grid_x + (b / total_beats) * grid_w
        pitch_to_y = lambda p: grid_y + (pitch_hi - p) * row_h

        DEGREE_NAMES = ['1st', '2nd', '3rd', '4th', '5th', '6th', '7th']
        selected = state['selected']
        scale = self._get_active_intervals(state)

        for ci, clip in enumerate(node.clips):
            cr, cg, cb = clip.color
            for ni, note in enumerate(clip.notes):
                abs_beat = clip.start_beat + note.offset
                nx1 = beat_to_x(abs_beat)
                nx2 = beat_to_x(abs_beat + note.duration)
                ny = pitch_to_y(note.pitch)

                # Only draw if visible
                if ny + row_h < grid_y or ny > grid_y + grid_h:
                    continue

                is_sel = (ci, ni) in selected

                # Vertical offset for detune (cents → fraction of row height)
                detune_offset = -(note.detune / 100.0) * row_h  # negative because y goes down
                ny += detune_offset

                # Note rectangle
                if is_sel:
                    draw_list.add_rect_filled(
                        imgui.ImVec2(nx1 + 1, ny + 1),
                        imgui.ImVec2(nx2 - 1, ny + row_h - 1),
                        imgui.IM_COL32(255, 255, 255, 240), 2.0
                    )
                    draw_list.add_rect(
                        imgui.ImVec2(nx1, ny),
                        imgui.ImVec2(nx2, ny + row_h),
                        imgui.IM_COL32(255, 220, 100, 255), 2.0, thickness=2.0
                    )
                else:
                    is_chromatic = note.chromatic_offset != 0
                    if is_chromatic:
                        # Orange tint for off-scale notes
                        draw_list.add_rect_filled(
                            imgui.ImVec2(nx1 + 1, ny + 1),
                            imgui.ImVec2(nx2 - 1, ny + row_h - 1),
                            imgui.IM_COL32(200, 130, 60, 200), 2.0
                        )
                        draw_list.add_rect(
                            imgui.ImVec2(nx1 + 1, ny + 1),
                            imgui.ImVec2(nx2 - 1, ny + row_h - 1),
                            imgui.IM_COL32(240, 170, 80, 255), 2.0
                        )
                    else:
                        draw_list.add_rect_filled(
                            imgui.ImVec2(nx1 + 1, ny + 1),
                            imgui.ImVec2(nx2 - 1, ny + row_h - 1),
                            imgui.IM_COL32(cr, cg, cb, 220), 2.0
                        )
                        draw_list.add_rect(
                            imgui.ImVec2(nx1 + 1, ny + 1),
                            imgui.ImVec2(nx2 - 1, ny + row_h - 1),
                            imgui.IM_COL32(min(cr + 40, 255), min(cg + 40, 255),
                                           min(cb + 40, 255), 255), 2.0
                        )

                # Note label — only show what fits, clipped to note rect
                note_w = nx2 - nx1
                text_col = imgui.IM_COL32(0, 0, 0, 220) if is_sel \
                    else imgui.IM_COL32(255, 255, 255, 200)
                if note_w > 18 and row_h > 10:
                    name = self._note_name(note.pitch)
                    deg_str = ""
                    if note.degree >= 0 and note.degree < len(DEGREE_NAMES):
                        deg_str = DEGREE_NAMES[note.degree]

                    # Build detune suffix
                    detune_str = ""
                    if abs(note.detune) >= 0.5:
                        detune_str = f" {note.detune:+.0f}c"

                    if note_w > 100:
                        label = f"{name} {deg_str}{detune_str}" if deg_str else f"{name}{detune_str}"
                    elif note_w > 60:
                        label = f"{name}{detune_str}"
                    elif note_w > 40:
                        label = name
                    else:
                        label = f"{self.NOTE_NAMES[note.pitch % 12]}"

                    # Clip text to note bounds
                    draw_list.push_clip_rect(
                        imgui.ImVec2(nx1 + 2, ny),
                        imgui.ImVec2(nx2 - 2, ny + row_h)
                    )
                    draw_list.add_text(
                        imgui.ImVec2(nx1 + 4, ny + 1),
                        text_col, label
                    )
                    draw_list.pop_clip_rect()

                # Resize handles (left and right edges)
                handle_w = 5
                handle_alpha = 60 if is_sel else 40
                # Right handle
                draw_list.add_rect_filled(
                    imgui.ImVec2(nx2 - handle_w, ny + 1),
                    imgui.ImVec2(nx2 - 1, ny + row_h - 1),
                    imgui.IM_COL32(255, 255, 255, handle_alpha), 1.0
                )
                # Left handle
                draw_list.add_rect_filled(
                    imgui.ImVec2(nx1 + 1, ny + 1),
                    imgui.ImVec2(nx1 + handle_w, ny + row_h - 1),
                    imgui.IM_COL32(255, 255, 255, handle_alpha), 1.0
                )

        # Draw selection box if active
        if state['selecting_box']:
            sb_beat1, sb_pitch1, sb_beat2, sb_pitch2 = state['selecting_box']
            bx1 = beat_to_x(min(sb_beat1, sb_beat2))
            bx2 = beat_to_x(max(sb_beat1, sb_beat2))
            by1 = pitch_to_y(max(sb_pitch1, sb_pitch2))
            by2 = pitch_to_y(min(sb_pitch1, sb_pitch2)) + row_h
            draw_list.add_rect_filled(
                imgui.ImVec2(bx1, by1),
                imgui.ImVec2(bx2, by2),
                imgui.IM_COL32(255, 220, 100, 30)
            )
            draw_list.add_rect(
                imgui.ImVec2(bx1, by1),
                imgui.ImVec2(bx2, by2),
                imgui.IM_COL32(255, 220, 100, 150), 0, 0, 1.0
            )

        # Interaction: use invisible button over the grid area
        imgui.set_cursor_screen_pos(imgui.ImVec2(grid_x, grid_y))
        imgui.invisible_button(f"##piano_grid_{node.id}", imgui.ImVec2(grid_w, grid_h))
        is_hovered = imgui.is_item_hovered()

        io = imgui.get_io()
        shift_held = io.key_shift
        alt_held = io.key_alt  # Alt = free drag (no snap)
        ctrl_held = io.key_ctrl  # Ctrl+drag = detune

        if is_hovered:
            mouse = imgui.get_mouse_pos()
            # Scroll pitch with mouse wheel
            wheel = io.mouse_wheel
            if wheel != 0:
                state['scroll_pitch'] = max(12, min(115,
                    int(state['scroll_pitch'] + wheel * 3)))

            # Calculate pitch and beat under mouse
            rel_x = mouse.x - grid_x
            rel_y = mouse.y - grid_y
            hover_beat = (rel_x / grid_w) * total_beats
            hover_pitch = int(pitch_hi - (rel_y / grid_h) * visible_range)

            snap = state['snap'] if state['snap'] > 0 else 0.0625

            def _snap_beat(b):
                """Snap beat to grid unless Alt is held."""
                if alt_held:
                    return b
                return round(b / snap) * snap

            # Find note under cursor, return (ci, ni, edge)
            # edge: 'left', 'right', or 'body'
            def _find_note_at(beat, pitch):
                for ci, clip in enumerate(node.clips):
                    for ni, note in enumerate(clip.notes):
                        abs_beat = clip.start_beat + note.offset
                        if abs_beat <= beat <= abs_beat + note.duration and note.pitch == pitch:
                            nx1 = beat_to_x(abs_beat)
                            nx2 = beat_to_x(abs_beat + note.duration)
                            if mouse.x < nx1 + 7:
                                return ci, ni, 'left'
                            elif mouse.x > nx2 - 7:
                                return ci, ni, 'right'
                            else:
                                return ci, ni, 'body'
                return None

            # Set cursor shape based on what's under mouse
            probe = _find_note_at(hover_beat, hover_pitch)
            if probe and probe[2] in ('left', 'right'):
                imgui.set_mouse_cursor(imgui.MouseCursor_.resize_ew)

            if imgui.is_mouse_clicked(imgui.MouseButton_.left):
                hit = _find_note_at(hover_beat, hover_pitch)

                if hit and shift_held:
                    ci, ni, _ = hit
                    key = (ci, ni)
                    if key in state['selected']:
                        state['selected'].discard(key)
                    else:
                        state['selected'].add(key)
                elif hit:
                    ci, ni, edge = hit
                    if ctrl_held:
                        mode = 'detune'
                    elif edge == 'left':
                        mode = 'resize_left'
                    elif edge == 'right':
                        mode = 'resize_right'
                    else:
                        mode = 'move'

                    if (ci, ni) not in state['selected']:
                        state['selected'] = {(ci, ni)}

                    state['dragging'] = (ci, ni, mode)
                    state['drag_start'] = (hover_beat, hover_pitch)
                else:
                    # Click on empty: start potential box select or note place
                    if not shift_held:
                        state['selected'] = set()
                    state['_empty_click'] = (hover_beat, hover_pitch)
                    state['selecting_box'] = (hover_beat, hover_pitch, hover_beat, hover_pitch)

            # Box selection drag
            if state['selecting_box'] and imgui.is_mouse_down(imgui.MouseButton_.left):
                sb = state['selecting_box']
                state['selecting_box'] = (sb[0], sb[1], hover_beat, hover_pitch)

            # Finish box selection or place note
            if state['selecting_box'] and imgui.is_mouse_released(imgui.MouseButton_.left):
                sb = state['selecting_box']
                b1, b2 = min(sb[0], sb[2]), max(sb[0], sb[2])
                p1, p2 = min(sb[1], sb[3]), max(sb[1], sb[3])
                drag_dist = abs(sb[2] - sb[0]) + abs(sb[3] - sb[1])

                if drag_dist < 0.3:
                    # Barely moved — place a note instead
                    click_beat, click_pitch = state.get('_empty_click', (hover_beat, hover_pitch))
                    snapped_beat = _snap_beat(click_beat)
                    target_clip = None
                    for clip in node.clips:
                        if clip.start_beat <= snapped_beat < clip.start_beat + clip.length_beats:
                            target_clip = clip
                            break
                    if target_clip:
                        local_beat = snapped_beat - target_clip.start_beat
                        dur = snap * 4 if not alt_held else 0.5
                        new_note = MidiNote(local_beat, click_pitch, dur)
                        root = state['key_root']
                        sc = self._get_active_intervals(state)
                        deg, oct, chrom = self._pitch_to_degree(click_pitch, root, sc)
                        new_note.degree = deg
                        new_note.octave = oct
                        new_note.chromatic_offset = chrom
                        target_clip.notes.append(new_note)
                else:
                    # Dragged enough — select notes in box
                    for ci, clip in enumerate(node.clips):
                        for ni, note in enumerate(clip.notes):
                            abs_beat = clip.start_beat + note.offset
                            if abs_beat + note.duration >= b1 and abs_beat <= b2 \
                                    and p1 <= note.pitch <= p2:
                                state['selected'].add((ci, ni))

                state['selecting_box'] = None
                state['_empty_click'] = None

            # Dragging
            if state['dragging'] and imgui.is_mouse_down(imgui.MouseButton_.left):
                ci, ni, mode = state['dragging']
                if ci < len(node.clips) and ni < len(node.clips[ci].notes):
                    start_beat, start_pitch = state['drag_start']

                    if mode == 'move':
                        delta_beat = hover_beat - start_beat
                        delta_pitch = hover_pitch - start_pitch
                        if delta_beat != 0 or delta_pitch != 0:
                            for sci, sni in state['selected']:
                                if sci < len(node.clips) and sni < len(node.clips[sci].notes):
                                    n = node.clips[sci].notes[sni]
                                    new_off = n.offset + delta_beat
                                    n.offset = max(0, _snap_beat(new_off) if not alt_held else new_off)
                                    n.pitch = max(0, min(127, n.pitch + delta_pitch))
                            state['drag_start'] = (hover_beat, hover_pitch)

                    elif mode == 'resize_right':
                        n = node.clips[ci].notes[ni]
                        abs_start = node.clips[ci].start_beat + n.offset
                        new_end = _snap_beat(hover_beat) if not alt_held else hover_beat
                        n.duration = max(0.03125, new_end - abs_start)

                    elif mode == 'resize_left':
                        n = node.clips[ci].notes[ni]
                        abs_end = node.clips[ci].start_beat + n.offset + n.duration
                        new_start = _snap_beat(hover_beat) if not alt_held else hover_beat
                        new_start = min(new_start, abs_end - 0.03125)
                        new_start = max(node.clips[ci].start_beat, new_start)
                        n.duration = abs_end - new_start
                        n.offset = new_start - node.clips[ci].start_beat

                    elif mode == 'detune':
                        # Ctrl+drag vertically to detune
                        # Mouse Y delta → cents (moving up = positive detune)
                        mouse_delta_y = io.mouse_delta.y
                        cents_delta = -(mouse_delta_y / row_h) * 100  # pixels to cents
                        for sci, sni in state['selected']:
                            if sci < len(node.clips) and sni < len(node.clips[sci].notes):
                                n = node.clips[sci].notes[sni]
                                n.detune = max(-100, min(100, n.detune + cents_delta))

            if imgui.is_mouse_released(imgui.MouseButton_.left):
                state['dragging'] = None
                state['drag_start'] = None

            # Right-click context menu
            if imgui.is_mouse_clicked(imgui.MouseButton_.right):
                hit = _find_note_at(hover_beat, hover_pitch)
                if hit:
                    ci, ni, _ = hit
                    if (ci, ni) not in state['selected']:
                        state['selected'] = {(ci, ni)}
                    state['_rclick_note'] = (ci, ni)
                    imgui.open_popup(f"##note_ctx_{node.id}")
                else:
                    state['_rclick_beat'] = hover_beat
                    state['_rclick_pitch'] = hover_pitch
                    imgui.open_popup(f"##empty_ctx_{node.id}")

        # Note right-click context menu
        num_sel = len(state['selected'])
        sel_label = f"{num_sel} note{'s' if num_sel != 1 else ''}"
        root = state['key_root']
        intervals = self._get_active_intervals(state)

        if imgui.begin_popup(f"##note_ctx_{node.id}"):
            imgui.text_colored(imgui.ImVec4(0.7, 0.7, 0.7, 1.0), sel_label)
            imgui.separator()

            if imgui.menu_item_simple("Delete"):
                for sci, sni in sorted(state['selected'], reverse=True):
                    if sci < len(node.clips) and sni < len(node.clips[sci].notes):
                        node.clips[sci].notes.pop(sni)
                state['selected'] = set()

            if imgui.menu_item_simple("Duplicate"):
                new_sel = set()
                for sci, sni in sorted(state['selected']):
                    if sci < len(node.clips) and sni < len(node.clips[sci].notes):
                        orig = node.clips[sci].notes[sni]
                        dup = MidiNote(orig.offset + 0.25, orig.pitch, orig.duration,
                                       orig.degree, orig.octave, orig.chromatic_offset, orig.detune)
                        node.clips[sci].notes.append(dup)
                        new_sel.add((sci, len(node.clips[sci].notes) - 1))
                state['selected'] = new_sel

            imgui.separator()

            if imgui.begin_menu("Transpose"):
                if imgui.menu_item_simple("+1 Semitone"):
                    self._apply_to_selected(node, state,
                        lambda n: setattr(n, 'pitch', min(127, n.pitch + 1)))
                if imgui.menu_item_simple("-1 Semitone"):
                    self._apply_to_selected(node, state,
                        lambda n: setattr(n, 'pitch', max(0, n.pitch - 1)))
                if imgui.menu_item_simple("+1 Octave"):
                    self._apply_to_selected(node, state,
                        lambda n: setattr(n, 'pitch', min(127, n.pitch + 12)))
                if imgui.menu_item_simple("-1 Octave"):
                    self._apply_to_selected(node, state,
                        lambda n: setattr(n, 'pitch', max(0, n.pitch - 12)))
                imgui.end_menu()

            if imgui.begin_menu("Duration"):
                if imgui.menu_item_simple("Double"):
                    self._apply_to_selected(node, state,
                        lambda n: setattr(n, 'duration', n.duration * 2))
                if imgui.menu_item_simple("Halve"):
                    self._apply_to_selected(node, state,
                        lambda n: setattr(n, 'duration', max(0.125, n.duration / 2)))
                imgui.separator()
                for label, val in [("1/16", 0.25), ("1/8", 0.5), ("1/4", 1.0), ("1/2", 2.0), ("1", 4.0)]:
                    if imgui.menu_item_simple(f"Set to {label}"):
                        self._apply_to_selected(node, state,
                            lambda n, v=val: setattr(n, 'duration', v))
                imgui.end_menu()

            if imgui.begin_menu("Scale"):
                if imgui.menu_item_simple("Snap to Scale"):
                    self._apply_to_selected(node, state,
                        lambda n: setattr(n, 'pitch', self._snap_to_scale(n.pitch, root, intervals)))
                if imgui.menu_item_simple("Change Key"):
                    for clip in node.clips:
                        self._rekey_notes(clip, root, intervals)
                imgui.end_menu()

            if imgui.begin_menu("Detune"):
                if imgui.menu_item_simple("Reset to 0"):
                    self._apply_to_selected(node, state,
                        lambda n: setattr(n, 'detune', 0.0))
                for label, val in [("-50c", -50), ("-25c", -25), ("+25c", 25), ("+50c", 50)]:
                    if imgui.menu_item_simple(f"Set {label}"):
                        self._apply_to_selected(node, state,
                            lambda n, v=val: setattr(n, 'detune', float(v)))
                imgui.end_menu()

            imgui.separator()
            if imgui.menu_item_simple("Select All"):
                state['selected'] = set()
                for ci2, clip in enumerate(node.clips):
                    for ni2 in range(len(clip.notes)):
                        state['selected'].add((ci2, ni2))

            if imgui.menu_item_simple("Reverse"):
                sel = self._get_all_selected_notes(node, state)
                if sel:
                    notes_data = [(ci2, ni2, node.clips[ci2].notes[ni2]) for ci2, ni2 in sel]
                    offsets = [node.clips[ci2].start_beat + n.offset for ci2, ni2, n in notes_data]
                    min_off = min(offsets)
                    max_off = max(o + n.duration for o, (_, _, n) in zip(offsets, notes_data))
                    for (ci2, ni2, note), abs_off in zip(notes_data, offsets):
                        new_abs = max_off - (abs_off - min_off) - note.duration
                        note.offset = max(0, new_abs - node.clips[ci2].start_beat)

            imgui.end_popup()

        # Empty space right-click context menu
        if imgui.begin_popup(f"##empty_ctx_{node.id}"):
            beat = state.get('_rclick_beat', 0)
            pitch = state.get('_rclick_pitch', 60)

            if imgui.menu_item_simple("Place Note Here"):
                snap_val = state['snap'] if state['snap'] > 0 else 0.0625
                snapped = round(beat / snap_val) * snap_val
                for clip in node.clips:
                    if clip.start_beat <= snapped < clip.start_beat + clip.length_beats:
                        local = snapped - clip.start_beat
                        new_note = MidiNote(local, pitch, snap_val * 4)
                        deg, oct, chrom = self._pitch_to_degree(pitch, root, intervals)
                        new_note.degree = deg
                        new_note.octave = oct
                        new_note.chromatic_offset = chrom
                        clip.notes.append(new_note)
                        break

            if imgui.menu_item_simple("Select All"):
                state['selected'] = set()
                for ci2, clip in enumerate(node.clips):
                    for ni2 in range(len(clip.notes)):
                        state['selected'].add((ci2, ni2))

            if imgui.menu_item_simple("Deselect All"):
                state['selected'] = set()

            if imgui.menu_item_simple("Paste (TODO)"):
                pass

            imgui.end_popup()

        # Reserve space
        imgui.set_cursor_screen_pos(imgui.ImVec2(cursor.x, grid_y + grid_h))
        imgui.dummy(imgui.ImVec2(avail_width, 1))

    def _draw_audio_editor(self, node: Node, area_height: float):
        """Draw the audio clip timeline editor."""
        cursor = imgui.get_cursor_screen_pos()
        draw_list = imgui.get_window_draw_list()
        tl_width = imgui.get_content_region_avail().x
        tl_height = area_height

        if tl_height < 20:
            imgui.dummy(imgui.ImVec2(tl_width, tl_height))
            return

        total_beats = self._get_timeline_beats(node)

        # Background
        draw_list.add_rect_filled(
            imgui.ImVec2(cursor.x, cursor.y),
            imgui.ImVec2(cursor.x + tl_width, cursor.y + tl_height),
            imgui.IM_COL32(15, 15, 25, 255), 3.0
        )

        # Beat grid
        for beat in range(int(total_beats) + 1):
            x = cursor.x + (beat / total_beats) * tl_width
            is_bar = beat % 4 == 0
            if is_bar:
                draw_list.add_text(
                    imgui.ImVec2(x + 2, cursor.y + 2),
                    imgui.IM_COL32(200, 200, 200, 180),
                    str(beat // 4 + 1)
                )
            alpha = 100 if is_bar else 30
            draw_list.add_line(
                imgui.ImVec2(x, cursor.y + 14),
                imgui.ImVec2(x, cursor.y + tl_height),
                imgui.IM_COL32(255, 255, 255, alpha)
            )

        clip_area_y = cursor.y + 16
        clip_area_h = tl_height - 20

        for clip in node.clips:
            x1 = cursor.x + (clip.start_beat / total_beats) * tl_width
            x2 = cursor.x + ((clip.start_beat + clip.length_beats) / total_beats) * tl_width
            cr, cg, cb = clip.color

            draw_list.add_rect_filled(
                imgui.ImVec2(x1, clip_area_y),
                imgui.ImVec2(x2, clip_area_y + clip_area_h),
                imgui.IM_COL32(cr, cg, cb, 60), 3.0
            )
            draw_list.add_rect(
                imgui.ImVec2(x1, clip_area_y),
                imgui.ImVec2(x2, clip_area_y + clip_area_h),
                imgui.IM_COL32(cr, cg, cb, 160), 3.0
            )

            self._draw_audio_waveform(draw_list, clip, x1, x2, clip_area_y, clip_area_h)

            draw_list.add_text(
                imgui.ImVec2(x1 + 4, clip_area_y + 2),
                imgui.IM_COL32(255, 255, 255, 220),
                clip.name
            )

        imgui.dummy(imgui.ImVec2(tl_width, tl_height))


def main():
    app = SoundShop()

    runner_params = immapp.RunnerParams()
    runner_params.app_window_params.window_title = "SoundShop"
    runner_params.app_window_params.window_geometry.size = (1280, 720)
    runner_params.callbacks.show_gui = app.gui
    runner_params.fps_idling.enable_idling = False

    addons = immapp.AddOnsParams()
    addons.with_node_editor = True

    immapp.run(runner_params, addons)


if __name__ == "__main__":
    main()
