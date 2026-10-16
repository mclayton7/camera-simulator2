# Yale Bright Star Catalogue (BSC5)

- Source: http://tdc-www.harvard.edu/catalogs/BSC5 (fetched by `scripts/sky/fetch_bsc5.sh`; stored in git LFS)
- Licence: public domain (Yale University Observatory / SAO)
- Format (little-endian): 28-byte header of int32 STAR0, STAR1, STARN (negative = J2000), STNUM, MPROP, NMAG, NBENT,
  then |STARN| entries of NBENT (32) bytes: float32 XNO, float64 SRA0 (rad), float64 SDEC0 (rad), char[2] IS
  (spectral type), int16 MAG[1] (V x 100), float32 XRPM, float32 XDPM.
- B-V is not in the binary: taken from spectral class IS[0]: O -0.30, B -0.18, A 0.0, F 0.40, G 0.65, K 1.10, M 1.55, else 0.6.
