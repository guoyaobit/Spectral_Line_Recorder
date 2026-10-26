# Spectral Line Recorder

The recorder accepts complete spectral or continuum results over ZeroMQ/TCP
and writes SDFITS output. Every unique entry in `result_ports` opens one PULL
endpoint. Processing servers may start before the recorder;
while no Writer is connected their non-blocking PUSH sockets discard results,
then automatically resume after this process starts. Each logical window has a
send high-water mark of one, so at most one already queued result can survive a
mid-run disconnect.

## Observation directory

With a system controller, set `Observation_ID` in Recorder and every processing
node to the same value. Spectral-line and continuum modes write beneath
`Storage_folder/<Observation_ID>/`; Recorder does not alter the ID or append a
timestamp. The controller must write a new ID before each new observation. An
existing directory is reused so Recorder may restart during that observation.
The ID must contain 1-64 ASCII letters, digits, `.`, `_`, or `-`, and must not
be `.` or `..`. Incoming messages are accepted only when the CRC32C value in
`spectrum_header.obs_id` matches this configured ID.

Without a controller, omit `Observation_ID`, leave it null, or set it to an
empty string on both sides. Processing nodes then send `obs_id=0`; Recorder
accepts `obs_id=0` and creates an ISO-8601 basic UTC directory containing the
target and pointing state, for example
`Storage_folder/20260925T143012.317Z_M87_ON/`. Unsafe target-name characters
are normalized for use in a path. The generated folder name is local storage
metadata and is not sent back to processing nodes.

```yaml
Observation_ID: 20260924T123015Z_M87_scan003
Storage_folder: /data
```

## Offline SDFITS ON/OFF reduction

`make_class_onoff.py` combines one source ON scan and one reference OFF scan
into a single CLASS-readable FITS spectrum. It supports exposure-normalized
integration, XX or YY selection, noise-diode calibration, LSRK radio velocity,
Hanning/boxcar/Gaussian smoothing, and separate Python frequency and velocity
plots.

Install the Python dependencies and inspect the command-line options with:

```sh
python -m pip install -r requirements-sdfits.txt
python make_class_onoff.py --help
```

Recorder `OBSFREQ` values may describe the intermediate-frequency band rather
than absolute sky frequency. Supply `--lo-mhz`, `--if-start-mhz`,
`--if-stop-mhz`, and the correct sideband when reducing such files. Supply
`--date-obs` in UTC when the input header does not contain the true observing
time.

The script does not perform baseline subtraction. See
[`SDFITS_REDUCTION.md`](SDFITS_REDUCTION.md) for calibration equations,
frequency and time metadata requirements, CLASS plotting commands, a complete
NH3(1,1) example, and the validation checklist.

## Manage with systemd

The supplied unit runs Recorder as the dedicated `spectral-recorder` account.
It does not require root or DPDK access. After building, install the runtime
files from the repository root (these commands are for a fresh installation):

```sh
sudo useradd --system --user-group --no-create-home --shell /usr/sbin/nologin spectral-recorder
sudo install -d -o root -g spectral-recorder -m 0770 /opt/Spectral_Line_Recorder
sudo install -d -o root -g spectral-recorder -m 0750 /opt/Spectral_Line_Recorder/build
sudo install -m 0755 build/7mm_recorder /opt/Spectral_Line_Recorder/build/7mm_recorder
sudo install -o root -g spectral-recorder -m 0640 config.yaml vegas_SDFITS_template.txt /opt/Spectral_Line_Recorder/
sudo install -m 0644 systemd/spectral-line-recorder.service /etc/systemd/system/
sudo systemctl daemon-reload
```

Skip account creation if it already exists. On upgrades, stop the service
before replacing the executable, and preserve the deployed `config.yaml`.
The working directory must remain the installation directory: Recorder reads
both configuration and the FITS template there, and writes `log_*.log` there.
The directory group therefore needs write permission.

Before starting, edit `/opt/Spectral_Line_Recorder/config.yaml`, verify the
storage mount, and grant the service account write/search access to the chosen
`Storage_folder`. For a new, dedicated output directory, for example:

```sh
sudo install -d -o spectral-recorder -g spectral-recorder -m 0750 /data/recorder
sudo -u spectral-recorder test -w /data/recorder
```

Set `Storage_folder: /data/recorder` for that example; the default remains `/data`.
Do not recursively change ownership of existing observation data. Update the
unit's `RequiresMountsFor` with `systemctl edit` if storage is on another mount.
Ports below 1024 require additional privileges; the default ports do not.

```sh
sudo systemctl start spectral-line-recorder
sudo systemctl status spectral-line-recorder --no-pager -l
sudo journalctl -u spectral-line-recorder -f
sudo systemctl stop spectral-line-recorder
```

