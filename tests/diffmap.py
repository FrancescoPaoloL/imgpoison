#!/usr/bin/env python3
# diffmap.py — visualize the difference between original and stego image
# usage: python3 diffmap.py original.png stego.jpg

import os
import sys
import numpy as np
from PIL import Image

out_path = os.path.join(os.path.dirname(os.path.abspath(sys.argv[2])), "diffmap.png")


# how much to amplify the differences — SS changes pixels by ~10 units,
# which is hard to see. multiplying by 10 makes them clearly visible.
AMPLIFY_FACTOR = 255 #10

# pixel values are 0-255 — cap there after amplification
MAX_PIXEL = 255

# load both images as RGB arrays of shape (height, width, 3)
# convert to float so subtraction does not wrap around (e.g. 2 - 5 = -3, not 253)
orig  = np.array(Image.open(sys.argv[1]).convert("RGB")).astype(float)
stego = np.array(Image.open(sys.argv[2]).convert("RGB")).astype(float)

# subtract — result is how much each pixel changed
# abs() because we care about the magnitude, not the direction
diff = np.abs(stego - orig)

# amplify so small differences are visible, then clamp back to 0-255
amplified = np.clip(diff * AMPLIFY_FACTOR, 0, MAX_PIXEL).astype(np.uint8)

# save the amplified diff as a grayscale-ish image
# LSB  → regular pattern (bits concentrated in LSB of each pixel)
# SS   → uniform salt-and-pepper noise (signal spread across all pixels)
Image.fromarray(amplified).save(out_path)

total_pixels = orig.shape[0] * orig.shape[1]
# collapse the 3 channels — a pixel is "changed" if any channel changed
changed = np.sum(np.any(diff > 0, axis=2))

print(f"output        : {out_path}  (differences amplified {AMPLIFY_FACTOR}x)")
print(f"image size    : {orig.shape[1]}x{orig.shape[0]} px  ({total_pixels} pixels total)")
print(f"pixels changed: {changed} / {total_pixels}  ({100*changed/total_pixels:.1f}%)")
print(f"max diff      : {diff.max():.1f}  (largest single pixel change)")
print(f"mean diff     : {diff.mean():.3f}  (average change — below ~5 is imperceptible)\n")


