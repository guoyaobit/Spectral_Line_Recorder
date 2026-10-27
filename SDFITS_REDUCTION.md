# SDFITS ON/OFF spectral reduction

`make_class_onoff.py` reduces one source ON scan and one reference OFF
scan into a single CLASS-readable FITS spectrum. It supports exposure-weighted
integration, XX/YY selection, noise-diode calibration, LSRK radio velocities,
spectral smoothing, and separate Python frequency/velocity plots.

## Dependencies

Install the Python dependencies with the same interpreter that will run the
reduction:

```sh
python -m pip install -r requirements-sdfits.txt
```

The script requires NumPy, Astropy, and Matplotlib. GILDAS is not required to
write the FITS file or Python plots; it is only needed to inspect the result in
CLASS or create a CLASS-rendered figure.

## Input assumptions

Each input must contain a binary-table HDU with a `DATA` column. Dimensions are
read from `NCHAN`, `NSUBBAND`, and `NPOL`, or from the `TDIM` keyword attached
to `DATA`. The recorder stores XX and YY powers in the first two correlation
products. The remaining cross-products are not assumed to be standard Stokes
U and V.

Rows are exposure-normalized before they are combined:

- `NOISE=0` rows form the astronomical spectrum.
- `NOISE=1` rows are retained for noise-diode calibration.
- `NOISE=2` and unknown states are ignored.

Without a noise-diode temperature or an independent system temperature, the
result is the dimensionless position-switch ratio

```text
(P_on - P_off) / P_off
```

## Frequency and time metadata

New files use `FREQTYPE=RF`, with frequency values supplied by processing nodes
in Hz. Read their frequency metadata directly; do not supply LO/IF overrides
or add the LO a second time. The script does not use `FREQTYPE` to automatically
convert frequencies.

For historical IF files only, supply the actual receiver setup explicitly:

```sh
--lo-mhz LO --if-start-mhz IF_START --if-stop-mhz IF_STOP
```

For the default upper sideband, sky frequency is `LO + IF`. Use
`--sideband lower` when sky frequency decreases as IF increases. By default,
the IF limits are treated as band edges; use `--if-reference centers` only if
the supplied limits are the first and last channel centres.

Native files store the actual first channel centre in row `CRVAL3` with
`CRPIX3=1`, while `OBSFREQ` is the configured band midpoint. Keep the script's
default `--obsfreq-reference band-center` for this project's files:
`f[k] = OBSFREQ + (k - NCHAN/2) * CHAN_BW`. Use
`--obsfreq-reference middle-channels` only for external files whose OBSFREQ
is the midpoint of the first and last channel centres. Choosing the wrong
convention introduces a half-channel offset.

Directory timestamps are not read as observation metadata. When supplied,
`--date-obs` must be the true UTC start time; convert local observing times
to UTC first. Otherwise the script uses the earliest valid NOISE=0 row time.
For LSRK it estimates the ON-scan midpoint from the earliest row start and the
latest row start plus that row's exposure. If `--date-obs` is supplied, the
same estimated span is anchored at that time; total exposure is a fallback
only when row times are unavailable.

Row exposure is live integration time, not necessarily elapsed wall time.
For noise-switch integrations spanning gaps, the estimated end and midpoint
can therefore be inaccurate. Confirm the actual observing interval separately
when the LSRK correction requires higher time precision.

Astropy's bundled IERS table is used in offline mode. Refresh the local Astropy
IERS data before precision reductions when a newer Earth-orientation solution
is required.

## Polarization and temperature calibration

Select one connected receptor with `--polarization xx` or `--polarization yy`.
Use `--polarization stokes-i` only when both receptors contain valid power and
calibration data.

For noise-diode calibration, provide the effective injected temperature:

```sh
--tcal-k 8
```

or receptor-specific values:

```sh
--tcal-xx-k 8 --tcal-yy-k 8
```

The OFF-position scan determines a system temperature in every channel:

```text
Tsys[i] = Tcal * Pnoise_off[i] / (Pnoise_on[i] - Pnoise_off[i])
```

The source temperature is then

