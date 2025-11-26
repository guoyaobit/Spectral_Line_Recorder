import sys
import numpy as np
import matplotlib.pyplot as plt
from astropy.io import fits
from astropy.time import Time

filename_ON = sys.argv[1]
filename_OFF = sys.argv[2]

ON_X = np.zeros(65536)
ON_Y = np.zeros(65536)
hdul = fits.open(filename_ON)
data = hdul[1].data
for frame_idx in range(len(data)):
    row = data[frame_idx]
    ON_X += np.array(row['I'])
    ON_Y += np.array(row['Q'])

ON_X/=len(data)
ON_Y/=len(data)
hdul = fits.open(filename_OFF)
data = hdul[1].data
OFF_X = np.zeros(65536)
OFF_Y = np.zeros(65536)
for frame_idx in range(len(data)):
    row = data[frame_idx]
    OFF_X += np.array(row['I'])
    OFF_Y += np.array(row['Q'])

OFF_X/=len(data)
OFF_Y/=len(data)
spec_diff_X = (ON_X-OFF_X)/OFF_X
spec_diff_Y = (ON_Y-OFF_Y)/OFF_Y
fig, axs = plt.subplots(2, 1, sharex=True)
freqs = np.linspace(0, 256e6, 65536)
stokes_data = [spec_diff_X, spec_diff_Y]
labels = ['spec_diff_X', 'spec_diff_Y',]
for ax, data, label in zip(axs, stokes_data, labels):
    ax.plot(freqs, data, label=label)
    ax.set_ylabel(label)
    ax.grid(True)
axs[-1].set_xlabel("Frequency (Hz)")

plt.show()