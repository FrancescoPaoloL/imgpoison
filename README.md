# imgpoison

A small C project that explores image steganography and robust signal embedding.

The project implements LSB and Spread Spectrum steganography, with PNG and JPEG
support, texture-aware embedding, payload redundancy, image analysis and
robustness tests.

This is a learning project. The goal is to understand the algorithms by
implementing them directly in C rather than relying on high-level image
processing libraries.


## Why

Studying how hidden payloads survive image processing pipelines.

The project started from a simple question:

Can a small signal be hidden inside an image and still be recovered after the
image has been modified?

The experiments focus on JPEG compression, pseudorandom pixel distribution,
signal correlation, redundancy and texture-aware embedding.

The project also explores the limits of this approach. JPEG recompression can
be handled, while geometric transformations such as rotation currently break
the spatial synchronization between embedding and extraction.


## Methods

**LSB** — replaces the least significant bit of selected pixels. Simple and
high-capacity, but fragile under JPEG compression and easy to detect.

**Spread Spectrum (SS)** — spreads a weak signal across many pixels using a
seeded pseudorandom sequence. More resistant to compression, but requires a
seed and has lower capacity.

**Texture mask** — weights the Spread Spectrum signal according to local image
structure. The automatic mask is computed from image luminance, Sobel gradients
and a structure tensor. This makes the embedding stronger in textured areas
and weaker in flat areas.

**Payload redundancy** — repeats payload bits and uses majority voting during
extraction. This improves robustness at the cost of payload capacity.


## Build

Requires:

    gcc
    zlib
    libjpeg

Build:

    make

The executable is created as:

    bin/imgpoison

PNG support is a small encoder/decoder written directly against zlib, not
libpng.


## Usage

### LSB

Embed:

    ./bin/imgpoison --embed --method lsb \
        --payload "hello" input.png output.png

Extract:

    ./bin/imgpoison --extract --method lsb \
        output.png

### Spread Spectrum

Embed:

    ./bin/imgpoison --embed --method ss --seed 42 --strength 10 \
        --payload "hello" input.png output.jpg

Extract:

    ./bin/imgpoison --extract --method ss --seed 42 \
        output.jpg

SS with an explicit texture mask:

    ./bin/imgpoison --embed --method ss --seed 42 --mask mask.bin \
        --payload "hello" input.png output.jpg

`mask.bin` is a flat binary file of float32 values, one per pixel in raster
order (width*height entries, not width*height*channels - the mask applies
once per pixel across all its channels), not an image file.

SS with an automatically generated texture mask:

    ./bin/imgpoison --embed --method ss --seed 42 --auto-mask 1.0 \
        --payload "hello" input.png output.jpg

`--auto-mask` takes the gamma value directly as its argument (see Texture
mask below) - it is not a bare flag.

Extraction with a mask (explicit or automatic) must use the same mask the
embedding used. The mask is public - it is recomputed from the received
image itself, not transmitted alongside the payload - so pass the same
`--mask`/`--auto-mask` on extract too:

    ./bin/imgpoison --extract --method ss --seed 42 --auto-mask 1.0 \
        output.jpg


## Parameters

| Parameter     | Default | Description                                       |
|---------------|---------|----------------------------------------------------|
| --seed        | 42      | seed used for embedding and extraction              |
| --strength    | 10      | signal strength. higher = robust, visible           |
| --method      | lsb     | lsb or ss                                           |
| --mask        |         | explicit texture mask file (flat float32, per pixel)|
| --auto-mask   |         | generate a texture mask automatically, takes gamma  |


## Texture mask

The automatic texture mask is computed from the image luminance.

    image
      ↓
    luminance
      ↓
    Sobel gradients
      ↓
    structure tensor
      ↓
    texture measure
      ↓
    normalization
      ↓
    gamma
      ↓
    mask

Sobel gradients measure local changes in luminance. The structure tensor uses
these gradients to estimate local image structure and texture.

The resulting mask weights the Spread Spectrum signal. Textured areas receive
more signal than flat areas.

The mask is normalized before being applied, so changing the texture weighting
does not simply increase the overall signal energy.


## STRENGTH trade-off

Higher strength improves robustness but makes the embedding more visible.

Lower strength is harder to detect visually but is more easily destroyed by
compression.

There is no single optimal value. The useful range depends on the image,
compression level and payload.

The same trade-off applies to texture weighting. Increasing the contribution of
highly textured areas can improve perceptual behavior, but does not remove the
fundamental robustness limits of the algorithm.


## How SS works

Spread Spectrum embeds a weak signal over many pixels.