The unit deliberately uses `Restart=no`: a crash must remain visible, and an
unattended restart must not start a new manual observation. Boot startup is
opt-in with `sudo systemctl enable spectral-line-recorder`; it is not enabled
by installation. Configuration changes require stopping and starting the
service. Recorder and a manually started copy must not run on the same ports.

There is currently no graceful signal shutdown/queue-draining implementation.
`systemctl stop` sends SIGTERM; queued results may be lost. Stop upstream
senders first and allow pending writes to finish before stopping Recorder.
This is not a guarantee that all queued rows have been saved. Check output
files after an interrupted observation. `restart` has the same limitation.

## Result ports

`result_ports` is only the set of TCP endpoints on which Recorder listens. It
may contain one or more arbitrary, unique, non-contiguous ports, and list order
has no meaning. A processing server's `subbands[].port` selects any one of
these endpoints. Multiple subbands, beams, servers, or windows may share the
same port.

Recorder does not derive data identity from the TCP port. It validates the
transport envelope and reads the global `subband_id`, `beam_id`, and
`window_id` from `spectrum_header`; these fields select the independent output
file and continuum input. Therefore changing or sharing destination ports does
not merge different results. Port changes require configuration changes only,
not recompilation.

## Continuum aggregation

In continuum mode each processing server sends one `float` scalar for every
physical-subband/beam input and integration. The receiver groups results by
the compensated VDIF integration timestamp, with a 1 microsecond matching
tolerance. Its identity is `(global subband ID, beam ID)`, so duplicates are
not counted twice. Power is accumulated in `double` precision and written only
when `continuum_inputs` unique values have arrived.

For all eight servers, `continuum_inputs` is `8 * 4 * 2 = 64`; for two enabled
servers it is 16. Non-finite values, inconsistent exposure/noise states, and
incomplete integrations are discarded. An incomplete integration times out
after five seconds; the recorder never writes a partial continuum sum.

## Build

On Debian or Ubuntu install the build dependencies, including ZeroMQ:

```sh
sudo apt install build-essential meson ninja-build pkg-config \
  libcfitsio-dev libspdlog-dev libyaml-cpp-dev libzmq3-dev
rm -rf build
meson setup build --buildtype=release
ninja -C build
```

The result transport no longer requires a DPDK-bound NIC. The storage address
must belong to a kernel-managed interface, such as the 100G SR-IOV VF used by
the processing servers. Allow every configured `result_ports` value through
the host firewall.

Each message contains a 24-byte transport envelope and a 103-byte version-3
`spectrum_header`, and one complete result payload. The receiver checks the
server/subband mapping, payload size, sequence number, and CRC32C before a
result can enter a Writer queue. A full Writer queue drops the complete result
and records the drop; partial SDFITS rows are never written.

Version 3 adds an explicit PFB-corrected integration start timestamp.
The existing centre timestamp calculation is retained.
`EXPOSURE` remains the accumulated live time; blanked periods
and the other noise state are excluded. SDFITS `TIME` and the initial
`DATE-OBS`/`MJD-OBS`/`STTMJD` use the explicit start, even when a calibration
integration spans several switching cycles. Continuum output uses the same
explicit start. Sender and Recorder must be upgraded together; v2 is rejected.

On Linux the recorder reads its allowed process CPU mask and assigns separate
cores to the main thread, the ZeroMQ I/O pool, each configured PULL endpoint,
and each SDFITS Writer. With the default eight-port spectral configuration this
uses 25 cores; using fewer listening ports reduces that count. If fewer cores
are available the mapping wraps and is reported in the startup log. A systemd
`CPUAffinity=` setting or `taskset` can restrict the CPU set from which the
recorder automatically assigns cores.

## Shared SDFITS metadata configuration

Use the same scan and header keys on processing nodes and Recorder:

```yaml
object: test
source_on: true
sdfits_metadata:
  telescope: NSRT
  instrument: 7mm
  # ra_deg: actual ICRS right ascension in degrees
  # dec_deg: actual ICRS declination in degrees
  # site_lon_deg: surveyed east-positive longitude in degrees
  # site_lat_deg: surveyed latitude in degrees
  # site_height_m: surveyed elevation in metres
```

`object` populates OBJECT. `source_on` populates SRCSTATE and the ON/OFF
filename component. Telescope/instrument populate TELESCOP/INSTRUME;
coordinates populate CRVAL4/CRVAL5 and SITELONG/SITELAT/SITEELEV.
Unknown coordinates must be omitted. These settings do not calibrate power
or convert IF to sky frequency. Raw SDFITS is reduced with the existing
ON/OFF script before CLASS import.

Developer: YAO GUO, Institute of Automation, Chinese Academy of Sciences.
