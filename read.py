import sys
import numpy as np
import matplotlib.pyplot as plt

def main():
    if len(sys.argv) < 2:
        print(f"Usage: python {sys.argv[0]} <filename>")
        sys.exit(1)


    filename = sys.argv[1]

    # 读取 float32 数据
    data = np.fromfile(filename, dtype=np.float32)

    # 转成 (N, 4) 的二维数组
    data = data.reshape(-1, 4)
    N = data.shape[0]
    print(N);
    Fs = 32e6 
    freqs = np.linspace(-Fs/2, Fs/2, N, endpoint=False)
    # freqs = np.fft.fftfreq(N, d=1/Fs)  # [-Fs/2, Fs/2) after fftshift
    # 拆分 I, Q, U, V
    I, Q, U, V = data[:, 0], data[:, 1], data[:, 2], data[:, 3]
    fig, axs = plt.subplots(4, 1, sharex=True)
    stokes_data = [I, Q, U, V]
    labels = ['I', 'Q', 'U', 'V']

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
            # 找到最小值
        idx_min = np.argmin(data)
        freq_min = freqs[idx_min]
        val_min = data[idx_min]
        ax.scatter(freq_min, val_min, color='blue')
        ax.text(freq_min, val_min,
                f'Min ({freq_min/1e6:.2f} MHz, {val_min:.2f})',
                color='blue', fontsize=9,
                ha='left', va='top')

    axs[-1].set_xlabel("Frequency (Hz)")
    fig.suptitle("Stokes Parameters (I, Q, U, V)", fontsize=14)

    # plt.tight_layout(rect=[0, 0, 1, 0.97])
    plt.show()
    # plt.plot(freqs_shifted, np.log10(Q_shifted), label='Q')
    # plt.plot(freqs_shifted, U_shifted, label='U')
    # plt.plot(freqs_shifted, V_shifted, label='V')

    # plt.xlabel("Frequency (Hz)")  # 或 "Time (s)"
    # plt.ylabel("Amplitude / Power")
    # plt.title("Stokes Parameters")
    # plt.legend()
    # plt.grid(True)
    # plt.show()

if __name__ == "__main__":
    main()

