#!/usr/bin/env python3
"""Calibrate an ON/OFF SDFITS pair and write one CLASS-readable spectrum.

The recorder used by this project stores four correlation accumulators for each
channel.  The first two are XX and YY powers; the other two are cross-product
terms and are not standard Stokes U/V.  This program therefore forms Stokes I
from XX and YY only.

Rows with NOISE=0 form the astronomical spectrum; NOISE=1 rows are kept only
for noise-diode calibration and NOISE=2/unknown rows are ignored.  The result
is (ON - OFF) / OFF unless --tsys-k or a measured noise-diode Tcal is supplied,
in which case it is placed on an antenna-temperature scale in kelvin.  A
three-channel Hanning smooth is applied by default and can be changed or
disabled from the command line.
"""

from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass
from pathlib import Path
import warnings

import numpy as np
from astropy import units as u
from astropy.coordinates import EarthLocation, SkyCoord, SpectralCoord
from astropy.io import fits
from astropy.time import Time
from astropy.utils import iers


# Reduction hosts are often offline.  Avoid a long network timeout while still
# allowing Astropy to warn when its bundled Earth-orientation table is stale.
iers.conf.auto_download = False
iers.conf.iers_degraded_accuracy = "warn"
iers.earth_orientation_table.set(iers.IERS_A.open(iers.IERS_A_FILE))


@dataclass
class IntegratedSpectrum:
    xx: np.ndarray
    yy: np.ndarray
    cal_xx: np.ndarray | None
    cal_yy: np.ndarray | None
    frequency_mhz: np.ndarray
    start_mjd: float | None
    end_mjd: float | None
    rows_used: int
    exposure_s: float
    cal_rows_used: int
    cal_exposure_s: float


def parse_tdim(value: object) -> tuple[int, ...] | None:
    """Parse a FITS TDIM string without evaluating arbitrary code."""
    if value is None:
        return None
    try:
        dims = ast.literal_eval(str(value))
        return tuple(int(item) for item in dims)
    except (SyntaxError, ValueError, TypeError):
        return None


def find_spectrum_hdu(hdul: fits.HDUList) -> fits.BinTableHDU:
    for hdu in hdul[1:]:
        if isinstance(hdu, fits.BinTableHDU) and hdu.data is not None:
            names = {name.upper() for name in (hdu.columns.names or [])}
            if "DATA" in names:
                return hdu
    raise RuntimeError("No binary-table HDU containing a DATA column was found")


def spectral_dimensions(hdu: fits.BinTableHDU) -> tuple[int, int, int]:
    """Read dimensions, using only the TDIM belonging to the DATA column."""
    header = hdu.header
    values = (header.get("NCHAN"), header.get("NSUBBAND"), header.get("NPOL"))
    if all(value is not None for value in values):
        nchan, nsub, npol = (int(value) for value in values)
    else:
        names = [name.upper() for name in hdu.columns.names]
        data_column_number = names.index("DATA") + 1
        dims = parse_tdim(header.get(f"TDIM{data_column_number}"))
        if dims is None or len(dims) < 3:
            raise RuntimeError(
                "Cannot determine NCHAN/NSUBBAND/NPOL from the DATA-column header"
            )
        nchan, nsub, npol = dims[:3]

    if min(nchan, nsub, npol) <= 0:
        raise RuntimeError(
            f"Invalid spectral dimensions: NCHAN={nchan}, NSUBBAND={nsub}, NPOL={npol}"
        )
    return nchan, nsub, npol


def normalize_frequency_unit(
    obsfreq: float, chan_bw: float, unit_choice: str
) -> tuple[float, float]:
    if unit_choice == "hz" or (unit_choice == "auto" and abs(obsfreq) >= 1.0e7):
        return obsfreq / 1.0e6, chan_bw / 1.0e6
    if unit_choice in {"auto", "mhz"}:
        return obsfreq, chan_bw
    raise ValueError(f"Unknown header frequency unit: {unit_choice}")