For each payload bit, the encoder generates a pseudorandom chip sequence from
the seed. The chip sequence is multiplied by the signal strength and added to
selected image samples.

The decoder generates the same sequence from the same seed and computes the
correlation between the received signal and the expected chip sequence.

Positive correlation means one bit value, negative correlation means the other.

The pixel positions are distributed using a seeded pseudorandom permutation.
The current implementation uses an LCG and a Fisher-Yates shuffle. The shuffle
avoids the modulo bias introduced by a naive `random() % n` selection.

The seed is used for reproducible synchronization. It is not a cryptographic
key and the current PRNG is not cryptographically secure.


### Header protection

The payload header is protected separately from the payload data.

This allows the decoder to reject corrupted or incorrectly synchronized data
before attempting to interpret the payload.


### Payload redundancy

Payload bits can be repeated before embedding. During extraction, the repeated
values are combined using majority voting.

This improves tolerance to corrupted observations at the cost of payload
capacity.


## Tools

### `--analyze`

Provides simple statistical analysis of the image.

For Spread Spectrum it can be used to inspect the correlation of the embedded
signal.

For LSB it provides statistical information useful for studying the effect of
LSB embedding.

### `--bitacc`

Raw per-bit accuracy against a known payload, bypassing the magic marker check
and majority voting that the normal extract path uses.

Normal extraction is all-or-nothing: the magic marker either matches or the
whole extraction is rejected, and majority voting collapses repeated bits into
a single decision. That makes robustness look binary - an attack either barely
dents the payload or destroys it completely, with no visibility into how much
margin is actually left. `--bitacc` reports the fraction of individual bit
decisions that are still correct, so degradation shows up as a curve instead
of a cliff.

    ./bin/imgpoison --bitacc --payload "hello" --seed 42 --auto-mask 1.0 \
        attacked.jpg

### `diffmap.py`

Visualizes the pixel differences between an original image and a modified
image.

Typical result:

    LSB → changes concentrated where the payload is written
    SS  → a low-amplitude distributed pattern across the image
    SS + --auto-mask → the pattern concentrates where the texture mask found
                        high texture, instead of spreading uniformly

This is useful for comparing LSB, Spread Spectrum and texture-aware embedding.


## Robustness tests

Run:

    python3 tests/test_robustness.py

The tests currently cover:

    baseline
    JPEG q90
    JPEG q85
    JPEG q75
    rotation by 1 degree

The tests are split into three paths.

Pipeline:

    embed → JPEG → JPEG → extract

Algorithm:

    embed → PNG → JPEG → extract

Auto-mask:

    embed (--auto-mask) → PNG → JPEG → extract (--auto-mask)

The JPEG tests measure how well the embedded signal survives image degradation.
The auto-mask path checks that concentrating the payload in high-texture
regions doesn't cost robustness compared to uniform strength - it is not a
perceptual-quality comparison, that is a separate question.

Rotation is a different problem. A geometric transformation changes the
position of the pixels used during embedding. The current implementation does
not perform geometric synchronization, so the rotation test is expected to fail.


### Current results

    baseline               PASS
    recompress q90         PASS
    recompress q85         PASS
    recompress q75         PASS
    rotate 1 degree        FAIL

JPEG recompression currently survives down to q75 in the tested image and
configuration, on all three paths (pipeline, algorithm, auto-mask).


## Limitations

The current SS implementation is robust to the tested JPEG recompression
levels, but does not handle geometric transformations such as rotation.

The PRNG is not cryptographically secure.

The texture mask describes local image structure. It does not understand the
semantic content of the image.

The project is intended for learning and experimentation, not production use.


## Pending

* Geometric synchronization
* Better payload capacity estimation
* More robustness tests
* Better statistical analysis

## References
* Marvel, Boncelet, Retter — Methodology of Spread-Spectrum Image Steganography, ARL-TR-1698, 1998. apps.dtic.mil/sti/citations/ADA349102
* Press, Teukolsky, Vetterling, Flannery — Numerical Recipes in C, 2nd ed., 1992. LCG constants (multiplier 1664525, increment 1013904223) from chapter 7.
* Knuth — The Art of Computer Programming, vol. 2, sec. 3.4.2. Fisher-Yates shuffle implementation.
* ITU-R BT.601 — luma coefficients reference. en.wikipedia.org/wiki/Luma_(video)#Rec._601_luma_versus_Rec._709_luma


## License
MIT


## Connect with me
[LinkedIn](https://www.linkedin.com/in/francescopl/) · [Kaggle](https://www.kaggle.com/francescopaolol)

