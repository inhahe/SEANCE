"""
Search for quaternion Julia parameters that best match a reference image.

Searches over quaternion c values and camera angles. Uses silhouette
comparison (thresholded binary mask) so lighting/palette differences
don't affect the match.

Usage:
    py -3.14 search.py <reference.jpg> <julia4d.exe>
"""

import sys, os, subprocess, struct, math, itertools
from pathlib import Path

try:
    from PIL import Image
    import numpy as np
    HAS_PIL = True
except ImportError:
    HAS_PIL = False
    print("WARNING: Install Pillow+numpy for best results: pip install Pillow numpy")


def load_silhouette(path, size=(128, 128)):
    """Load image, resize, convert to binary silhouette (object vs background)."""
    if HAS_PIL:
        img = np.array(Image.open(path).convert('L').resize(size, Image.LANCZOS), dtype=np.float32)
        # Threshold: anything brighter than ~5% is "object"
        mask = (img > 12).astype(np.float32)
        return mask
    else:
        # BMP fallback
        with open(path, 'rb') as f:
            header = f.read(54)
            w = struct.unpack('<i', header[18:22])[0]
            h = struct.unpack('<i', header[22:26])[0]
            row_size = (w * 3 + 3) & ~3
            pixels = []
            for y in range(h):
                row = f.read(row_size)
                for x in range(w):
                    b, g, r = row[x*3], row[x*3+1], row[x*3+2]
                    pixels.append(0.299*r + 0.587*g + 0.114*b)
        tw, th = size
        mask = []
        for y in range(th):
            for x in range(tw):
                sx = int(x * w / tw)
                sy = int(y * h / th)
                mask.append(1.0 if pixels[sy * w + sx] > 12 else 0.0)
        return mask


def similarity(a, b):
    """IoU (intersection over union) of two binary silhouettes."""
    if HAS_PIL:
        a = a.flatten()
        b = b.flatten()
        intersection = np.sum(a * b)
        union = np.sum(np.clip(a + b, 0, 1))
        if union < 1: return 0.0
        return float(intersection / union)
    else:
        intersection = sum(1 for x, y in zip(a, b) if x > 0.5 and y > 0.5)
        union = sum(1 for x, y in zip(a, b) if x > 0.5 or y > 0.5)
        if union < 1: return 0.0
        return intersection / union