def frequency_axis_mhz(
    header: fits.Header,
    nchan: int,
    *,
    lo_mhz: float | None,
    if_start_mhz: float | None,
    if_stop_mhz: float | None,
    sideband: str,
    if_reference: str,
    center_mhz: float | None,
    bandwidth_mhz: float | None,
    header_frequency_unit: str,
    obsfreq_reference: str,
) -> np.ndarray:
    """Return channel-centre frequencies in MHz."""
    manual_if = (lo_mhz, if_start_mhz, if_stop_mhz)
    if any(value is not None for value in manual_if):
        if not all(value is not None for value in manual_if):
            raise ValueError(
                "--lo-mhz, --if-start-mhz, and --if-stop-mhz must be supplied together"
            )
        if if_reference == "edges":
            step = (float(if_stop_mhz) - float(if_start_mhz)) / nchan
            if_mhz = float(if_start_mhz) + (np.arange(nchan) + 0.5) * step
        else:
            if_mhz = np.linspace(float(if_start_mhz), float(if_stop_mhz), nchan)
        sign = 1.0 if sideband == "upper" else -1.0
        return float(lo_mhz) + sign * if_mhz

    if center_mhz is not None and bandwidth_mhz is not None:
        channel_width = float(bandwidth_mhz) / nchan
        first_center = float(center_mhz) - 0.5 * float(bandwidth_mhz) + 0.5 * channel_width
        return first_center + np.arange(nchan) * channel_width

    header_obsfreq = header.get("OBSFREQ")
    obsfreq = center_mhz if center_mhz is not None else header_obsfreq
    chan_bw = header.get("CHAN_BW")
    if obsfreq is not None and chan_bw is not None:
        if center_mhz is None:
            obsfreq_mhz, chan_bw_mhz = normalize_frequency_unit(
                float(obsfreq), float(chan_bw), header_frequency_unit
            )
        else:
            # --center-mhz is always MHz, but CHAN_BW still uses the input
            # header's unit.  In auto mode use OBSFREQ to identify that unit.
            unit_probe = float(header_obsfreq) if header_obsfreq is not None else 1.0e9
            _, chan_bw_mhz = normalize_frequency_unit(
                unit_probe, float(chan_bw), header_frequency_unit
            )
            obsfreq_mhz = float(center_mhz)
        center_index = nchan / 2.0 if obsfreq_reference == "band-center" else 0.5 * (nchan - 1)
        return obsfreq_mhz + (np.arange(nchan) - center_index) * chan_bw_mhz

    if center_mhz is None or bandwidth_mhz is None:
        raise RuntimeError(
            "No usable OBSFREQ/CHAN_BW header. Supply either the LO/IF triplet or "
            "--center-mhz and --bandwidth-mhz."
        )

    raise RuntimeError("Internal error while constructing the frequency axis")


def row_times_mjd(table: fits.FITS_rec, row_index: int) -> tuple[float, float] | None:
    names = {name.upper(): name for name in table.names}
    if "TIME" not in names:
        return None
    start = float(table[names["TIME"]][row_index])
    if not np.isfinite(start):
        return None
    exposure = 0.0
    if "EXPOSURE" in names:
        exposure = float(table[names["EXPOSURE"]][row_index])
        if not np.isfinite(exposure) or exposure < 0.0:
            exposure = 0.0
    return start, start + exposure / 86400.0


def extract_project_layout(
    row: np.ndarray, nchan: int, nsub: int, npol: int, sb_index: int
) -> np.ndarray:
    """Decode the project's documented channel/subband/product byte order."""
    expected = nchan * nsub * npol
    flat = np.asarray(row).reshape(-1)
    if flat.size != expected:
        raise ValueError(f"DATA row has {flat.size} values; expected exactly {expected}")
    return flat.reshape(nchan, nsub, npol)[:, sb_index, :]


