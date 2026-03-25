# imgpoison

Steganography tool — hides text payloads inside images.

## Methods

**LSB** (Least Significant Bit) — fast, high capacity, dies after JPEG compression.

**SS** (Spread Spectrum) — survives JPEG compression, requires seed as key.

## Usage
```bash
# embed
./bin/imgpoison --embed --method ss --seed 42 --payload "text" input.png output.jpg

# extract
./bin/imgpoison --extract --method ss --seed 42 output.jpg
```

## Build
```bash
sudo apt install libjpeg-turbo8-dev
make
```

Dependencies: zlib, libjpeg-turbo, libm.

## Status

Work in progress.

- [ ] STRENGTH as CLI parameter
- [ ] README: algorithm explanation, STRENGTH vs robustness trade-off
- [ ] Tests
- [ ] LLM security angle documentation

## References

Marvel, Boncelet, Retter — *Methodology of Spread-Spectrum Image Steganography*,
ARL-TR-1698, 1998. <apps.dtic.mil/sti/citations/ADA349102>
