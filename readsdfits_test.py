#!/usr/bin/env python3
"""
Plot the four correlation products stored in SDFITS as:
  0: XX, 1: YY, 2: XY, 3: YX

Usage:
  python plot_sdfits.py file.fits [subband_index]

Behavior:
  - Reads the last row of the DATA column, reshapes according to header keywords (NCHAN, NSUBBAND, NPOL)
    or TDIM if needed.
  - Extracts the chosen subband (default 0).
  - Plots XX, YY, |XY|, |YX| (in dB) in a 2x2 figure. If XY/YX are real, magnitude = abs(real).
"""
import sys
import ast
import numpy as np
import matplotlib.pyplot as plt
from astropy.io import fits

EPS = 1e-12

def parse_tdim(tdim_str):
    try:
        tup = ast.literal_eval(tdim_str)
        return tuple(int(x) for x in tup)
    except Exception:
        return None

def get_nch_nsub_npol(hdr):
    nchan = hdr.get('NCHAN')
    nsub = hdr.get('NSUBBAND')
    npol = hdr.get('NPOL')
    if nchan is not None and nsub is not None and npol is not None:
        return int(nchan), int(nsub), int(npol)

    # fallback: try to find a TDIM entry
    tdim_val = None
    for key in hdr:
        if key.startswith('TDIM'):
            tdim_val = hdr.get(key)
            if tdim_val:
                break
    if tdim_val:
        tdim = parse_tdim(tdim_val)
        if tdim and len(tdim) >= 3:
            nchan = nchan or int(tdim[0])
            nsub = nsub or int(tdim[1])
            npol = npol or int(tdim[2])
            return int(nchan), int(nsub), int(npol)
    raise RuntimeError("Cannot determine NCHAN/NSUBBAND/NPOL from header")

def main(fname, sb_index=0):
    hdul = fits.open(fname)
    hdr = hdul[1].header
    data = hdul[1].data
    raw = data["DATA"]

    nchan, nsub, npol = get_nch_nsub_npol(hdr)
    if npol < 4:
        print("Warning: NPOL < 4, expected 4 correlation products (XX,YY,XY,YX)")

    # choose subband index bounds check
    sb_index = int(sb_index)
    if sb_index < 0 or sb_index >= nsub:
        raise ValueError(f"subband index {sb_index} out of range [0, {nsub-1}]")

    # take last row
    row = raw[-1]
    arr = np.asarray(row)

    expected = nchan * nsub * npol
    if arr.size < expected:
        raise RuntimeError(f"DATA size {arr.size} smaller than expected {expected}")
    # Only use first expected elements in case of extra padding
    arr = arr.ravel()[:expected]
    # reshape assuming TDIM order: (NCHAN, NSUBBAND, NPOL, 1, 1)
    try:
        arr = arr.reshape((nchan, nsub, npol, 1, 1))
    except Exception as ex:
        raise RuntimeError("Unable to reshape DATA to (NCHAN,NSUBBAND,NPOL,1,1): " + str(ex))

    # extract (NCHAN, NPOL) for chosen subband
    spec = arr[:, sb_index, :, 0, 0]  # shape (NCHAN, NPOL)

    # Ensure we have at least indices 0..3; pad if necessary
    if spec.shape[1] < 4:
        # pad missing pols with zeros
        pad_width = 4 - spec.shape[1]
        spec = np.pad(spec, ((0,0),(0,pad_width)), mode='constant', constant_values=0.0)

    # mapping:
    # 0: XX, 1: YY, 2: XY, 3: YX
    xx = spec[:, 0]
    yy = spec[:, 1]
    xy = spec[:, 2]
    yx = spec[:, 3]

    # If XY/YX appear to encode complex values as pairs, detect and reconstruct:
    # Some writers store complex as interleaved real/imag across expanded npol; this is heuristic.
    # If xy is float but we detect that next value pattern indicates complex, user should adapt writer.
    # For now, treat xy/yx as possibly complex dtype; compute magnitude accordingly.
    def to_magnitude_db(a):
        if np.iscomplexobj(a):
            mag = np.abs(a)
        else:
            mag = np.abs(a)  # if real, just abs
        return 20.0 * np.log10(mag + EPS)

    def to_power_db(a):
        # for auto-correlations (XX,YY) we treat as power and convert to 10*log10
        if np.iscomplexobj(a):
            val = np.real(a)  # prefer real part for power
        else:
            val = a
        return 10.0 * np.log10(np.maximum(val, EPS))

    xx_db = to_power_db(xx.astype(float))
    yy_db = to_power_db(yy.astype(float))
    xy_db = to_magnitude_db(xy)
    yx_db = to_magnitude_db(yx)

    # Frequency axis if available
    obsfreq = hdr.get('OBSFREQ')  # Hz or None
    chan_bw = hdr.get('CHAN_BW')  # Hz per channel (or channel spacing)
    obsfreq = 128e6
    if obsfreq is not None and chan_bw is not None:
        obsfreq = float(obsfreq)
        chan_bw = float(chan_bw)
        freqs = obsfreq + (np.arange(nchan) - 0.5*(nchan-1)) * chan_bw
        x = freqs
        xlabel = "Frequency (Hz)"
    else:
        x = np.arange(nchan)
        xlabel = "Channel"

    # Plotting 2x2
    fig, axes = plt.subplots(2,2, figsize=(12,8), sharex=True)
    ax = axes.ravel()

    ax[0].plot(x,xx_db, color='C0')
    ax[0].set_title('XX (auto-corr)')
    ax[0].set_ylabel('Amplitude (dB)')
    ax[0].grid(True)

    ax[1].plot(x,yy_db, color='C1')
    ax[1].set_title('YY (auto-corr)')
    ax[1].grid(True)

    ax[2].plot(x,xy_db, color='C2')
    ax[2].set_title('XY (cross mag)')
    ax[2].set_ylabel('Magnitude (dB)')
    ax[2].set_xlabel(xlabel)
    ax[2].grid(True)

    ax[3].plot(x,yx_db, color='C3')
    ax[3].set_title('YX (cross mag)')
    ax[3].set_xlabel(xlabel)
    ax[3].grid(True)

    plt.suptitle(f"{fname}  (row {len(raw)-1}, subband {sb_index})")
    plt.tight_layout(rect=[0, 0.03, 1, 0.97])
    plt.show()

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python plot_sdfits.py file.fits [subband_index]")
        sys.exit(1)
    fname = sys.argv[1]
    sb = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    main(fname, sb)
