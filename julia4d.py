"""
4D Julia Set Explorer
=====================
Interactive desktop application for exploring 4D Julia sets (z -> z^2 + c).
Renders arbitrary 2D cross-sections and 3D hyperplane slices through 4D space,
with full 4D rotation controls across all six rotation planes.

Dependencies: numpy, numba, pyvista, pyvistaqt, matplotlib, PyQt5 (or PyQt6)
"""

import sys
import math
import time
import numpy as np
from numba import njit, prange

# ── Qt compatibility ──────────────────────────────────────────────────────────

try:
    from PyQt5.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QGridLayout, QLabel, QPushButton, QGroupBox, QSizePolicy,
        QStatusBar, QMessageBox, QStackedWidget,
    )
    from PyQt5.QtGui import QImage, QPixmap, QPainter, QColor, QFont, QKeySequence
    from PyQt5.QtCore import Qt, QTimer, QSize, pyqtSignal, QThread, QPoint
    QT_VERSION = 5
    FORMAT_RGB888 = QImage.Format_RGB888
    LEFT_BUTTON = Qt.LeftButton
    STRONG_FOCUS = Qt.StrongFocus
    ALIGN_CENTER = Qt.AlignCenter
    ALIGN_LEFT = Qt.AlignLeft
    KEY_MAP = {
        'Left': Qt.Key_Left, 'Right': Qt.Key_Right,
        'Up': Qt.Key_Up, 'Down': Qt.Key_Down,
        'Plus': Qt.Key_Plus, 'Minus': Qt.Key_Minus,
        'Equal': Qt.Key_Equal,
        'Tab': Qt.Key_Tab, 'R': Qt.Key_R, 'C': Qt.Key_C,
        'H': Qt.Key_H, 'W': Qt.Key_W, 'S': Qt.Key_S,
        'Q': Qt.Key_Q, 'E': Qt.Key_E,
        'I': Qt.Key_I, 'K': Qt.Key_K,
        'Period': Qt.Key_Period, 'Less': Qt.Key_Less,
        'Comma': Qt.Key_Comma, 'Greater': Qt.Key_Greater,
        '1': Qt.Key_1, '2': Qt.Key_2, '3': Qt.Key_3,
        '4': Qt.Key_4, '5': Qt.Key_5, '6': Qt.Key_6,
    }
except ImportError:
    from PyQt6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QGridLayout, QLabel, QPushButton, QGroupBox, QSizePolicy,
        QStatusBar, QMessageBox, QStackedWidget,
    )
    from PyQt6.QtGui import QImage, QPixmap, QPainter, QColor, QFont, QKeySequence
    from PyQt6.QtCore import Qt, QTimer, QSize, pyqtSignal, QThread, QPoint
    QT_VERSION = 6
    FORMAT_RGB888 = QImage.Format.Format_RGB888
    LEFT_BUTTON = Qt.MouseButton.LeftButton
    STRONG_FOCUS = Qt.FocusPolicy.StrongFocus
    ALIGN_CENTER = Qt.AlignmentFlag.AlignCenter
    ALIGN_LEFT = Qt.AlignmentFlag.AlignLeft
    KEY_MAP = {
        'Left': Qt.Key.Key_Left, 'Right': Qt.Key.Key_Right,
        'Up': Qt.Key.Key_Up, 'Down': Qt.Key.Key_Down,
        'Plus': Qt.Key.Key_Plus, 'Minus': Qt.Key.Key_Minus,
        'Equal': Qt.Key.Key_Equal,
        'Tab': Qt.Key.Key_Tab, 'R': Qt.Key.Key_R, 'C': Qt.Key.Key_C,
        'H': Qt.Key.Key_H, 'W': Qt.Key.Key_W, 'S': Qt.Key.Key_S,
        'Q': Qt.Key.Key_Q, 'E': Qt.Key.Key_E,
        'I': Qt.Key.Key_I, 'K': Qt.Key.Key_K,
        'Period': Qt.Key.Key_Period, 'Less': Qt.Key.Key_Less,
        'Comma': Qt.Key.Key_Comma, 'Greater': Qt.Key.Key_Greater,
        '1': Qt.Key.Key_1, '2': Qt.Key.Key_2, '3': Qt.Key.Key_3,
        '4': Qt.Key.Key_4, '5': Qt.Key.Key_5, '6': Qt.Key.Key_6,
    }

import pyvista as pv
from pyvistaqt import QtInteractor
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap

# ── Constants ─────────────────────────────────────────────────────────────────

LOG2 = math.log(2.0)

ROTATION_PLANES = [
    (0, 1, "X-Y", "Screen rotation"),
    (0, 2, "X-R", "Mix X into Re(c)"),
    (0, 3, "X-I", "Mix X into Im(c)"),
    (1, 2, "Y-R", "Mix Y into Re(c)"),
    (1, 3, "Y-I", "Mix Y into Im(c)"),
    (2, 3, "R-I", "Rotate c-plane"),
]

AXIS_NAMES = ["X (Re z)", "Y (Im z)", "R (Re c)", "I (Im c)"]

