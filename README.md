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
Block A is nudged slightly brighter, block B slightly darker (or vice versa).
To extract: compare A and B — whichever is brighter encodes the bit value.

JPEG adds random noise to pixels, but because the noise hits A and B equally,
it cancels out when you subtract them. The signal survives. LSB does not survive
because JPEG randomizes the exact bit used to store the data.

The seed controls a pseudorandom sequence (chip) that scrambles which pixels
are used and how. Without the seed, extraction is not possible.


## Pending

- Shell script regression test (embed → extract round-trip)
- LLM security angle documentationi

## References

Marvel, Boncelet, Retter — Methodology of Spread-Spectrum Image
Steganography, ARL-TR-1698, 1998. apps.dtic.mil/sti/citations/ADA349102

Press, Teukolsky, Vetterling, Flannery — Numerical Recipes in C, 2nd ed., 1992.
LCG constants (multiplier 1664525, increment 1013904223) from chapter 7.


## Connect with me
<p align="left">
<a href="https://www.linkedin.com/in/francescopl/" target="blank"><img align="center" src="https://raw.githubusercontent.com/rahuldkjain/github-profile-readme-generator/master/src/images/icons/Social/linked-in-alt.svg" alt="francescopaololezza" height="20" width="30" /></a>
<a href="https://www.kaggle.com/francescopaolol" target="blank"><img align="center" src="https://raw.githubusercontent.com/rahuldkjain/github-profile-readme-generator/master/src/images/icons/Social/kaggle.svg" alt="francescopaololezza" height="20" width="30" /></a>
</p>

