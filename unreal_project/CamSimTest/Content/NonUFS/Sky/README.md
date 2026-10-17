# Yale Bright Star Catalogue (BSC5)

- Source: http://tdc-www.harvard.edu/catalogs/BSC5 (fetched by `scripts/sky/fetch_bsc5.sh`; stored in git LFS)
- Licence: public domain (Yale University Observatory / SAO)
- Format (little-endian): 28-byte header of int32 STAR0, STAR1, STARN (negative = J2000), STNUM, MPROP, NMAG, NBENT,
  then |STARN| entries of NBENT (32) bytes: float32 XNO, float64 SRA0 (rad), float64 SDEC0 (rad), char[2] IS
  (spectral type), int16 MAG[1] (V x 100), float32 XRPM, float32 XDPM.
- B-V is not in the binary: taken from spectral class IS[0]: O -0.30, B -0.18, A 0.0, F 0.40, G 0.65, K 1.10, M 1.55, else 0.6.

# Moon albedo map (`moon_albedo.pgm`)

- Source: NASA SVS CGI Moon Kit (ID 4720, Ernie Wright / Noah Petro), `lroc_color_16bit_srgb_4k.tif` (2025 colour map,
  4096 x 2048, 16-bit sRGB), adapted from the LROC WAC Hapke-normalised mosaic (643 / 566 / 415 nm as R / G / B).
  <https://svs.gsfc.nasa.gov/4720>. Rebuilt by `scripts/sky/fetch_moon_albedo.sh` (pinned URL and sha256s) with
  `scripts/sky/make_moon_albedo.py`; stored in git LFS.
- Licence: NASA imagery, public domain (US government work); credit "NASA's Scientific Visualization Studio" and
  NASA/GSFC/Arizona State University (LROC).
- Format: binary PGM (P5), 1024 x 512, 8-bit **linear** relative albedo (Rec. 709 luminance of the linearised colour
  map, 4 x 4 block mean, brightest texel 255). Equirectangular: longitude -180 .. 180 left to right (0, the mean
  sub-Earth meridian, at the centre), latitude +90 at the top (IAU mean Earth / polar axis frame).
- Read by `FMoonAlbedoMap` (`Environment/MoonSurface.h`) for the dome's moon disc and its normalisation, and by
  `scripts/sky/ephemeris.py` for `night_check.py` N3. Missing or an LFS pointer: one warning, a uniform disc.
