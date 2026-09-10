#!/usr/bin/env python3
"""
Plot the integrated XX and YY correlation products from SDFITS files.

Usage:
  python plot_sdfits_integrated.py file.fits [subband_index]

Behavior:
  - Reads all rows of the DATA column, reshapes according to header keywords (NCHAN, NSUBBAND, NPOL)
    or TDIM if needed.
  - Extracts the chosen subband (default 0).
  - Integrates XX and YY across all rows (time dimension).
  - Plots integrated XX and YY (in dB) in a 1x2 figure.
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
    noise = data['noise']
    print(noise)
    nchan, nsub, npol = get_nch_nsub_npol(hdr)
    if npol < 2:
        print("Warning: NPOL < 2, expected at least XX and YY")

    # choose subband index bounds check
    sb_index = int(sb_index)
    if sb_index < 0 or sb_index >= nsub:
        raise ValueError(f"subband index {sb_index} out of range [0, {nsub-1}]")

    expected = nchan * nsub * npol
    
    # Initialize integrated arrays
    xx_integrated = np.zeros(nchan, dtype=np.float64)
    yy_integrated = np.zeros(nchan, dtype=np.float64)
    
    # Process each row (time integration)
    num_rows = len(raw)
    print(f"Processing {num_rows} rows...")
    
    for row_idx, row in enumerate(raw[-10:-1]):
        print(row_idx)
        arr = np.asarray(row)
        
        if arr.size < expected:
            print(f"Warning: Row {row_idx} size {arr.size} smaller than expected {expected}, skipping")
            continue
        
        # Only use first expected elements in case of extra padding
        arr = arr.ravel()[:expected]
        
        # reshape assuming TDIM order: (NCHAN, NSUBBAND, NPOL, 1, 1)
        try:
            arr = arr.reshape((nchan, nsub, npol, 1, 1))
        except Exception as ex:
            print(f"Warning: Unable to reshape row {row_idx}: {str(ex)}, skipping")
            continue
        
        # extract (NCHAN, NPOL) for chosen subband
        spec = arr[:, sb_index, :, 0, 0]  # shape (NCHAN, NPOL)
        
        # Ensure we have at least indices 0..1 for XX and YY
        if spec.shape[1] < 2:
            print(f"Warning: Row {row_idx} has less than 2 polarizations, skipping")
            continue
        
        # mapping:
        # 0: XX, 1: YY
        xx = spec[:, 0].astype(float)
        yy = spec[:, 1].astype(float)
        
        # Convert to power (10*log10)
        xx_power = np.maximum(xx, EPS)
        yy_power = np.maximum(yy, EPS)
        
        # Accumulate linear power (not dB)
        xx_integrated += xx_power
        yy_integrated += yy_power
    
    # Average over all rows
    xx_integrated /= num_rows
    yy_integrated /= num_rows
    
    # Convert to dB
    xx_db = 10.0 * np.log10(xx_integrated)
    yy_db = 10.0 * np.log10(yy_integrated)
    
    # Frequency axis if available
    obsfreq = hdr.get('OBSFREQ')  # Hz or None
    chan_bw = hdr.get('CHAN_BW')  # Hz per channel (or channel spacing)
    
    if obsfreq is not None and chan_bw is not None:
        obsfreq = float(obsfreq)
        chan_bw = float(chan_bw)
        freqs = obsfreq + (np.arange(nchan) - 0.5*(nchan-1)) * chan_bw
        x = freqs / 1e6  # Convert to MHz for display
        xlabel = "Frequency (MHz)"
    else:
        x = np.arange(nchan)
        xlabel = "Channel"

    # Plotting 1x2
    fig, axes = plt.subplots(1, 2, figsize=(14, 5), sharex=True, sharey=True)

    axes[0].plot(x, xx_db, color='C0', linewidth=1.5)
    axes[0].set_title('XX (Integrated)', fontsize=12, fontweight='bold')
    axes[0].set_ylabel('Power (dB)', fontsize=11)
    axes[0].set_xlabel(xlabel, fontsize=11)
    axes[0].grid(True, alpha=0.3)

    axes[1].plot(x, yy_db, color='C1', linewidth=1.5)
    axes[1].set_title('YY (Integrated)', fontsize=12, fontweight='bold')
    axes[1].set_xlabel(xlabel, fontsize=11)
    axes[1].grid(True, alpha=0.3)

    plt.suptitle(f"{fname}  (subband {sb_index}, {num_rows} rows integrated)", 
                 fontsize=13, fontweight='bold')
    plt.tight_layout(rect=[0, 0.03, 1, 0.97])
    plt.show()

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python plot_sdfits_integrated.py file.fits [subband_index]")
        sys.exit(1)
    fname = sys.argv[1]
    sb = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    main(fname, sb)