def integrate_sdfits(
    path: Path,
    sb_index: int,
    *,
    lo_mhz: float | None,
    if_start_mhz: float | None,
    if_stop_mhz: float | None,
    sideband: str,
    if_reference: str,
    center_mhz: float | None,
    bandwidth_mhz: float | None,
    header_frequency_unit: str,
    obsfreq_reference: str,
) -> IntegratedSpectrum:
    with fits.open(path, memmap=True) as hdul:
        hdu = find_spectrum_hdu(hdul)
        header = hdu.header.copy()
        table = hdu.data
        nchan, nsub, npol = spectral_dimensions(hdu)
        if not 0 <= sb_index < nsub:
            raise ValueError(f"Subband index {sb_index} is outside [0, {nsub - 1}]")
        if npol < 2:
            raise RuntimeError("At least two correlation products (XX and YY) are required")

        frequency = frequency_axis_mhz(
            header,
            nchan,
            lo_mhz=lo_mhz,
            if_start_mhz=if_start_mhz,
            if_stop_mhz=if_stop_mhz,
            sideband=sideband,
            if_reference=if_reference,
            center_mhz=center_mhz,
            bandwidth_mhz=bandwidth_mhz,
            header_frequency_unit=header_frequency_unit,
            obsfreq_reference=obsfreq_reference,
        )

        names = {name.upper(): name for name in table.names}
        data_name = names["DATA"]
        exposure_name = names.get("EXPOSURE")
        # Index 0 is the normal/noise-diode-OFF state; index 1 is diode ON.
        xx_sum = np.zeros((2, nchan), dtype=np.float64)
        yy_sum = np.zeros((2, nchan), dtype=np.float64)
        xx_weight = np.zeros((2, nchan), dtype=np.float64)
        yy_weight = np.zeros((2, nchan), dtype=np.float64)
        rows_used = np.zeros(2, dtype=np.int64)
        total_exposure = np.zeros(2, dtype=np.float64)
        has_exposure = exposure_name is not None
        noise_name = names.get("NOISE")
        start_mjd: float | None = None
        end_mjd: float | None = None

        for row_index, row in enumerate(table[data_name]):
            try:
                products = extract_project_layout(row, nchan, nsub, npol, sb_index)
            except ValueError as exc:
                warnings.warn(f"Skipping row {row_index} in {path}: {exc}")
                continue

            weight = 1.0
            if exposure_name is not None:
                weight = float(table[exposure_name][row_index])
            if not np.isfinite(weight) or weight <= 0.0:
                warnings.warn(f"Skipping row {row_index} in {path}: invalid exposure")
                continue

            noise_state = 0
            if noise_name is not None:
                try:
                    noise_state = int(table[noise_name][row_index])
                except (TypeError, ValueError, OverflowError):
                    warnings.warn(f"Skipping row {row_index} in {path}: invalid NOISE state")
                    continue
                if noise_state not in (0, 1):
                    # State 2 is BLANK in the Recorder protocol; unknown states
                    # must likewise never enter either power average.
                    continue

            xx = np.asarray(products[:, 0], dtype=np.float64)
            yy = np.asarray(products[:, 1], dtype=np.float64)
            valid_xx = np.isfinite(xx)
            valid_yy = np.isfinite(yy)
            # Recorder DATA values are integration sums, not per-second
            # averages.  Sum the accumulators and divide by total exposure;
            # multiplying by exposure here would weight long rows twice.
            xx_sum[noise_state, valid_xx] += xx[valid_xx]
            yy_sum[noise_state, valid_yy] += yy[valid_yy]
            xx_weight[noise_state, valid_xx] += weight
            yy_weight[noise_state, valid_yy] += weight
            rows_used[noise_state] += 1
            total_exposure[noise_state] += weight

            if noise_state == 0:
                time_range = row_times_mjd(table, row_index)
                if time_range is not None:
                    row_start, row_end = time_range
                    start_mjd = row_start if start_mjd is None else min(start_mjd, row_start)
                    end_mjd = row_end if end_mjd is None else max(end_mjd, row_end)

    if rows_used[0] == 0:
        state_note = " with NOISE=0" if noise_name is not None else ""
        raise RuntimeError(f"No valid spectra{state_note} found in {path}")

    xx_mean = np.full((2, nchan), np.nan, dtype=np.float64)
    yy_mean = np.full((2, nchan), np.nan, dtype=np.float64)
    np.divide(xx_sum, xx_weight, out=xx_mean, where=xx_weight > 0.0)
    np.divide(yy_sum, yy_weight, out=yy_mean, where=yy_weight > 0.0)
    return IntegratedSpectrum(
        xx=xx_mean[0],
        yy=yy_mean[0],
        cal_xx=xx_mean[1] if rows_used[1] > 0 else None,
        cal_yy=yy_mean[1] if rows_used[1] > 0 else None,
        frequency_mhz=frequency,
        start_mjd=start_mjd,
        end_mjd=end_mjd,
        rows_used=int(rows_used[0]),
        exposure_s=float(total_exposure[0]) if has_exposure else float("nan"),
        cal_rows_used=int(rows_used[1]),
        cal_exposure_s=float(total_exposure[1]) if has_exposure else float("nan"),
    )


def on_off_ratio(on: np.ndarray, off: np.ndarray) -> np.ndarray:
    result = np.full_like(on, np.nan, dtype=np.float64)
    valid = np.isfinite(on) & np.isfinite(off) & (off > 0.0)
    np.divide(on - off, off, out=result, where=valid)
    return result


def calibrated_polarization(
    on: IntegratedSpectrum, off: IntegratedSpectrum, selection: str
) -> np.ndarray:
    """Calibrate each receptor before combining them into Stokes I."""
    xx = on_off_ratio(on.xx, off.xx)
    yy = on_off_ratio(on.yy, off.yy)
    if selection == "xx":
        return xx
    if selection == "yy":
        return yy

    result = np.full_like(xx, np.nan, dtype=np.float64)
    both = np.isfinite(xx) & np.isfinite(yy)
    result[both] = 0.5 * (xx[both] + yy[both])
    return result


def diode_system_temperature(
    cal_off: np.ndarray, cal_on: np.ndarray | None, tcal_k: float
) -> np.ndarray:
    """Return channel-wise Tsys = Tcal * Poff / (Pon - Poff)."""
    if cal_on is None:
        raise RuntimeError("Noise-diode calibration requested, but no NOISE=1 rows were found")
    if not np.isfinite(tcal_k) or tcal_k <= 0.0:
        raise ValueError("Noise-diode Tcal must be finite and positive")

    denominator = cal_on - cal_off
    tsys = np.full_like(cal_off, np.nan, dtype=np.float64)
    valid = (
        np.isfinite(cal_off)
        & np.isfinite(cal_on)
        & (cal_off > 0.0)
        & (denominator > 0.0)
    )
    np.divide(tcal_k * cal_off, denominator, out=tsys, where=valid)
    return tsys


