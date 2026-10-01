# Makes data/moon_stars.bin, the stars the Moon passes in front of, from the
# Tycho-2 catalogue (Hog et al. 2000; VizieR I/259) and its first supplement
# (the bright stars Tycho-2 leaves out).
#
# The CSV files come from VizieR's TAP service, for the band of sky within
# 7.5 degrees of the ecliptic (the Moon never leaves it), e.g. for each
# 60-degree slice of right ascension:
#
#   SELECT RAmdeg,DEmdeg,"RA(ICRS)","DE(ICRS)",pmRA,pmDE,BTmag,VTmag
#   FROM "I/259/tyc2" WHERE "RA(ICRS)">=0 AND "RA(ICRS)"<60 AND
#   ABS(DEGREES(ASIN(SIN(RADIANS("DE(ICRS)"))*0.917482
#       -COS(RADIANS("DE(ICRS)"))*0.397777*SIN(RADIANS("RA(ICRS)")))))<7.5
#
#   SELECT "RA(ICRS)","DE(ICRS)",pmRA,pmDE,BTmag,VTmag,HIP FROM "I/259/suppl_1" WHERE (the same band)
#
# Usage: make_stars.py <folder with tyc2_*.csv and suppl.csv> <out.bin>
#
# The file: "MOONSTAR", a uint32 count, then for each star (sorted by right
# ascension) 12 bytes, little-endian: uint32 right ascension (a full turn is
# 2^32), int32 declination (90 degrees is 2^31 - 1), int16 V magnitude in
# thousandths, int8 B-V in fiftieths, and a zero byte.  Positions are ICRS,
# moved by the stars' proper motions to the epoch below.

import csv, glob, math, os, struct, sys

EPOCH = 2026.75
FAINTEST = 12.5

def number(text):
    text = text.strip()
    return float(text) if text else None

def star(ra, dec, pm_ra, pm_dec, years, bt, vt):
    if pm_ra is not None and pm_dec is not None:
        dec2 = dec + pm_dec * years / 3.6e6                      # mas/yr -> degrees
        ra2 = ra + pm_ra * years / 3.6e6 / max(0.01, math.cos(math.radians(dec)))
        ra, dec = ra2 % 360.0, dec2
    if vt is not None and bt is not None:
        v, bv = vt - 0.090 * (bt - vt), 0.850 * (bt - vt)       # Tycho to Johnson
    elif vt is not None:
        v, bv = vt, 0.6
    elif bt is not None:
        v, bv = bt - 0.6, 0.6
    else:
        return None
    if v > FAINTEST:
        return None
    return (ra, dec, v, max(-0.4, min(2.0, bv)))

def main(folder, out):
    stars = []
    for name in sorted(glob.glob(os.path.join(folder, 'tyc2_*.csv'))):
        with open(name, newline='') as f:
            rows = csv.reader(f)
            next(rows)
            for r in rows:
                ram, dem, ra, dec, pm_ra, pm_dec, bt, vt = [number(x) for x in r[:8]]
                if ram is not None and dem is not None:
                    s = star(ram, dem, pm_ra, pm_dec, EPOCH - 2000.0, bt, vt)   # mean position, epoch J2000
                else:
                    s = star(ra, dec, None, None, 0, bt, vt)                    # observed position only
                if s:
                    stars.append(s)
    with open(os.path.join(folder, 'suppl.csv'), newline='') as f:
        rows = csv.reader(f)
        next(rows)
        for r in rows:
            ra, dec, pm_ra, pm_dec, bt, vt = [number(x) for x in r[:6]]
            s = star(ra, dec, pm_ra, pm_dec, EPOCH - 1991.25, bt, vt)
            if s:
                stars.append(s)
    stars.sort()
    with open(out, 'wb') as f:
        f.write(b'MOONSTAR')
        f.write(struct.pack('<I', len(stars)))
        for ra, dec, v, bv in stars:
            f.write(struct.pack('<IihbB', int(ra / 360.0 * 4294967296.0) & 0xFFFFFFFF,
                                int(round(dec / 90.0 * 2147483647.0)), int(round(v * 1000)),
                                int(round(bv * 50)), 0))
    print(len(stars), 'stars; brightest V', min(s[2] for s in stars), '; file', os.path.getsize(out), 'bytes')

main(sys.argv[1], sys.argv[2])
