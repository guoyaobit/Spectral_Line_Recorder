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

Each message contains a 24-byte transport envelope, the unchanged 95-byte
`spectrum_header`, and one complete result payload. The receiver checks the
server/subband mapping, payload size, sequence number, and CRC32C before a
result can enter a Writer queue. A full Writer queue drops the complete result
and records the drop; partial SDFITS rows are never written.

On Linux the recorder reads its allowed process CPU mask and assigns separate
cores to the main thread, the ZeroMQ I/O pool, each configured PULL endpoint,
and each SDFITS Writer. With the default eight-port spectral configuration this
uses 25 cores; using fewer listening ports reduces that count. If fewer cores
are available the mapping wraps and is reported in the startup log. A systemd
`CPUAffinity=` setting or `taskset` can restrict the CPU set from which the
recorder automatically assigns cores.