PALETTES = [
    ("Ocean",   (0.5, 0.5, 0.5),   (0.5, 0.5, 0.5),   (1.0, 1.0, 1.0),   (0.0, 0.10, 0.20)),
    ("Ember",   (0.5, 0.1, 0.0),   (0.5, 0.4, 0.3),   (1.0, 0.7, 0.4),   (0.0, 0.15, 0.20)),
    ("Ice",     (0.0, 0.15, 0.25), (0.35, 0.45, 0.55), (1.0, 1.0, 1.0),   (0.0, 0.15, 0.25)),
    ("Rainbow", (0.5, 0.5, 0.5),   (0.5, 0.5, 0.5),   (1.0, 1.0, 1.0),   (0.0, 0.33, 0.67)),
    ("Inferno", None, None, None, None),
]

PRESETS = [
    ("Dendrite",   np.array([0.0, 0.0, -0.7269, 0.1889])),
    ("Rabbit",     np.array([0.0, 0.0, -0.123, 0.745])),
    ("Spiral",     np.array([0.0, 0.0, -0.8, 0.156])),
    ("Siegel",     np.array([0.0, 0.0, -0.391, -0.587])),
    ("Lightning",  np.array([0.0, 0.0, 0.355, 0.355])),
    ("Galaxy",     np.array([0.0, 0.0, -0.162, 1.04])),
]

MANDELBROT_PRESET_ORIGIN = np.array([0.0, 0.0, -0.5, 0.0])
MANDELBROT_PRESET_BASIS = np.array([
    [0.0, 0.0, 1.0, 0.0],
    [0.0, 0.0, 0.0, 1.0],
    [1.0, 0.0, 0.0, 0.0],
    [0.0, 1.0, 0.0, 0.0],
], dtype=np.float64)

# ── Dark Theme ────────────────────────────────────────────────────────────────

DARK_STYLE = """
QMainWindow, QWidget {
    background-color: #0a0a1a;
    color: #bbbbcc;
    font-family: Consolas, Menlo, Monaco, monospace;
    font-size: 12px;
}
QGroupBox {
    border: 1px solid #223;
    border-radius: 6px;
    margin-top: 10px;
    padding: 12px 8px 8px 8px;
    font-weight: bold;
    color: #0cf;
    font-size: 11px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
}
QPushButton {
    background-color: #12122a;
    color: #889;
    border: 1px solid #334;
    border-radius: 4px;
    padding: 5px 10px;
    font-size: 11px;
    min-width: 28px;
}
QPushButton:hover {
    background-color: #1e1e3a;
    color: #def;
    border-color: #556;
}
QPushButton:pressed {
    background-color: #0a0a20;
    border-color: #08a;
}
QPushButton[checked="true"], QPushButton:checked {
    background-color: #0a2040;
    color: #0df;
    border-color: #0af;
}
QLabel {
    color: #99a;
    font-size: 11px;
}
QStatusBar {
    background-color: #060612;
    color: #667;
    font-size: 11px;
    border-top: 1px solid #1a1a2e;
}
QStatusBar QLabel {
    color: #667;
    padding: 2px 6px;
}
"""

# ── Numba JIT Functions ──────────────────────────────────────────────────────

@njit(parallel=True, cache=True)
def julia_2d(origin, u, v, width, height, max_iter, zoom):
    """Compute 2D escape-time slice. Returns float32[height, width].
    Value = smooth iteration count, or -1 for interior."""
    result = np.empty((height, width), dtype=np.float32)
    min_dim = min(width, height)
    scale = 2.0 / zoom
    for py in prange(height):
        for px in range(width):
            sx = (px - width * 0.5) / min_dim * scale
            sy = ((height - 1 - py) - height * 0.5) / min_dim * scale
            zr = origin[0] + sx * u[0] + sy * v[0]
            zi = origin[1] + sx * u[1] + sy * v[1]
            cr = origin[2] + sx * u[2] + sy * v[2]
            ci = origin[3] + sx * u[3] + sy * v[3]
            n = 0
            for _ in range(max_iter):
                zr2 = zr * zr
                zi2 = zi * zi
                if zr2 + zi2 > 256.0:
                    break
                zi = 2.0 * zr * zi + ci
                zr = zr2 - zi2 + cr
                n += 1
            if n < max_iter:
                mod2 = zr * zr + zi * zi
                log_zn = 0.5 * math.log(max(mod2, 1e-30))
                mu = n + 1.0 - math.log(max(log_zn, 1e-10)) / LOG2
                result[py, px] = mu
            else:
                result[py, px] = -1.0
    return result


@njit(parallel=True, cache=True)
def julia_3d(origin, u, v, w, N, max_iter):
    """Compute 3D escape-time volume. Returns float32[N,N,N], values in [0,1]."""
    result = np.empty((N, N, N), dtype=np.float32)
    scale = 4.0 / (N - 1)
    for iz in prange(N):
        sz = iz * scale - 2.0
        for iy in range(N):
            sy = iy * scale - 2.0
            for ix in range(N):
                sx = ix * scale - 2.0
                zr = origin[0] + sx * u[0] + sy * v[0] + sz * w[0]
                zi = origin[1] + sx * u[1] + sy * v[1] + sz * w[1]
                cr = origin[2] + sx * u[2] + sy * v[2] + sz * w[2]
                ci = origin[3] + sx * u[3] + sy * v[3] + sz * w[3]
                n = 0
                for _ in range(max_iter):
                    zr2 = zr * zr
                    zi2 = zi * zi
                    if zr2 + zi2 > 4.0:
                        break
                    zi = 2.0 * zr * zi + ci
                    zr = zr2 - zi2 + cr
                    n += 1
                result[iz, iy, ix] = n / max_iter
    return result