```text
Ta[i] = ((Psource_on[i] - Preference_off[i]) / Preference_off[i]) * Tsys[i]
```

The representative `TSYS` written to the FITS header is the median of all
finite channel-wise values. This number, and the final antenna-temperature
scale, vary linearly with the adopted `Tcal`. Calibration requires
`Pnoise_on > Pnoise_off > 0`; invalid channels are excluded from the
representative median.

Use `--tsys-k` only for an independently measured scalar system temperature.
It cannot be combined with a Tcal option.

## Smoothing and baseline treatment

A three-channel Hanning kernel `[0.25, 0.5, 0.25]` is applied by default.
Alternatives are:

```sh
--smooth none
--smooth boxcar --smooth-width 5
--smooth gaussian --smooth-sigma 2
```

Smoothing is not baseline subtraction. The script does not fit or remove a
constant, slope, polynomial, standing wave, or continuum level. ON/OFF
switching removes much of the instrumental background, but residual baselines
may remain. Perform a separate baseline fit using line-free channels before
measuring baseline-subtracted peak temperatures, integrated intensities, or
line widths. For molecules with hyperfine structure, exclude every real
component from the baseline fit.

## Output and plots

Every invocation writes exactly one FITS spectrum. The primary spectral axis
is a topocentric frequency offset understood by CLASS; `RESTFREQ`, `VELO-LSR`,
and the source/site/time metadata allow CLASS to show topocentric frequency
above LSRK radio velocity. The header records source name, coordinates,
observatory, polarization, calibration values, exposure, and smoothing history.

Save separate Python plots with:

```sh
--plot-prefix results/source --velocity-min -40 --velocity-max 5
```

This creates:

```text
results/source_frequency.png
results/source_velocity.png
```

The frequency plot is restricted to the channels corresponding to the selected
velocity interval. Plot titles are intentionally omitted. Use `--plot` to show
the figures interactively instead of, or in addition to, saving them.

To make a CLASS plot with velocity below and frequency above the spectrum:

```text
fits read output.fits
set unit v f
set mode x -40 5
plot
hardcopy output_velocity_frequency.png /device png white /overwrite
```

## Historical IF example: NH3(1,1), YY, noise diode

The following example uses an upper-sideband LO of 23294.4955 MHz. The
NH3(1,1) rest frequency 23694.4955 MHz corresponds to IF 400 MHz and therefore
falls in the 384-640 MHz IF recorder window. This is a historical setup,
not the supplied 29984-38080 MHz RF allocation. Do not apply its LO/IF override
to a new RF file.

```powershell
C:/ProgramData/miniconda3/python.exe make_class_onoff.py `
  source_on.sdfits reference_off.sdfits output.fits `
  --polarization yy --tcal-yy-k 8 `
  --lo-mhz 23294.4955 --if-start-mhz 384 --if-stop-mhz 640 `
  --rest-mhz 23694.4955 `
  --ra 00:36:47.51 --dec +63:29:02.1 --object 0033+636 `
  --line "NH3(1,1)" --telescope NSRT `
  --date-obs 2026-09-19T18:15:23 `
  --smooth hanning `
  --plot-prefix 0033+636_NH3_11_YY_Tcal8 `
  --velocity-min -40 --velocity-max 5
```

## Validation checklist

Before accepting a reduction, verify:

1. ON and OFF frequency axes are identical.
2. For RF files, the line is in the configured RF band and no LO is added.
   For historical IF files, the actual LO, sideband and IF limits are supplied.
3. `--date-obs` is UTC and the source coordinates use the intended frame.
4. Only physically connected polarization products are selected.
5. Noise-diode states have adequate and comparable integration time.
6. The peak velocity is plausible and the frequency/velocity axes agree.
7. `BUNIT=1` for uncalibrated ratios or `BUNIT=K` when a valid temperature
   calibration was applied.
8. Residual baselines are treated separately when quantitative line parameters
   are required.

The recorder's files are structurally compatible with the reduction when the
required table and data fields are present. They are not fully self-describing
if source coordinates, UTC time, absolute sky frequency, or LO metadata are
missing; provide those values explicitly rather than inferring them from a
directory name.