def noise_diode_temperature(
    on: IntegratedSpectrum,
    off: IntegratedSpectrum,
    selection: str,
    tcal_xx_k: float | None,
    tcal_yy_k: float | None,
) -> tuple[np.ndarray, np.ndarray]:
    """Apply position-switch calibration with OFF-scan diode-derived Tsys.

    Each receptor uses T_A = ((P_on - P_off) / P_off) * Tsys_off before
    the two calibrated receptor temperatures are averaged into Stokes I.
    """
    if selection == "xx":
        if tcal_xx_k is None:
            raise ValueError("XX calibration requires --tcal-k or --tcal-xx-k")
        tsys_xx = diode_system_temperature(off.xx, off.cal_xx, tcal_xx_k)
        temp_xx = on_off_ratio(on.xx, off.xx) * tsys_xx
        return temp_xx, tsys_xx
    if selection == "yy":
        if tcal_yy_k is None:
            raise ValueError("YY calibration requires --tcal-k or --tcal-yy-k")
        tsys_yy = diode_system_temperature(off.yy, off.cal_yy, tcal_yy_k)
        temp_yy = on_off_ratio(on.yy, off.yy) * tsys_yy
        return temp_yy, tsys_yy

    if tcal_xx_k is None or tcal_yy_k is None:
        raise ValueError("Stokes I calibration requires Tcal for both XX and YY")
    tsys_xx = diode_system_temperature(off.xx, off.cal_xx, tcal_xx_k)
    tsys_yy = diode_system_temperature(off.yy, off.cal_yy, tcal_yy_k)
    temp_xx = on_off_ratio(on.xx, off.xx) * tsys_xx
    temp_yy = on_off_ratio(on.yy, off.yy) * tsys_yy

    temperature = np.full_like(temp_xx, np.nan, dtype=np.float64)
    system_temperature = np.full_like(tsys_xx, np.nan, dtype=np.float64)
    valid_temp = np.isfinite(temp_xx) & np.isfinite(temp_yy)
    valid_tsys = np.isfinite(tsys_xx) & np.isfinite(tsys_yy)
    temperature[valid_temp] = 0.5 * (temp_xx[valid_temp] + temp_yy[valid_temp])
    system_temperature[valid_tsys] = 0.5 * (
        tsys_xx[valid_tsys] + tsys_yy[valid_tsys]
    )
    return temperature, system_temperature


def smoothing_kernel(method: str, width: int, sigma: float) -> np.ndarray:
    if method == "none":
        return np.ones(1, dtype=np.float64)
    if method == "hanning":
        return np.array([0.25, 0.5, 0.25], dtype=np.float64)
    if method == "boxcar":
        if width < 1 or width % 2 == 0:
            raise ValueError("--smooth-width must be a positive odd integer")
        return np.ones(width, dtype=np.float64) / width
    if method == "gaussian":
        if not np.isfinite(sigma) or sigma <= 0.0:
            raise ValueError("--smooth-sigma must be finite and positive")
        radius = max(1, int(np.ceil(4.0 * sigma)))
        channels = np.arange(-radius, radius + 1, dtype=np.float64)
        kernel = np.exp(-0.5 * (channels / sigma) ** 2)
        return kernel / np.sum(kernel)
    raise ValueError(f"Unknown smoothing method: {method}")


def smooth_spectrum(
    spectrum: np.ndarray, method: str, width: int, sigma: float
) -> tuple[np.ndarray, str]:
    """Smooth without allowing NaNs to contaminate neighbouring channels."""
    kernel = smoothing_kernel(method, width, sigma)
    if kernel.size == 1:
        return np.asarray(spectrum, dtype=np.float64).copy(), "none"

    values = np.asarray(spectrum, dtype=np.float64)
    if kernel.size > values.size:
        raise ValueError(
            f"Smoothing kernel has {kernel.size} channels but spectrum has only {values.size}"
        )
    finite = np.isfinite(values)
    numerator = np.convolve(np.where(finite, values, 0.0), kernel, mode="same")
    denominator = np.convolve(finite.astype(np.float64), kernel, mode="same")
    smoothed = np.full_like(values, np.nan, dtype=np.float64)
    np.divide(numerator, denominator, out=smoothed, where=denominator > 0.0)

    if method == "hanning":
        description = "Hanning 3-channel [0.25,0.5,0.25]"
    elif method == "boxcar":
        description = f"boxcar {width}-channel"
    else:
        description = f"Gaussian sigma={sigma:g} channel, kernel={kernel.size} channel"
    return smoothed, description


def observation_times(
    on: IntegratedSpectrum, date_obs: str | None
) -> tuple[Time, Time, Time]:
    """Return ON-scan start, midpoint, and end times.

    The celestial signal and its velocity correction belong to the ON scan;
    the OFF scan is a calibration reference and must not shift DATE-AVG.
    """
    if date_obs is not None:
        start = Time(date_obs, scale="utc")
        duration_days = 0.0
        if on.start_mjd is not None and on.end_mjd is not None:
            duration_days = max(0.0, on.end_mjd - on.start_mjd)
        elif np.isfinite(on.exposure_s):
            duration_days = max(0.0, on.exposure_s / 86400.0)
        end = start + duration_days * u.day
        midpoint = start + 0.5 * duration_days * u.day
        return start, midpoint, end
    if on.start_mjd is not None and on.end_mjd is not None:
        start = Time(on.start_mjd, format="mjd", scale="utc")
        end = Time(on.end_mjd, format="mjd", scale="utc")
        midpoint = Time(0.5 * (start.mjd + end.mjd), format="mjd", scale="utc")
        return start, midpoint, end
    raise RuntimeError("No valid TIME column; --date-obs is required")