# ── Color Palette Functions ───────────────────────────────────────────────────

def iq_palette(t, a, b, c, d):
    """IQ cosine palette: a + b * cos(2*pi*(c*t + d)). Vectorized over t."""
    a = np.array(a, dtype=np.float64)
    b = np.array(b, dtype=np.float64)
    c = np.array(c, dtype=np.float64)
    d = np.array(d, dtype=np.float64)
    return a + b * np.cos(2.0 * np.pi * (c * t[..., np.newaxis] + d))


def apply_palette_2d(data, palette_idx, max_iter):
    """Apply color palette to 2D escape-time data. Returns uint8[h,w,3]."""
    h, w = data.shape
    rgb = np.zeros((h, w, 3), dtype=np.uint8)
    mask = data >= 0
    if not np.any(mask):
        return rgb

    vals = data[mask]
    t = vals / max_iter

    name, a, b, c, d = PALETTES[palette_idx]
    if name == "Inferno":
        cmap = plt.get_cmap('inferno')
        colors = cmap(t)[:, :3]
    else:
        colors = iq_palette(t, a, b, c, d)

    colors = np.clip(colors * 255, 0, 255).astype(np.uint8)
    rgb[mask] = colors
    return rgb


# Volume-rendering colormaps: monotonic dark→bright (no cycling).
# 3D structure comes from opacity and geometry, not color oscillation.
VOLUME_CMAPS = [
    ("Ocean",    [(0.00, '#020210'), (0.25, '#0a1845'), (0.45, '#0f4880'),
                  (0.60, '#18a0b8'), (0.75, '#60d8d0'), (0.90, '#c0f0f0'),
                  (1.00, '#f0f8ff')]),
    ("Ember",    [(0.00, '#080004'), (0.20, '#300808'), (0.40, '#801800'),
                  (0.55, '#c04000'), (0.70, '#e08020'), (0.85, '#f0c040'),
                  (1.00, '#fffff0')]),
    ("Ice",      [(0.00, '#000010'), (0.25, '#0a0a40'), (0.45, '#2040a0'),
                  (0.60, '#4080d0'), (0.75, '#80c0e8'), (0.90, '#c0e8ff'),
                  (1.00, '#f0f8ff')]),
    ("Rainbow",  [(0.00, '#100020'), (0.17, '#4000a0'), (0.33, '#0060e0'),
                  (0.50, '#00b060'), (0.67, '#a0d000'), (0.83, '#f08000'),
                  (1.00, '#ff2020')]),
    ("Inferno",  None),
]

def make_colormap_3d(palette_idx, n_colors=256):
    """Create a monotonic colormap for 3D volume rendering."""
    name, stops = VOLUME_CMAPS[palette_idx % len(VOLUME_CMAPS)]
    if stops is None:
        return plt.get_cmap('inferno')
    from matplotlib.colors import LinearSegmentedColormap, to_rgba
    colors_list = [(pos, to_rgba(col)) for pos, col in stops]
    positions = [c[0] for c in colors_list]
    rgba_vals = [c[1] for c in colors_list]
    return LinearSegmentedColormap.from_list(f"vol_{name}", list(zip(positions, rgba_vals)))


# ── 4D State ──────────────────────────────────────────────────────────────────

