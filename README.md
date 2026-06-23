# imgpoison

A command-line tool that hides text inside images using steganography.
The image looks identical to the original — the hidden text is invisible to the eye
and survives JPEG compression.

**This is a learning project.** Built to understand steganography from first principles
and explore LLM security concepts. Not production-ready, not optimized, not hardened.

## Why

Studying how hidden payloads survive image processing pipelines — specifically
whether a prompt injection embedded in an image can reach a multimodal AI system
after JPEG compression, resizing, or re-encoding.

## Methods

**LSB** (Least Significant Bit) — flips the last bit of each pixel to encode data.
Fast and high capacity, but fragile: JPEG compression destroys the payload.
Use only with lossless formats (PNG).

**SS** (Spread Spectrum) — spreads the payload signal across hundreds of pixels
using a pseudorandom chip sequence. Survives JPEG compression. Requires a seed
(the key) to embed and extract.

## Build

    sudo apt install libjpeg-turbo8-dev
    make

Dependencies: zlib, libjpeg-turbo, libm. No libpng — PNG codec is hand-rolled.

## Usage

Embed:

    ./bin/imgpoison --embed --method ss --seed 42 --payload "text" input.png output.jpg

Extract:

    ./bin/imgpoison --extract --method ss --seed 42 output.jpg

LSB embed (PNG only):

    ./bin/imgpoison --embed --method lsb --payload "text" input.png output.png

Detect LSB embedding:

    ./bin/imgpoison --extract --detect input.png

Analyze — inspect an image for hidden payload without extracting it:

    ./bin/imgpoison --analyze --method ss --seed 42 output.jpg
    ./bin/imgpoison --analyze --method lsb input.png

SS analyze requires the same seed used at embed time. It prints the correlation
strength for each header bit and a verdict. LSB analyze runs a chi-square test
on pixel value distribution.

## Parameters

| Parameter    | Default | Notes                                        |
|--------------|---------|----------------------------------------------|
| --seed       | 42      | key for embed and extract — must match       |
| --strength   | 10      | signal strength. higher = robust, visible    |
| --method     | lsb     | lsb or ss                                    |

## STRENGTH trade-off

| STRENGTH | SNR vs JPEG noise | Visible? | Survives q95? |
|----------|-------------------|----------|---------------|
| 3        | ~96:1             | barely   | yes           |
| 10       | ~320:1            | slightly | yes           |
| 20       | ~1280:1           | yes      | yes           |

## How SS works

Each payload bit is hidden across two groups of pixels (block A and B).
Block A is shifted slightly brighter, block B slightly darker (or vice versa).
To extract: compare A and B — whichever is brighter encodes the bit value.

JPEG adds random noise to pixels, but because the noise hits A and B equally,
it cancels out when you subtract them. The signal survives. LSB does not survive
because JPEG randomizes the exact bit used to store the data.

The seed controls a pseudorandom sequence (chip) that scrambles which pixels
are used and how. Without the seed, extraction is not possible.

### Header protection

The payload is preceded by a small header: a fixed 16-bit magic marker
(`0xCAFE`) and the payload length. Both are written several times and read
back by majority vote.

The magic marker lets extraction tell a real payload apart from noise. With a
wrong seed, or an image that has been damaged past the point of recovery, the
marker will not come back and the tool reports a clear "magic mismatch" instead
of returning garbage. The length is repeated because a single flipped bit in it
would otherwise make the whole message unreadable.

## Tools

**diffmap** — visualize the difference between original and stego image.
Useful to verify the embedding is working correctly.

    python tests/diffmap.py img/original.png img/stego.jpg

What to look for:

- SS  → uniform salt-and-pepper noise across the whole image (signal spread everywhere)
- LSB → a small bright patch in the top-left corner (payload concentrated at the start)

**test_robustness** — tests payload survival through image transformations.

    python tests/test_robustness.py

The test runs two paths, because "does the payload survive" has two honest answers:

- **pipeline** — embeds straight to JPEG q95 (the real output format), then
  recompresses. Every case is a double compression, the way a real user would
  hit the image.
- **algorithm** — embeds to lossless PNG, then a single recompression. This
  isolates the spread-spectrum scheme's own robustness from the extra
  compression the JPEG output adds.

Results with default settings (STRENGTH=10, CHIP_SIZE=512):

| Transformation         | pipeline | algorithm |
|------------------------|----------|-----------|
| baseline               | PASS     | PASS      |
| recompress q90         | PASS     | PASS      |
| recompress q85         | PASS     | PASS      |
| recompress q75         | PASS     | PASS      |
| rotate 1 degree        | FAIL     | FAIL      |

The payload survives JPEG recompression down to q75 in both paths.

The rotation case fails on purpose, and not because of compression noise. The
seed picks fixed pixel positions; a 1-degree rotation moves every pixel a little
through interpolation, so those positions no longer hold the signal. This is
geometric desync, not a quality problem — no amount of extra strength or chip
size fixes it. Solving it would need synchronization (for example a pilot
template or a Fourier-Mellin transform), which is out of scope for this project.
The case is kept in the suite as a documented limit; it now fails cleanly with a
magic mismatch instead of a confusing result.

Note: the test writes its working images into `img/` (named `stego_robustness*`)
and they are ignored by git.

## Pending

- Shell script regression test (embed → extract round-trip)
- LLM security angle documentation
- Fix strength estimate in --analyze (currently inflated by JPEG noise)
- Rotation robustness (needs geometric synchronization)

## References

Marvel, Boncelet, Retter — Methodology of Spread-Spectrum Image
Steganography, ARL-TR-1698, 1998. apps.dtic.mil/sti/citations/ADA349102

Press, Teukolsky, Vetterling, Flannery — Numerical Recipes in C, 2nd ed., 1992.
LCG constants (multiplier 1664525, increment 1013904223) from chapter 7.

Knuth — The Art of Computer Programming, vol. 2, sec. 3.4.2.
Fisher-Yates shuffle implementation.

ITU-R BT.601 — luma coefficients reference.
en.wikipedia.org/wiki/Luma_(video)#Rec._601_luma_versus_Rec._709_luma

## Connect with me

[LinkedIn](https://www.linkedin.com/in/francescopl/) · [Kaggle](https://www.kaggle.com/francescopaolol)