def velocity_axis_lsrk(
    frequency_mhz: np.ndarray,
    rest_mhz: float,
    ra: str,
    dec: str,
    obstime: Time,
    location: EarthLocation,
) -> np.ndarray:
    observer = location.get_itrs(obstime=obstime)
    # A distant, barycentrically stationary direction is sufficient for a
    # spectral reference-frame correction and avoids implicit Astropy defaults.
    target = SkyCoord(
        ra,
        dec,
        unit=(u.hourangle, u.deg),
        frame="icrs",
        distance=1.0e6 * u.kpc,
        radial_velocity=0.0 * u.km / u.s,
    )
    observed = SpectralCoord(frequency_mhz * u.MHz, observer=observer, target=target)
    lsrk = observed.with_observer_stationary_relative_to("lsrk")
    return lsrk.to(
        u.km / u.s,
        doppler_convention="radio",
        doppler_rest=float(rest_mhz) * u.MHz,
    ).value


def write_class_fits(
    path: Path,
    spectrum: np.ndarray,
    frequency_mhz: np.ndarray,
    velocity_km_s: np.ndarray,
    *,
    rest_mhz: float,
    ra: str,
    dec: str,
    object_name: str,
    line_name: str,
    telescope: str,
    start_time: Time,
    midpoint: Time,
    location: EarthLocation,
    polarization: str,
    tsys_k: float | None,
    tcal_xx_k: float | None,
    tcal_yy_k: float | None,
    on_exposure_s: float,
    smoothing: str,
    calibration: str,
    on_path: Path,
    off_path: Path,
) -> None:
    spectrum = np.asarray(spectrum, dtype=np.float32)
    frequency_hz = np.asarray(frequency_mhz, dtype=np.float64) * 1.0e6
    velocity_m_s = np.asarray(velocity_km_s, dtype=np.float64) * 1000.0
    nchan = spectrum.size
    if frequency_hz.size != nchan or velocity_m_s.size != nchan:
        raise ValueError("Spectrum, frequency, and velocity axes must have equal lengths")
    if nchan < 2:
        raise ValueError("At least two channels are required")

    frequency_step = float(np.median(np.diff(frequency_hz)))
    velocity_step = float(np.median(np.diff(velocity_m_s)))
    velocity_model = velocity_m_s[0] + np.arange(nchan) * velocity_step
    residual = np.nanmax(np.abs(velocity_m_s - velocity_model))
    # Relativistic frame conversion can leave millimetre-per-second numerical
    # curvature across a wide band.  This is negligible compared with the
    # channel spacing but still reject departures above 0.01 m/s (or 1e-4 of
    # a channel, whichever is larger).
    if residual > max(1.0e-2, abs(velocity_step) * 1.0e-4):
        raise RuntimeError(
            f"LSRK velocity axis is not sufficiently linear (max residual {residual:g} m/s)"
        )

    sky = SkyCoord(ra, dec, unit=(u.hourangle, u.deg), frame="icrs")
    x_m, y_m, z_m = location.to_geocentric()
    stokes_code = {"stokes-i": 1.0, "xx": -5.0, "yy": -6.0}[polarization]

    hdu = fits.PrimaryHDU(spectrum.reshape(1, 1, 1, nchan))
    header = hdu.header
    header["BUNIT"] = ("K" if tsys_k is not None else "1", "Spectrum unit")
    header["OBJECT"] = (object_name, "Source name")
    header["LINE"] = (line_name, "Spectral line")
    header["TELESCOP"] = (telescope, "Telescope/backend")
    header["DATE-OBS"] = (start_time.utc.isot, "UTC observation start")
    header["MJD-OBS"] = (float(start_time.utc.mjd), "UTC observation start")
    header["DATE-AVG"] = (midpoint.utc.isot, "UTC observation midpoint")
    header["MJD-AVG"] = (float(midpoint.utc.mjd), "UTC observation midpoint")
    reduction_time = Time.now().utc
    header["DATE"] = (reduction_time.isot, "UTC FITS file creation time")
    header["DATE-RED"] = (reduction_time.isot, "UTC data reduction time for CLASS")
    header["TIMESYS"] = ("UTC", "Time scale")
    header["RADESYS"] = ("ICRS", "Celestial reference frame")
    if np.isfinite(on_exposure_s):
        header["OBSTIME"] = (float(on_exposure_s), "Total ON exposure in seconds")
    if tsys_k is not None:
        header["TSYS"] = (float(tsys_k), "Representative system temperature in kelvin")
    if tcal_xx_k is not None:
        header["TCALXX"] = (float(tcal_xx_k), "XX noise-diode temperature in kelvin")
    if tcal_yy_k is not None:
        header["TCALYY"] = (float(tcal_yy_k), "YY noise-diode temperature in kelvin")

    lon, lat, height = location.to_geodetic()
    header["SITELONG"] = (float(lon.to_value(u.deg)), "East longitude in degrees")
    header["SITELAT"] = (float(lat.to_value(u.deg)), "Latitude in degrees")
    header["SITEELEV"] = (float(height.to_value(u.m)), "Elevation in metres")
    header["OBSGEO-X"] = (float(x_m.to_value(u.m)), "ITRS geocentric X in metres")
    header["OBSGEO-Y"] = (float(y_m.to_value(u.m)), "ITRS geocentric Y in metres")
    header["OBSGEO-Z"] = (float(z_m.to_value(u.m)), "ITRS geocentric Z in metres")

    # CLASS expects this frequency axis as an offset from RESTFREQ.  VELO-LSR
    # supplies the observatory-to-LSRK correction at the reference channel,
    # allowing CLASS to draw topocentric frequency above LSRK radio velocity.
    rest_hz = float(rest_mhz) * 1.0e6
    reference_pixel = 1.0 + (rest_hz - float(frequency_hz[0])) / frequency_step
    velocity_at_reference = float(
        velocity_m_s[0] + (reference_pixel - 1.0) * velocity_step
    )
    header["CTYPE1"] = ("FREQ", "CLASS sky-frequency axis")
    header["CUNIT1"] = ("Hz", "Frequency unit")
    header["CRPIX1"] = (reference_pixel, "Reference channel")
    header["CRVAL1"] = (0.0, "Offset from RESTFREQ")
    header["CDELT1"] = (frequency_step, "Frequency increment")
    header["RESTFRQ"] = (rest_hz, "Rest frequency in Hz")
    header["RESTFREQ"] = (rest_hz, "CLASS rest frequency in Hz")
    header["SPECSYS"] = ("LSRK", "Spectral reference frame")
    header["SSYSOBS"] = ("TOPOCENT", "Observed spectral frame")
    header["VELDEF"] = ("RADI-LSR", "Radio velocity in LSRK")
    header["VELO-LSR"] = (
        velocity_at_reference,
        "LSRK velocity at reference channel",
    )
    header["DELTAV"] = (velocity_step, "Velocity spacing in m/s")
    header["OBSFREQ"] = (float(np.mean(frequency_hz)), "Mean topocentric frequency")
    header["OBSBW"] = (
        abs(float(np.median(np.diff(frequency_hz)))) * nchan,
        "Nominal bandwidth in Hz",
    )
    header["CTYPE2"] = ("RA---TAN", "Right ascension")
    header["CUNIT2"] = ("deg", "RA unit")
    header["CRPIX2"] = 1.0
    header["CRVAL2"] = float(sky.ra.deg)
    header["CDELT2"] = -1.0 / 3600.0
    header["CTYPE3"] = ("DEC--TAN", "Declination")
    header["CUNIT3"] = ("deg", "Dec unit")
    header["CRPIX3"] = 1.0
    header["CRVAL3"] = float(sky.dec.deg)
    header["CDELT3"] = 1.0 / 3600.0
    header["CTYPE4"] = ("STOKES", "Polarization")
    header["CRPIX4"] = 1.0
    header["CRVAL4"] = stokes_code
    header["CDELT4"] = 1.0
    header.add_history(f"ON input: {on_path.name}")
    header.add_history(f"OFF input: {off_path.name}")
    header.add_history(f"Calibration: {calibration}")
    header.add_history(f"Spectral smoothing: {smoothing}")

    fits.HDUList([hdu]).writeto(path, overwrite=True, output_verify="exception")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Make an exposure-normalized ON/OFF spectrum with an LSRK velocity axis."
    )
    parser.add_argument("on_fits", type=Path)
    parser.add_argument("off_fits", type=Path)
    parser.add_argument("output_fits", type=Path)
    parser.add_argument("--sb-index", type=int, default=0)
    parser.add_argument("--polarization", choices=("stokes-i", "xx", "yy"), default="stokes-i")
    calibration = parser.add_argument_group("temperature calibration")
    calibration.add_argument(
        "--tsys-k", type=float, help="Multiply (ON-OFF)/OFF by a known scalar Tsys [K]"
    )
    calibration.add_argument(
        "--tcal-k",
        type=float,
        help="Noise-diode temperature [K], used for both XX and YY",
    )
    calibration.add_argument(
        "--tcal-xx-k", type=float, help="XX noise-diode temperature [K]"
    )
    calibration.add_argument(
        "--tcal-yy-k", type=float, help="YY noise-diode temperature [K]"
    )
    smoothing = parser.add_argument_group("spectral smoothing")
    smoothing.add_argument(
        "--smooth",
        choices=("none", "hanning", "boxcar", "gaussian"),
        default="hanning",
        help="Smoothing method (default: hanning)",
    )
    smoothing.add_argument(
        "--smooth-width",
        type=int,
        default=5,
        help="Odd boxcar width in channels (default: 5)",
    )
    smoothing.add_argument(
        "--smooth-sigma",
        type=float,
        default=2.0,
        help="Gaussian sigma in channels (default: 2.0)",
    )

    frequency = parser.add_argument_group("frequency setup")
    frequency.add_argument("--lo-mhz", type=float)
    frequency.add_argument("--if-start-mhz", type=float)
    frequency.add_argument("--if-stop-mhz", type=float)
    frequency.add_argument("--sideband", choices=("upper", "lower"), default="upper")
    frequency.add_argument("--if-reference", choices=("edges", "centers"), default="edges")
    frequency.add_argument("--center-mhz", type=float)
    frequency.add_argument("--bandwidth-mhz", type=float)
    frequency.add_argument("--header-frequency-unit", choices=("auto", "hz", "mhz"), default="auto")
    frequency.add_argument(
        "--obsfreq-reference",
        choices=("band-center", "middle-channels"),
        default="band-center",
        help="This project's recorder writes OBSFREQ at the centre between band edges",
    )
    frequency.add_argument("--rest-mhz", type=float, default=22235.080)

    target = parser.add_argument_group("target and observatory")
    target.add_argument("--ra", default="02:27:03.866")
    target.add_argument("--dec", default="+61:52:24.82")
    target.add_argument("--object", default="W3(OH)")
    target.add_argument("--line", default="H2O-6(16)-5(23)")
    target.add_argument("--telescope", default="NSRT")
    target.add_argument(
        "--date-obs",
        help="True UTC start; when supplied, overrides the input TIME epoch",
    )
    target.add_argument("--site-lon-deg", type=float, default=87.10)
    target.add_argument("--site-lat-deg", type=float, default=43.28)
    target.add_argument("--site-height-m", type=float, default=2029.38)
    parser.add_argument("--plot", action="store_true", help="Display frequency and velocity plots")
    parser.add_argument(
        "--plot-prefix",
        type=Path,
        help="Save separate Python plots as PREFIX_frequency.png and PREFIX_velocity.png",
    )
    parser.add_argument("--velocity-min", type=float, help="Lower velocity plot limit [km/s]")
    parser.add_argument("--velocity-max", type=float, help="Upper velocity plot limit [km/s]")
    return parser