class State4D:
    """Mutable 4D viewing state."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.origin = np.array([0.0, 0.0, -0.7269, 0.1889])
        self.basis = np.eye(4, dtype=np.float64)
        self.zoom = 1.0
        self.max_iter = 256
        self.max_iter_3d = 48
        self.vol_res = 160
        self.mode = '2d'
        self.active_plane = 0
        self.palette = 0

    @property
    def u(self):
        return self.basis[:, 0]

    @property
    def v(self):
        return self.basis[:, 1]

    @property
    def w(self):
        return self.basis[:, 2]

    @property
    def t(self):
        return self.basis[:, 3]

    def rotate(self, plane_idx, angle_deg):
        """Apply rotation in the given plane to all basis vectors."""
        a1, a2, _, _ = ROTATION_PLANES[plane_idx]
        angle = math.radians(angle_deg)
        cos_a = math.cos(angle)
        sin_a = math.sin(angle)
        new_basis = self.basis.copy()
        for col in range(4):
            x = self.basis[a1, col]
            y = self.basis[a2, col]
            new_basis[a1, col] = cos_a * x - sin_a * y
            new_basis[a2, col] = sin_a * x + cos_a * y
        self.basis = new_basis

    def translate(self, axis_col, amount):
        """Move origin along a basis vector."""
        step = amount / self.zoom
        self.origin += step * self.basis[:, axis_col]

    def status_text(self):
        """Format current 4D position for display."""
        o = self.origin
        return (
            f"z\u2080 = ({o[0]:.4f}, {o[1]:.4f})  "
            f"c = ({o[2]:.4f}, {o[3]:.4f})  "
            f"zoom = {self.zoom:.2f}"
        )


# ── Background Volume Computation ────────────────────────────────────────────

class VolumeWorker(QThread):
    """Compute 3D volume in a background thread."""
    finished = pyqtSignal(object, float)

    def __init__(self, origin, u, v, w, N, max_iter):
        super().__init__()
        self.origin = origin.copy()
        self.u = u.copy()
        self.v = v.copy()
        self.w = w.copy()
        self.N = N
        self.max_iter = max_iter

    def run(self):
        t0 = time.perf_counter()
        vol = julia_3d(self.origin, self.u, self.v, self.w,
                       self.N, self.max_iter)
        elapsed = time.perf_counter() - t0
        self.finished.emit(vol, elapsed)


# ── 2D Fractal View Widget ───────────────────────────────────────────────────

class FractalView(QWidget):
    """Custom widget for rendering 2D fractal slices with pan/zoom."""

    def __init__(self, state, parent=None):
        super().__init__(parent)
        self.state = state
        self.pixmap = None
        self._last_data = None
        self._drag_start = None
        self._drag_origin = None
        self._dragging = False
        self._resize_timer = QTimer(self)
        self._resize_timer.setSingleShot(True)
        self._resize_timer.setInterval(50)
        self._resize_timer.timeout.connect(self._on_resize_done)
        self.setMinimumSize(400, 300)
        self.setFocusPolicy(STRONG_FOCUS)

    def sizeHint(self):
        return QSize(800, 600)

    def compute_and_display(self, low_res=False):
        """Compute the fractal and display it."""
        w = self.width()
        h = self.height()
        if w < 2 or h < 2:
            return 0.0

        if low_res:
            rw, rh = max(w // 2, 2), max(h // 2, 2)
        else:
            rw, rh = w, h

        t0 = time.perf_counter()
        data = julia_2d(
            self.state.origin,
            self.state.u.copy(),
            self.state.v.copy(),
            rw, rh,
            self.state.max_iter,
            self.state.zoom,
        )
        elapsed = time.perf_counter() - t0

        rgb = apply_palette_2d(data, self.state.palette, self.state.max_iter)
        self._last_data = data

        qimg = QImage(rgb.data, rw, rh, 3 * rw, FORMAT_RGB888).copy()
        self.pixmap = QPixmap.fromImage(qimg)
        if low_res:
            self.pixmap = self.pixmap.scaled(
                w, h,
                Qt.IgnoreAspectRatio if QT_VERSION == 5
                else Qt.AspectRatioMode.IgnoreAspectRatio,
                Qt.FastTransformation if QT_VERSION == 5
                else Qt.TransformationMode.FastTransformation,
            )
        self.update()
        return elapsed

    def paintEvent(self, event):
        if self.pixmap:
            painter = QPainter(self)
            painter.drawPixmap(0, 0, self.pixmap)
            painter.end()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._resize_timer.start()

    def _on_resize_done(self):
        if self.state.mode == '2d':
            self.compute_and_display()

    def mousePressEvent(self, event):
        if event.button() == LEFT_BUTTON:
            self._drag_start = event.pos()
            self._drag_origin = self.state.origin.copy()
            self._dragging = True

    def mouseMoveEvent(self, event):
        if self._dragging and self._drag_start is not None:
            delta = event.pos() - self._drag_start
            dx = delta.x()
            dy = delta.y()
            min_dim = min(self.width(), self.height())
            scale = 2.0 / (self.state.zoom * min_dim)
            self.state.origin = (
                self._drag_origin
                - dx * scale * self.state.u
                + dy * scale * self.state.v
            )
            self.compute_and_display(low_res=True)

    def mouseReleaseEvent(self, event):
        if event.button() == LEFT_BUTTON and self._dragging:
            self._dragging = False
            self._drag_start = None
            self.compute_and_display(low_res=False)

    def wheelEvent(self, event):
        delta = event.angleDelta().y()
        if delta == 0:
            return
        factor = 1.15 if delta > 0 else 1.0 / 1.15

        # Zoom toward cursor position
        pos = event.pos() if QT_VERSION == 5 else event.position().toPoint()
        min_dim = min(self.width(), self.height())
        scale = 2.0 / (self.state.zoom * min_dim)
        cx = (pos.x() - self.width() * 0.5) * scale
        cy = (self.height() * 0.5 - pos.y()) * scale

        # Move origin toward cursor, then zoom
        cursor_4d = cx * self.state.u + cy * self.state.v
        self.state.origin += cursor_4d * (1.0 - 1.0 / factor)
        self.state.zoom *= factor

        self.compute_and_display()


# ── Main Application Window ──────────────────────────────────────────────────

class Julia4DApp(QMainWindow):
    """Main application window for the 4D Julia Set Explorer."""

    def __init__(self):
        super().__init__()
        self.state = State4D()
        self._volume_worker = None
        self._3d_debounce = QTimer(self)
        self._3d_debounce.setSingleShot(True)
        self._3d_debounce.setInterval(500)
        self._3d_debounce.timeout.connect(self._compute_3d)
        self._last_fps = 0.0
        self._plane_buttons = []

        self.setWindowTitle("4D Julia Set Explorer")
        self.resize(1200, 800)
        self._build_ui()
        self._connect_shortcuts()

        # Initial render
        QTimer.singleShot(50, self._initial_render)

    def _initial_render(self):
        elapsed = self.fractal_view.compute_and_display()
        if elapsed > 0:
            self._last_fps = 1.0 / elapsed
        self._update_status()

    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        outer = QVBoxLayout(central)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(2)

        main_layout = QHBoxLayout()
        main_layout.setContentsMargins(4, 4, 4, 4)
        main_layout.setSpacing(4)

        # Left: viewport stack
        self._viewport_stack = QStackedWidget()
        self.fractal_view = FractalView(self.state)
        self._viewport_stack.addWidget(self.fractal_view)

        self._pv_container = QWidget()
        pv_layout = QVBoxLayout(self._pv_container)
        pv_layout.setContentsMargins(0, 0, 0, 0)
        self._pv_plotter = None
        self._viewport_stack.addWidget(self._pv_container)

        main_layout.addWidget(self._viewport_stack, stretch=1)

        # Right: control panel
        panel = QWidget()
        panel.setFixedWidth(210)
        panel_layout = QVBoxLayout(panel)
        panel_layout.setContentsMargins(4, 4, 4, 4)
        panel_layout.setSpacing(4)

        # Mode selector
        mode_group = QGroupBox("VIEW MODE")
        mode_lay = QHBoxLayout(mode_group)
        self._btn_2d = QPushButton("2D Slice")
        self._btn_3d = QPushButton("3D Volume")
        self._btn_2d.setCheckable(True)
        self._btn_3d.setCheckable(True)
        self._btn_2d.setChecked(True)
        self._btn_2d.clicked.connect(lambda: self._set_mode('2d'))
        self._btn_3d.clicked.connect(lambda: self._set_mode('3d'))
        mode_lay.addWidget(self._btn_2d)
        mode_lay.addWidget(self._btn_3d)
        panel_layout.addWidget(mode_group)

        # 4D rotation
        rot_group = QGroupBox("4D ROTATION")
        rot_lay = QVBoxLayout(rot_group)
        grid = QGridLayout()
        labels = ["XY", "XR", "XI", "YR", "YI", "RI"]
        for i, lbl in enumerate(labels):
            btn = QPushButton(lbl)
            btn.setCheckable(True)
            btn.setChecked(i == 0)
            btn.clicked.connect(lambda checked, idx=i: self._select_plane(idx))
            grid.addWidget(btn, i // 3, i % 3)
            self._plane_buttons.append(btn)
        rot_lay.addLayout(grid)

        self._plane_desc = QLabel(ROTATION_PLANES[0][3])
        self._plane_desc.setAlignment(ALIGN_CENTER)
        self._plane_desc.setStyleSheet("color: #556; font-size: 10px;")
        rot_lay.addWidget(self._plane_desc)

        rot_btns = QHBoxLayout()
        self._repeating_btn("\u25c4 Rot", lambda: self._do_rotate(-5), rot_btns)
        self._repeating_btn("Rot \u25ba", lambda: self._do_rotate(5), rot_btns)
        rot_lay.addLayout(rot_btns)
        panel_layout.addWidget(rot_group)

        # Movement: screen
        move_group = QGroupBox("MOVE (SCREEN)")
        move_lay = QGridLayout(move_group)
        self._repeating_btn("\u2190", lambda: self._do_translate(0, -0.05), move_lay, 0, 0)
        self._repeating_btn("\u2191", lambda: self._do_translate(1, 0.05), move_lay, 0, 1)
        self._repeating_btn("\u2193", lambda: self._do_translate(1, -0.05), move_lay, 0, 2)
        self._repeating_btn("\u2192", lambda: self._do_translate(0, 0.05), move_lay, 0, 3)
        panel_layout.addWidget(move_group)

        # Movement: depth
        depth_group = QGroupBox("MOVE (DEPTH)")
        depth_lay = QVBoxLayout(depth_group)
        row1 = QHBoxLayout()
        self._repeating_btn("In W", lambda: self._do_translate(2, 0.05), row1)
        self._repeating_btn("Out S", lambda: self._do_translate(2, -0.05), row1)
        depth_lay.addLayout(row1)
        row2 = QHBoxLayout()
        self._repeating_btn("4th+ Q", lambda: self._do_translate(3, 0.05), row2)
        self._repeating_btn("4th- E", lambda: self._do_translate(3, -0.05), row2)
        depth_lay.addLayout(row2)
        panel_layout.addWidget(depth_group)

        # View controls
        view_group = QGroupBox("VIEW CONTROLS")
        view_lay = QVBoxLayout(view_group)

        # Zoom
        zoom_row = QHBoxLayout()
        zoom_row.addWidget(QLabel("Zoom"))
        self._repeating_btn("+", lambda: self._do_zoom(1.15), zoom_row)
        self._repeating_btn("-", lambda: self._do_zoom(1 / 1.15), zoom_row)
        self._zoom_label = QLabel(f"{self.state.zoom:.2f}")
        self._zoom_label.setFixedWidth(50)
        zoom_row.addWidget(self._zoom_label)
        view_lay.addLayout(zoom_row)

        # Iterations
        iter_row = QHBoxLayout()
        iter_row.addWidget(QLabel("Iter"))
        self._repeating_btn("+", self._iter_up, iter_row)
        self._repeating_btn("-", self._iter_down, iter_row)
        self._iter_label = QLabel(str(self.state.max_iter))
        self._iter_label.setFixedWidth(50)
        iter_row.addWidget(self._iter_label)
        view_lay.addLayout(iter_row)

        # 3D Resolution
        res_row = QHBoxLayout()
        res_row.addWidget(QLabel("3D Res"))
        btn_res_up = QPushButton("+")
        btn_res_up.clicked.connect(self._res_up)
        res_row.addWidget(btn_res_up)
        btn_res_down = QPushButton("-")
        btn_res_down.clicked.connect(self._res_down)
        res_row.addWidget(btn_res_down)
        self._res_label = QLabel(str(self.state.vol_res))
        self._res_label.setFixedWidth(50)
        res_row.addWidget(self._res_label)
        view_lay.addLayout(res_row)

        # Palette
        pal_row = QHBoxLayout()
        btn_pal = QPushButton("Palette \u25ba")
        btn_pal.clicked.connect(self._cycle_palette)
        pal_row.addWidget(btn_pal)
        self._pal_label = QLabel(PALETTES[0][0])
        self._pal_label.setStyleSheet("color: #fa0;")
        pal_row.addWidget(self._pal_label)
        view_lay.addLayout(pal_row)

        panel_layout.addWidget(view_group)

        # Reset / Help
        action_row = QHBoxLayout()
        btn_reset = QPushButton("Reset")
        btn_reset.clicked.connect(self._do_reset)
        action_row.addWidget(btn_reset)
        btn_help = QPushButton("Help")
        btn_help.clicked.connect(self._show_help)
        action_row.addWidget(btn_help)
        panel_layout.addLayout(action_row)

        panel_layout.addStretch()
        main_layout.addWidget(panel)

        # Add main content area to outer layout
        outer.addLayout(main_layout, stretch=1)

        # Bottom: presets
        preset_row = QHBoxLayout()
        for name, origin in PRESETS:
            btn = QPushButton(name)
            btn.clicked.connect(lambda checked, n=name, o=origin: self._apply_preset(n, o))
            preset_row.addWidget(btn)
        btn_mandel = QPushButton("Mandelbrot")
        btn_mandel.clicked.connect(self._apply_mandelbrot)
        preset_row.addWidget(btn_mandel)
        outer.addLayout(preset_row)

        # Status bar
        self._status = QStatusBar()
        self.setStatusBar(self._status)
        self._status_label = QLabel()
        self._status.addWidget(self._status_label, stretch=1)

    def _repeating_btn(self, text, action_fn, layout, *grid_pos):
        """Create a button that repeats its action when held."""
        btn = QPushButton(text)
        timer = QTimer(btn)
        timer.setInterval(50)
        timer.timeout.connect(action_fn)
        btn.pressed.connect(lambda: (action_fn(), timer.start()))
        btn.released.connect(timer.stop)
        if grid_pos:
            layout.addWidget(btn, *grid_pos)
        elif hasattr(layout, 'addWidget'):
            layout.addWidget(btn)
        return btn

    # ── Mode switching ────────────────────────────────────────────────────

    def _set_mode(self, mode):
        self.state.mode = mode
        self._btn_2d.setChecked(mode == '2d')
        self._btn_3d.setChecked(mode == '3d')

        if mode == '2d':
            self._viewport_stack.setCurrentIndex(0)
            self._render_2d()
        else:
            self._viewport_stack.setCurrentIndex(1)
            self._ensure_plotter()
            self._request_3d_compute()
        self._update_status()

    def _ensure_plotter(self):
        """Create the PyVista plotter widget on first 3D use."""
        if self._pv_plotter is not None:
            return
        try:
            self._pv_plotter = QtInteractor(self._pv_container)
            layout = self._pv_container.layout()
            widget_to_add = self._pv_plotter
            layout.addWidget(widget_to_add)
            self._pv_plotter.set_background('#060610')
            self._pv_plotter.add_axes()
        except Exception as e:
            self._status_label.setText(f"3D init error: {e}")

    # ── Rendering ─────────────────────────────────────────────────────────

    def _render_2d(self):
        elapsed = self.fractal_view.compute_and_display()
        if elapsed > 0:
            self._last_fps = 1.0 / elapsed
        self._update_labels()
        self._update_status()

    def _request_3d_compute(self):
        """Debounced request for 3D volume computation."""
        self._3d_debounce.start()

    def _compute_3d(self):
        """Start background 3D computation."""
        if self._volume_worker is not None and self._volume_worker.isRunning():
            return
        self._status_label.setText("Computing 3D volume...")
        QApplication.processEvents()
        self._volume_worker = VolumeWorker(
            self.state.origin,
            self.state.u.copy(),
            self.state.v.copy(),
            self.state.w.copy(),
            self.state.vol_res,
            self.state.max_iter_3d,
        )
        self._volume_worker.finished.connect(self._on_volume_ready)
        self._volume_worker.start()

    def _on_volume_ready(self, vol, elapsed):
        """Handle completed 3D volume."""
        if self._pv_plotter is None:
            return
        self._last_fps = 1.0 / max(elapsed, 0.001)

        try:
            self._pv_plotter.clear()
            N = vol.shape[0]
            spacing = 4.0 / max(N - 1, 1)

            print(f"  Volume {N}\u00b3: min={vol.min():.4f} max={vol.max():.4f} "
                  f"mean={vol.mean():.4f} nonzero={np.count_nonzero(vol)}/{vol.size}")

            grid = pv.ImageData(
                dimensions=(N, N, N),
                spacing=(spacing, spacing, spacing),
                origin=(-2.0, -2.0, -2.0),
            )
            grid.point_data['escape'] = vol.ravel(order='F')

            cmap = make_colormap_3d(self.state.palette)

            # Opacity: exterior→transparent, boundary→visible shell, interior→transparent.
            # Per-voxel opacity accumulates along rays: even 0.1 × 80 voxels → opaque.
            # So interior MUST be ~0 or it becomes a solid white blob.
            #
            # 9 values evenly spaced across [0, 1]:
            #   0.000  0.125  0.250  0.375  0.500  0.625  0.750  0.875  1.000
            opacity = [0, 0, 0, 0.03, 0.12, 0.22, 0.15, 0.04, 0.0]

            # 'gpu' mapper does trilinear interpolation (smooth).
            # 'smart'/'fixed_point' show raw voxel blocks.
            added = False
            for mapper in ('gpu', 'smart'):
                try:
                    self._pv_plotter.add_volume(
                        grid, scalars='escape', cmap=cmap, opacity=opacity,
                        clim=[0, 1], mapper=mapper, shade=False,
                    )
                    print(f"  Volume added with '{mapper}' mapper")
                    added = True
                    break
                except Exception as ve:
                    print(f"  '{mapper}' mapper failed: {ve}")
            if not added:
                print("  Volume rendering unavailable, using isosurface fallback")
                contours = grid.contour(isosurfaces=8, scalars='escape',
                                        rng=[0.15, 0.95])
                if contours.n_points > 0:
                    self._pv_plotter.add_mesh(
                        contours, cmap=cmap, clim=[0, 1],
                        opacity=0.6, smooth_shading=True,
                    )

            self._pv_plotter.reset_camera()
        except Exception as e:
            self._status_label.setText(f"3D render error: {e}")
            import traceback; traceback.print_exc()
            return

        self._update_status()

    # ── Actions ───────────────────────────────────────────────────────────

    def _select_plane(self, idx):
        self.state.active_plane = idx
        for i, btn in enumerate(self._plane_buttons):
            btn.setChecked(i == idx)
        self._plane_desc.setText(ROTATION_PLANES[idx][3])

    def _do_rotate(self, angle):
        self.state.rotate(self.state.active_plane, angle)
        self._after_param_change()

    def _do_translate(self, axis, amount):
        self.state.translate(axis, amount)
        self._after_param_change()

    def _do_zoom(self, factor):
        self.state.zoom *= factor
        self._after_param_change()

    def _iter_up(self):
        if self.state.mode == '2d':
            self.state.max_iter = min(self.state.max_iter + 64, 4096)
        else:
            self.state.max_iter_3d = min(self.state.max_iter_3d + 16, 256)
        self._after_param_change()

    def _iter_down(self):
        if self.state.mode == '2d':
            self.state.max_iter = max(self.state.max_iter - 64, 32)
        else:
            self.state.max_iter_3d = max(self.state.max_iter_3d - 16, 16)
        self._after_param_change()

    def _res_up(self):
        self.state.vol_res = min(self.state.vol_res + 32, 320)
        self._res_label.setText(str(self.state.vol_res))
        if self.state.mode == '3d':
            self._request_3d_compute()

    def _res_down(self):
        self.state.vol_res = max(self.state.vol_res - 32, 64)
        self._res_label.setText(str(self.state.vol_res))
        if self.state.mode == '3d':
            self._request_3d_compute()

    def _cycle_palette(self):
        self.state.palette = (self.state.palette + 1) % len(PALETTES)
        self._pal_label.setText(PALETTES[self.state.palette][0])
        self._after_param_change()

    def _do_reset(self):
        self.state.reset()
        self._select_plane(0)
        self._update_labels()
        self._after_param_change()

    def _apply_preset(self, name, origin):
        self.state.origin = origin.copy()
        self.state.basis = np.eye(4, dtype=np.float64)
        self.state.zoom = 1.0
        self._after_param_change()

    def _apply_mandelbrot(self):
        self.state.origin = MANDELBROT_PRESET_ORIGIN.copy()
        self.state.basis = MANDELBROT_PRESET_BASIS.copy()
        self.state.zoom = 1.0
        self._after_param_change()

    def _after_param_change(self):
        """Called after any 4D parameter change."""
        self._update_labels()
        if self.state.mode == '2d':
            self._render_2d()
        else:
            self._request_3d_compute()

    def _update_labels(self):
        self._zoom_label.setText(f"{self.state.zoom:.2f}")
        if self.state.mode == '2d':
            self._iter_label.setText(str(self.state.max_iter))
        else:
            self._iter_label.setText(str(self.state.max_iter_3d))
        self._res_label.setText(str(self.state.vol_res))
        self._pal_label.setText(PALETTES[self.state.palette][0])

    def _update_status(self):
        mode_str = "2D" if self.state.mode == '2d' else "3D"
        plane = ROTATION_PLANES[self.state.active_plane][2]
        self._status_label.setText(
            f"[{mode_str}]  {self.state.status_text()}  "
            f"plane={plane}  "
            f"fps={self._last_fps:.1f}"
        )

    # ── Keyboard shortcuts ────────────────────────────────────────────────

    def _connect_shortcuts(self):
        pass  # Handled in keyPressEvent

    def keyPressEvent(self, event):
        key = event.key()
        km = KEY_MAP

        # Arrows: pan
        if key == km['Left']:
            self._do_translate(0, -0.05)
        elif key == km['Right']:
            self._do_translate(0, 0.05)
        elif key == km['Up']:
            self._do_translate(1, 0.05)
        elif key == km['Down']:
            self._do_translate(1, -0.05)

        # W/S: depth
        elif key == km['W']:
            self._do_translate(2, 0.05)
        elif key == km['S']:
            self._do_translate(2, -0.05)

        # Q/E: 4th dimension
        elif key == km['Q']:
            self._do_translate(3, 0.05)
        elif key == km['E']:
            self._do_translate(3, -0.05)

        # +/-: zoom
        elif key == km['Plus'] or key == km['Equal']:
            self._do_zoom(1.15)
        elif key == km['Minus']:
            self._do_zoom(1.0 / 1.15)

        # < / . : rotate
        elif key == km['Comma'] or key == km['Less']:
            self._do_rotate(-5)
        elif key == km['Period'] or key == km['Greater']:
            self._do_rotate(5)

        # 1-6: select plane
        elif key == km['1']:
            self._select_plane(0)
        elif key == km['2']:
            self._select_plane(1)
        elif key == km['3']:
            self._select_plane(2)
        elif key == km['4']:
            self._select_plane(3)
        elif key == km['5']:
            self._select_plane(4)
        elif key == km['6']:
            self._select_plane(5)

        # Tab: toggle mode
        elif key == km['Tab']:
            self._set_mode('3d' if self.state.mode == '2d' else '2d')

        # R: reset
        elif key == km['R']:
            self._do_reset()

        # C: cycle palette
        elif key == km['C']:
            self._cycle_palette()

        # I/K: iterations
        elif key == km['I']:
            self._iter_up()
        elif key == km['K']:
            self._iter_down()

        # H: help
        elif key == km['H']:
            self._show_help()

        else:
            super().keyPressEvent(event)

    # ── Help dialog ───────────────────────────────────────────────────────

    def _show_help(self):
        help_text = (
            "4D Julia Set Explorer\n"
            "=====================\n\n"
            "KEYBOARD SHORTCUTS\n"
            "------------------\n"
            "Arrow keys     Pan (screen X/Y)\n"
            "W / S          Move in/out (depth axis)\n"
            "Q / E          Move along 4th dimension\n"
            "+ / -          Zoom in/out\n"
            ", / .          Rotate in active plane\n"
            "1-6            Select rotation plane\n"
            "Tab            Toggle 2D/3D mode\n"
            "C              Cycle color palette\n"
            "I / K          Increase/decrease iterations\n"
            "R              Reset view\n"
            "H              Show this help\n\n"
            "MOUSE (2D MODE)\n"
            "---------------\n"
            "Drag           Pan\n"
            "Scroll         Zoom at cursor\n\n"
            "MOUSE (3D MODE)\n"
            "---------------\n"
            "Drag           Orbit camera\n"
            "Right-drag     Pan camera\n"
            "Scroll         Zoom camera\n\n"
            "ROTATION PLANES\n"
            "---------------\n"
            "1: X-Y  Screen rotation\n"
            "2: X-R  Mix X into Re(c)\n"
            "3: X-I  Mix X into Im(c)\n"
            "4: Y-R  Mix Y into Re(c)\n"
            "5: Y-I  Mix Y into Im(c)\n"
            "6: R-I  Rotate c-plane\n"
        )
        msg = QMessageBox(self)
        msg.setWindowTitle("Help")
        msg.setText(help_text)
        msg.setStyleSheet(
            "QMessageBox { background: #0a0a1a; }"
            "QLabel { color: #bbc; font-family: Consolas, monospace; font-size: 12px; }"
            "QPushButton { min-width: 60px; }"
        )
        msg.exec() if QT_VERSION == 6 else msg.exec_()

    # ── Cleanup ───────────────────────────────────────────────────────────

    def closeEvent(self, event):
        if self._volume_worker is not None and self._volume_worker.isRunning():
            self._volume_worker.quit()
            self._volume_worker.wait(2000)
        if self._pv_plotter is not None:
            try:
                self._pv_plotter.close()
            except Exception:
                pass
        event.accept()


# ── Entry Point ───────────────────────────────────────────────────────────────

if __name__ == '__main__':
    print("Compiling fractal engine...")
    _d = np.zeros(4, dtype=np.float64)
    _u = np.array([1.0, 0.0, 0.0, 0.0])
    _v = np.array([0.0, 1.0, 0.0, 0.0])
    _w = np.array([0.0, 0.0, 1.0, 0.0])
    julia_2d(_d, _u, _v, 4, 4, 10, 1.0)
    julia_3d(_d, _u, _v, _w, 4, 10)
    print("Ready!")

    app = QApplication(sys.argv)
    app.setStyle('Fusion')
    app.setStyleSheet(DARK_STYLE)
    window = Julia4DApp()
    window.show()
    sys.exit(getattr(app, 'exec', app.exec_)())
