import sys
import numpy as np
import matplotlib.pyplot as plt
from astropy.io import fits
from astropy.time import Time
# 1. 打开 SDFITS 文件
# filename = "2184.000_2216.000MHz.fits"
filename = sys.argv[1]
hdul = fits.open(filename)
data = hdul[1].data
frame_idx = len(data) - 1
#frame_idx = 0
row = data[frame_idx]
# row_time = time[last_frame_idx]
# for frame_idx, row in enumerate(data):
time_sec = float(row['TIME'])
t = Time(time_sec, format='unix', scale='utc')
# freqs = np.array(row['FREQ'])
freqs = np.linspace(0, 256e6, 65536)
AA = np.array(row['I'])
BB = np.array(row['Q'])
AB_star = np.array(row['U'])
A_starB = np.array(row['V'])
#print(freqs[0],freqs[-1])
I_log = np.log10(AA + 1e-10)
Q_log = np.log10(BB+1e-10)
fig, axs = plt.subplots(2, 1, sharex=True)
stokes_data = [I_log, Q_log]
labels = ['I(dB)', 'Q_log', 'U', 'V']
for ax, data, label in zip(axs, stokes_data, labels):
    ax.plot(freqs, data, label=label)
    ax.set_ylabel(label)
    ax.grid(True)
    
    # 找到最大值并标记
    idx_max = np.argmax(data)
    freq_max = freqs[idx_max]
    val_max = data[idx_max]
    ax.scatter(freq_max, val_max, color='red')
    
    # 标注横纵坐标（MHz 显示更直观）
    ax.text(freq_max, val_max,
            f'({freq_max/1e6:.2f} MHz, {val_max:.2f})',
            color='red', fontsize=9,
            ha='left', va='bottom')
#     找到最小值
    idx_min = np.argmin(data)
    freq_min = freqs[idx_min]
    val_min = data[idx_min]
    ax.scatter(freq_min, val_min, color='blue')
    ax.text(freq_min, val_min,
        f'Min ({freq_min/1e6:.2f} MHz, {val_min:.2f})',
        color='blue', fontsize=9,
        ha='left', va='top')
axs[-1].set_xlabel("Frequency (Hz)")    
fig.suptitle(f"Frame {frame_idx} — UTC: {t.isot}")
plt.show()
hdul.close()
