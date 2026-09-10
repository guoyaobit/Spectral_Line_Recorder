#!/usr/bin/env python3
"""
Plot integrated XX and YY spectra from SDFITS, split by noise source state.

Usage:
  python plot_sdfits_integrated_noise.py file.fits [subband_index]

Behavior:
  - DATA column: shape (NCHAN, NSUBBAND, NPOL)
  - "noise" column: 0=off, 1=on
  - Integrates XX and YY for noise=0、noise=1分开显示
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
    # fallback: TDIM
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
    raw = data["DATA"][-100:-1]
    noise_col = "noise" if "noise" in data.names else "NOISE"
    noise_flags = data[noise_col][-100:-1]

    nchan, nsub, npol = get_nch_nsub_npol(hdr)
    sb_index = int(sb_index)
    expected = nchan * nsub * npol

    xx_0 = np.zeros(nchan); xx_1 = np.zeros(nchan)
    yy_0 = np.zeros(nchan); yy_1 = np.zeros(nchan)
    n_0 = 0; n_1 = 0

    for i, row in enumerate(raw):
        arr = np.asarray(row).ravel()[:expected]
        try:
            arr = arr.reshape((nchan, nsub, npol, 1, 1))
        except Exception:
            continue
        spec = arr[:, sb_index, :, 0, 0]
        if spec.shape[1] < 2:
            continue
        xx = np.maximum(spec[:, 0].astype(float), EPS)
        yy = np.maximum(spec[:, 1].astype(float), EPS)
        if noise_flags[i] == 0:
            xx_0 += xx; yy_0 += yy; n_0 += 1
        elif noise_flags[i] == 1:
            xx_1 += xx; yy_1 += yy; n_1 += 1

    if n_0 > 0:
        xx_0 /= n_0; yy_0 /= n_0
    if n_1 > 0:
        xx_1 /= n_1; yy_1 /= n_1

    xx_0_db = 10.0 * np.log10(xx_0 + EPS)
    yy_0_db = 10.0 * np.log10(yy_0 + EPS)
    xx_1_db = 10.0 * np.log10(xx_1 + EPS)
    yy_1_db = 10.0 * np.log10(yy_1 + EPS)

    obsfreq = hdr.get('OBSFREQ')
    chan_bw = hdr.get('CHAN_BW')
    if obsfreq is not None and chan_bw is not None:
        obsfreq = float(obsfreq)
        chan_bw = float(chan_bw)
        freqs = obsfreq + (np.arange(nchan) - 0.5*(nchan-1)) * chan_bw
        x = freqs / 1e6
        xlabel = "Frequency (MHz)"
    else:
        x = np.arange(nchan)
        xlabel = "Channel"

    fig, axes = plt.subplots(2, 2, figsize=(16, 8), sharex=True, sharey=True)
    axes[0][0].plot(x, xx_0_db, color='C0'); axes[0][0].set_title('XX (noise off)')
    axes[1][0].plot(x, yy_0_db, color='C1'); axes[1][0].set_title('YY (noise off)')
    axes[0][1].plot(x, xx_1_db, color='C0'); axes[0][1].set_title('XX (noise on)')
    axes[1][1].plot(x, yy_1_db, color='C1'); axes[1][1].set_title('YY (noise on)')

    for axrow in axes:
        for ax in axrow:
            ax.set_xlabel(xlabel); ax.set_ylabel('Power (dB)'); ax.grid(True, alpha=0.3)
    plt.suptitle(f"{fname} subband {sb_index} | noise=0({n_0}), noise=1({n_1}) integrated")
    plt.tight_layout(rect=[0,0.03,1,0.96])
    plt.show()

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python plot_sdfits_integrated_noise.py file.fits [subband_index]")
        sys.exit(1)
    fname = sys.argv[1]
    sb = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    main(fname, sb)