def render(exe, outpath, qc, cam_theta=0.4, cam_phi=0.3, cam_dist=6.0,
           w=200, h=200, palette=1, itr=48, steps=100):
    """Render a quaternion Julia set and save to BMP."""
    cmd = [exe, '--quat', '--save', outpath,
           '--width', str(w), '--height', str(h),
           '--qc', f'{qc[0]},{qc[1]},{qc[2]},{qc[3]}',
           '--palette', str(palette), '--iter', str(itr), '--steps', str(steps),
           '--cam-theta', f'{cam_theta:.3f}',
           '--cam-phi', f'{cam_phi:.3f}',
           '--cam-dist', f'{cam_dist:.1f}']
    try:
        subprocess.run(cmd, capture_output=True, timeout=30)
    except Exception as e:
        print(f"  render failed: {e}")


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <reference_image> <julia4d_exe>")
        sys.exit(1)

    ref_path = sys.argv[1]
    exe_path = sys.argv[2]
    tmp = Path("search_output")
    tmp.mkdir(exist_ok=True)

    print(f"Reference: {ref_path}")
    print(f"Executable: {exe_path}")

    ref = load_silhouette(ref_path)
    best_score = -1.0
    best_params = None

    def try_render(label, qc, theta=0.4, phi=0.3, dist=6.0):
        nonlocal best_score, best_params
        fname = f"{label}.bmp"
        fpath = str(tmp / fname)
        render(exe_path, fpath, qc, theta, phi, dist)
        if not os.path.exists(fpath):
            return -1.0
        test = load_silhouette(fpath)
        score = similarity(ref, test)
        if score > best_score:
            best_score = score
            best_params = {'qc': list(qc), 'theta': theta, 'phi': phi, 'dist': dist, 'score': score}
            print(f"  NEW BEST: qc=({qc[0]:.3f},{qc[1]:.3f},{qc[2]:.3f},{qc[3]:.3f}) "
                  f"cam=({theta:.2f},{phi:.2f},{dist:.1f}) IoU={score:.4f}")
        return score

    # ═══════════════════════════════════════════════════════════
    # Phase 1: Coarse sweep of quaternion c values
    # ═══════════════════════════════════════════════════════════
    # Classic quaternion Julia c values live roughly in |c| < 1.5
    # The user suspects "something simple" — try c with 2 components zero

    print("\n=== Phase 1: Sweep qc with two components zero ===")
    count = 0

    # Try c = (a, b, 0, 0) — most common case
    for a in [x * 0.1 for x in range(-15, 16)]:
        for b in [x * 0.1 for x in range(-15, 16)]:
            if a*a + b*b > 2.25: continue  # skip |c| > 1.5
            qc = (a, b, 0.0, 0.0)
            try_render(f"p1_ab_{count}", qc)
            count += 1

    # Try c = (a, 0, b, 0)
    for a in [x * 0.1 for x in range(-15, 16)]:
        for b in [x * 0.1 for x in range(-15, 16)]:
            if a*a + b*b > 2.25: continue
            qc = (a, 0.0, b, 0.0)
            try_render(f"p1_ac_{count}", qc)
            count += 1

    # Try c = (a, 0, 0, b)
    for a in [x * 0.1 for x in range(-15, 16)]:
        for b in [x * 0.1 for x in range(-15, 16)]:
            if a*a + b*b > 2.25: continue
            qc = (a, 0.0, 0.0, b)
            try_render(f"p1_ad_{count}", qc)
            count += 1

    print(f"\nPhase 1: {count} renders. Best IoU = {best_score:.4f}")
    if best_params:
        print(f"  qc = ({best_params['qc'][0]:.3f}, {best_params['qc'][1]:.3f}, "
              f"{best_params['qc'][2]:.3f}, {best_params['qc'][3]:.3f})")

    if best_score < 0.01:
        print("\nPhase 1 found nothing. Trying all 4-component combinations (coarser)...")
        for a in [x * 0.2 for x in range(-7, 8)]:
            for b in [x * 0.2 for x in range(-7, 8)]:
                for c in [x * 0.2 for x in range(-7, 8)]:
                    for d in [x * 0.2 for x in range(-7, 8)]:
                        if a*a+b*b+c*c+d*d > 2.25: continue
                        qc = (a, b, c, d)
                        try_render(f"p1b_{count}", qc)
                        count += 1

    if not best_params:
        print("No matches found at all!")
        return

    # ═══════════════════════════════════════════════════════════
    # Phase 2: Refine qc around best, ±0.05 steps
    # ═══════════════════════════════════════════════════════════
    print(f"\n=== Phase 2: Refine qc ±0.15 in 0.03 steps around best ===")
    bc = best_params['qc']
    count = 0
    offsets = [x * 0.03 for x in range(-5, 6)]
    for da in offsets:
        for db in offsets:
            qc = (bc[0]+da, bc[1]+db, bc[2], bc[3])
            try_render(f"p2a_{count}", qc)
            count += 1
    # Also refine the other two components if nonzero
    if bc[2] != 0 or bc[3] != 0:
        for dc in offsets:
            for dd in offsets:
                qc = (bc[0], bc[1], bc[2]+dc, bc[3]+dd)
                try_render(f"p2b_{count}", qc)
                count += 1

    print(f"\nPhase 2: {count} renders. Best IoU = {best_score:.4f}")

    # ═══════════════════════════════════════════════════════════
    # Phase 3: Try different camera angles with best qc
    # ═══════════════════════════════════════════════════════════
    print(f"\n=== Phase 3: Camera angle sweep with best qc ===")
    bc = best_params['qc']
    count = 0
    for theta in [x * 0.3 for x in range(-10, 11)]:
        for phi in [x * 0.2 for x in range(-7, 8)]:
            try_render(f"p3_{count}", bc, theta=theta, phi=phi)
            count += 1

    print(f"\nPhase 3: {count} renders. Best IoU = {best_score:.4f}")

    # ═══════════════════════════════════════════════════════════
    # Phase 4: Final refinement — qc + camera together
    # ═══════════════════════════════════════════════════════════
    print(f"\n=== Phase 4: Joint refinement ===")
    bp = best_params
    bc = bp['qc']
    count = 0
    for da in [-0.02, 0, 0.02]:
        for db in [-0.02, 0, 0.02]:
            for dt in [-0.15, 0, 0.15]:
                for dp in [-0.1, 0, 0.1]:
                    qc = (bc[0]+da, bc[1]+db, bc[2], bc[3])
                    try_render(f"p4_{count}", qc,
                               theta=bp['theta']+dt, phi=bp['phi']+dp)
                    count += 1

    # ═══════════════════════════════════════════════════════════
    # Result
    # ═══════════════════════════════════════════════════════════
    bp = best_params
    print(f"\n{'='*60}")
    print(f"BEST MATCH (IoU = {bp['score']:.4f}):")
    print(f"  --quat --qc {bp['qc'][0]:.4f},{bp['qc'][1]:.4f},{bp['qc'][2]:.4f},{bp['qc'][3]:.4f}")
    print(f"  --cam-theta {bp['theta']:.3f} --cam-phi {bp['phi']:.3f} --cam-dist {bp['dist']:.1f}")
    print(f"\nFull render command:")
    print(f"  {exe_path} --quat"
          f" --qc {bp['qc'][0]:.4f},{bp['qc'][1]:.4f},{bp['qc'][2]:.4f},{bp['qc'][3]:.4f}"
          f" --cam-theta {bp['theta']:.3f} --cam-phi {bp['phi']:.3f}"
          f" --palette 1 --iter 80 --steps 200"
          f" --save best_match.bmp --width 1024 --height 1024")


if __name__ == '__main__':
    main()