def main() -> None:
    args = build_parser().parse_args()
    common = dict(
        sb_index=args.sb_index,
        lo_mhz=args.lo_mhz,
        if_start_mhz=args.if_start_mhz,
        if_stop_mhz=args.if_stop_mhz,
        sideband=args.sideband,
        if_reference=args.if_reference,
        center_mhz=args.center_mhz,
        bandwidth_mhz=args.bandwidth_mhz,
        header_frequency_unit=args.header_frequency_unit,
        obsfreq_reference=args.obsfreq_reference,
    )
    on = integrate_sdfits(args.on_fits, **common)
    off = integrate_sdfits(args.off_fits, **common)
    if on.frequency_mhz.shape != off.frequency_mhz.shape or not np.allclose(
        on.frequency_mhz, off.frequency_mhz, rtol=0.0, atol=1.0e-9
    ):
        raise RuntimeError("ON and OFF frequency axes do not match")

    tcal_xx_k = args.tcal_xx_k if args.tcal_xx_k is not None else args.tcal_k
    tcal_yy_k = args.tcal_yy_k if args.tcal_yy_k is not None else args.tcal_k
    use_noise_diode = tcal_xx_k is not None or tcal_yy_k is not None
    if use_noise_diode and args.tsys_k is not None:
        raise ValueError("--tsys-k cannot be combined with noise-diode Tcal options")

    representative_tsys_k: float | None
    if use_noise_diode:
        unsmoothed_spectrum, channel_tsys = noise_diode_temperature(
            on,
            off,
            args.polarization,
            float(tcal_xx_k) if tcal_xx_k is not None else None,
            float(tcal_yy_k) if tcal_yy_k is not None else None,
        )
        finite_tsys = channel_tsys[np.isfinite(channel_tsys)]
        if finite_tsys.size == 0:
            raise RuntimeError(
                "No valid Tsys channels: require finite Pcal_on > Pcal_off > 0"
            )
        valid_fraction = finite_tsys.size / channel_tsys.size
        if valid_fraction < 0.9:
            warnings.warn(
                f"Only {valid_fraction:.1%} of channels have a valid diode-derived Tsys"
            )
        representative_tsys_k = float(np.median(finite_tsys))
        calibration_description = (
            "exposure-normalized noise diode; Tsys=Tcal*Poff/(Pcal-Poff), "
            "Ta=((Pon-Poff)/Poff)*Tsys_off"
        )
    else:
        unsmoothed_spectrum = calibrated_polarization(on, off, args.polarization)
        representative_tsys_k = args.tsys_k
        calibration_description = "exposure-normalized (ON-OFF)/OFF"

    if args.tsys_k is not None:
        if not np.isfinite(args.tsys_k) or args.tsys_k <= 0.0:
            raise ValueError("--tsys-k must be a finite positive value")
        unsmoothed_spectrum *= args.tsys_k
        calibration_description += f" multiplied by scalar Tsys={args.tsys_k:g} K"
    spectrum, smoothing_description = smooth_spectrum(
        unsmoothed_spectrum,
        args.smooth,
        args.smooth_width,
        args.smooth_sigma,
    )

    start, midpoint, _ = observation_times(on, args.date_obs)
    location = EarthLocation(
        lon=args.site_lon_deg * u.deg,
        lat=args.site_lat_deg * u.deg,
        height=args.site_height_m * u.m,
    )
    velocity = velocity_axis_lsrk(
        on.frequency_mhz,
        args.rest_mhz,
        args.ra,
        args.dec,
        midpoint,
        location,
    )
    write_class_fits(
        args.output_fits,
        spectrum,
        on.frequency_mhz,
        velocity,
        rest_mhz=args.rest_mhz,
        ra=args.ra,
        dec=args.dec,
        object_name=args.object,
        line_name=args.line,
        telescope=args.telescope,
        start_time=start,
        midpoint=midpoint,
        location=location,
        polarization=args.polarization,
        tsys_k=representative_tsys_k,
        tcal_xx_k=float(tcal_xx_k) if tcal_xx_k is not None else None,
        tcal_yy_k=float(tcal_yy_k) if tcal_yy_k is not None else None,
        on_exposure_s=on.exposure_s,
        smoothing=smoothing_description,
        calibration=calibration_description,
        on_path=args.on_fits,
        off_path=args.off_fits,
    )

    print(f"Wrote {args.output_fits}")
    print(
        f"ON rows/exposure:  noise off {on.rows_used} / {on.exposure_s:.6g} s; "
        f"noise on {on.cal_rows_used} / {on.cal_exposure_s:.6g} s"
    )
    print(
        f"OFF rows/exposure: noise off {off.rows_used} / {off.exposure_s:.6g} s; "
        f"noise on {off.cal_rows_used} / {off.cal_exposure_s:.6g} s"
    )
    if representative_tsys_k is not None:
        print(f"Representative Tsys: {representative_tsys_k:.6g} K")
    print(f"Frequency: {on.frequency_mhz[0]:.9f} to {on.frequency_mhz[-1]:.9f} MHz")
    print(f"Velocity:  {velocity[0]:.6f} to {velocity[-1]:.6f} km/s (LSRK, radio)")
    print(f"Smoothing: {smoothing_description}")

    if args.plot or args.plot_prefix is not None:
        import matplotlib.pyplot as plt

        velocity_mask = np.ones(velocity.size, dtype=bool)
        if args.velocity_min is not None:
            velocity_mask &= velocity >= args.velocity_min
        if args.velocity_max is not None:
            velocity_mask &= velocity <= args.velocity_max
        if not np.any(velocity_mask):
            raise ValueError("The requested velocity plot range contains no channels")

        peak_channel = np.flatnonzero(velocity_mask)[np.argmax(spectrum[velocity_mask])]
        ylabel = "K" if representative_tsys_k is not None else "Normalized (ON-OFF)/OFF"

        def make_axis_plot(
            x,
            xlabel,
            peak_x,
            peak_label,
            xlim=None,
            visible_mask=None,
        ):
            fig, axis = plt.subplots(figsize=(16, 9))
            axis.plot(x, spectrum, color="#1558B0", linewidth=1.5)
            axis.axvline(
                peak_x,
                color="red",
                linestyle="--",
                linewidth=1.2,
                label=peak_label,
            )
            if xlim is not None:
                axis.set_xlim(*xlim)
            if visible_mask is not None:
                visible_y = spectrum[visible_mask]
                visible_y = visible_y[np.isfinite(visible_y)]
                if visible_y.size:
                    y_min = float(np.min(visible_y))
                    y_max = float(np.max(visible_y))
                    y_span = y_max - y_min
                    padding = 0.08 * y_span if y_span > 0.0 else max(abs(y_max), 1.0) * 0.08
                    axis.set_ylim(y_min - padding, y_max + padding)
            axis.set_xlabel(xlabel)
            axis.set_ylabel(ylabel)
            axis.grid(alpha=0.3)
            axis.legend(loc="upper right")
            fig.tight_layout()
            return fig

        frequency_xlim = (
            float(np.min(on.frequency_mhz[velocity_mask])),
            float(np.max(on.frequency_mhz[velocity_mask])),
        )
        frequency_figure = make_axis_plot(
            on.frequency_mhz,
            "Topocentric frequency (MHz)",
            on.frequency_mhz[peak_channel],
            f"Peak {on.frequency_mhz[peak_channel]:.6f} MHz",
            frequency_xlim,
            velocity_mask,
        )
        velocity_xlim = None
        if args.velocity_min is not None or args.velocity_max is not None:
            velocity_xlim = (
                args.velocity_min if args.velocity_min is not None else float(np.min(velocity)),
                args.velocity_max if args.velocity_max is not None else float(np.max(velocity)),
            )
        velocity_figure = make_axis_plot(
            velocity,
            "LSRK radio velocity (km/s)",
            velocity[peak_channel],
            f"Peak {velocity[peak_channel]:.3f} km/s",
            velocity_xlim,
            velocity_mask,
        )

        if args.plot_prefix is not None:
            args.plot_prefix.parent.mkdir(parents=True, exist_ok=True)
            frequency_figure.savefig(
                args.plot_prefix.with_name(args.plot_prefix.name + "_frequency.png"), dpi=100
            )
            velocity_figure.savefig(
                args.plot_prefix.with_name(args.plot_prefix.name + "_velocity.png"), dpi=100
            )
        if args.plot:
            plt.show()
        else:
            plt.close(frequency_figure)
            plt.close(velocity_figure)


if __name__ == "__main__":
    main()
