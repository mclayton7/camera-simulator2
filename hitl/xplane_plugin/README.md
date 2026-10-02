# CamSimTruth: X-Plane 11 truth plugin

A read-only X-Plane 11 plugin for the Hooter HITL rig. It sends the aircraft's
truth state to the IG host as one UDP datagram per flight loop. The design is in
`HITL.md` ("Getting truth out of X-Plane 11") and the wire format in
`hitl/PROTOCOL.md` §1: 224 bytes, little-endian, magic `CSTR`, version 1.

- **Read-only.** It never writes a dataref, never changes X-Plane's network or
  data-output settings, and opens no listening socket. It can't disturb
  XPlaneHILInterface.
- **API level `XPLM303`** (X-Plane 11.50+). It builds against any SDK from 3.0.x
  up and uses no XPLM400 API.
- **One datagram per frame.** It sends from an `xplm_FlightLoop_Phase_AfterFlightModel`
  flight loop that returns −1, using a non-blocking `sendto`. It has no threads,
  and every XPLM call happens on X-Plane's main thread.

## Build

The SDK isn't checked in. `fetch_sdk.sh` downloads it into `.sdk/SDK`, which is
gitignored. The default is XPSDK 4.3.0 with a sha256 pin. To use another
version, run `XPSDK_VERSION=411 ./fetch_sdk.sh`, or pass `-DXPLANE_SDK_DIR=` to
CMake.

**Linux** (Ubuntu 18.04+ / any glibc ≥ 2.17):

```bash
cd hitl/xplane_plugin
./fetch_sdk.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
# -> build/CamSimTruth/lin_x64/CamSimTruth.xpl
```

The Linux `.xpl` links nothing from X-Plane: its `XPLM*` imports resolve when
X-Plane loads it. It needs only `libc.so.6`, since libgcc is linked statically.

**macOS** (Xcode command-line tools, CMake ≥ 3.21). This produces one universal
arm64 + x86_64 binary linked against the SDK's `XPLM.framework`:

```bash
./fetch_sdk.sh && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# -> build/CamSimTruth/mac_x64/CamSimTruth.xpl
```

The harness tests also build and run on macOS. The two ELF-specific checks
(`xpl_dlopen`, `xpl_symbols`) are skipped there.

**Windows** (Visual Studio 2019 16.8+ or 2022, x64). This links `XPLM_64.lib`
and `ws2_32` with a static CRT. Run `fetch_sdk.sh` from Git Bash, or unzip the
SDK into `.sdk\SDK` yourself:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
:: -> build\CamSimTruth\win_x64\CamSimTruth.xpl
```

The tests are not built on Windows.

Only the Linux build has been compiled and tested so far. The macOS and Windows
CMake paths are written but haven't been built yet.

## Install

```
X-Plane 11/Resources/plugins/CamSimTruth/
    CamSimTruth.ini              (optional)
    lin_x64/CamSimTruth.xpl
    mac_x64/CamSimTruth.xpl
    win_x64/CamSimTruth.xpl
```

`cmake --install build --prefix "<X-Plane 11>/Resources/plugins"` installs the
`.xpl` and `CamSimTruth.ini.example`.

## Configuration: `CamSimTruth.ini`

The plugin reads this file once, when it is enabled. It looks first in the
`CamSimTruth/` folder, then next to the `.xpl`. If neither file exists, it uses
the defaults. The format is `key = value`, with `#` or `;` starting a comment.
The plugin logs and ignores unknown keys and bad values.

| Key | Default | Meaning |
| --- | --- | --- |
| `dest` | `127.0.0.1:49300` | IG host `host:port`. IPv6 is written `[addr]:port`. A host name is resolved once, at enable. |
| `enabled` | `1` | Set `0` (or `false`/`no`/`off`) to load without sending anything. |
| `weather_period_s` | `1.0` | Seconds between weather-block refreshes. `0` sends the block in every datagram. |
| `terrain_probe_hz` | every datagram | Terrain probes per second. `0` turns the probe off; a negative value or leaving the key out probes every datagram. |

To apply a change, disable and re-enable the plugin in Plugin Admin, or restart
X-Plane.

## What it sends

The field list is in `hitl/PROTOCOL.md` §1. Every value is the raw dataref value
with no unit conversion. Behaviour the protocol leaves to the sender:

- **`seq`** starts at 0 when the plugin loads and increases by 1 per datagram.
  It keeps counting across disable/enable.
- **`cycle`** is `XPLMGetCycleNumber()`, and **`mono_ns`** is the sending
  machine's monotonic clock (`CLOCK_MONOTONIC` on Linux, `mach_absolute_time` on
  macOS, `QueryPerformanceCounter` on Windows). Use it only for ordering and
  jitter: it isn't wall-clock time.
- **Weather block (bit2).** The block runs from byte 136 to the end. It is
  filled only in the datagram that refreshes it, which by default is the first
  datagram after enable and then one about every second. Every other datagram
  has bit2 clear and the block zeroed, as PROTOCOL.md specifies.
  - The receiver keeps the last block it got.
  - One lost datagram delays the weather by one period, which is acceptable for
    slowly changing values.
  - The schedule runs on the monotonic clock with a fixed cadence. After a
    stall longer than one period it restarts from now instead of sending a
    burst.
- **Terrain probe (bit3).**
  - Each datagram that is due a probe converts the aircraft's double-precision
    lat/lon/elevation with `XPLMWorldToLocal` and probes with
    `XPLMProbeTerrainXYZ` (a `xplm_ProbeY` probe). That is every datagram by
    default.
  - The hit point goes back through `XPLMLocalToWorld`, whose altitude is
    metres MSL.
  - bit3 is set, and `terrain_msl` filled, only when the probe ran in that
    frame and hit terrain. Otherwise both are 0.
  - One probe a frame is cheap, and probing every frame gives the receiver a
    fresh value with every pose. Set `terrain_probe_hz` to reduce it.
- **Missing datarefs.**
  - The plugin looks up dataref handles once per enable.
  - A dataref that doesn't exist in the running X-Plane version reads as 0, and
    a flag that depends on it stays clear. Examples are `is_in_replay` (11.00+),
    `sim_speed_actual_ogl` (11.30+), `relative_humidity_sealevel_percent`
    (11.35+) and `true_psi` (10.30+).
  - All missing datarefs are logged once, at enable.
- **Dataref types.** The plugin reads each dataref by its published type,
  preferring double, then float, then int, then array element. So `sim_speed`,
  `cloud_type` and `wave_dir`, which are ints in XP11, arrive as floats, as the
  protocol says.
- **Cloud layers.** X-Plane 11 publishes them as separate scalar datarefs named
  with the index, for example `sim/weather/cloud_base_msl_m[0]`, `[1]` and
  `[2]`. The plugin looks up those names first. If they don't exist, it falls
  back to element *i* of an array named `sim/weather/cloud_base_msl_m` (the
  X-Plane 12 form).
- **Wind.** `wind_speed` and `wind_dir_degt` come from the scalars
  `sim/weather/wind_speed_kt` and `sim/weather/wind_direction_degt`, the
  "effective wind at the plane's location". They do not come from the per-layer
  `wind_speed_kt[i]`, which really is in knots.

## Checking it on the rig

1. **Log.txt.** After X-Plane starts, `X-Plane 11/Log.txt` should contain lines
   like these:

   ```
   CamSimTruth: 1.0.0 (protocol v1, XPLM303) sending 224-byte datagrams to 10.0.0.5:49300 every flight loop; weather every 1 s, terrain probe every datagram; config: .../plugins/CamSimTruth/CamSimTruth.ini
   CamSimTruth: all datarefs found
   ```

   - If datarefs are missing, the second line instead reads
     `missing datarefs (sent as 0, flags clear): ...`.
   - Send failures are logged once per distinct error. The total is reported
     when the plugin is disabled.

2. **Watch the datagrams.** Run this on the IG host, with the host's receiver
   stopped so the port is free:

   ```bash
   python3 - <<'EOF'
   import socket, struct
   S = struct.Struct("<4sHHIIQdddd" + "f"*18 + "BBBB" + "f"*23)
   s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("0.0.0.0", 49300))
   while True:
       v = S.unpack(s.recv(512))
       print(f"seq {v[4]} cyc {v[3]} flags {v[2]:04b} lat {v[6]:.7f} lon {v[7]:.7f} elev {v[8]:.1f} "
             f"terr {v[9]:.1f} agl {v[23]:.1f} hdg {v[10]:.1f} pit {v[11]:.1f} rol {v[12]:.1f} "
             f"ogl {v[25]:.2f} zulu {v[26]:.1f} {v[28]}/{v[29]} wind {v[36]:.1f}@{v[37]:.0f}")
   EOF
   ```

   You can also use the IG host's own receiver in `hitl/camsim_hitl/`.

3. **Check these datarefs once on the rig.** Each one is an assumption, not
   documented behaviour (HITL.md):
   - **`current_month` / `current_day`.** They are assumed to be the local
     date. Set a time just after local midnight with the zulu date still on the
     previous day. Check that the receiver's `xp11_utc()` gives the right UTC
     date, and check 29 February.
   - **`y_agl` reference.** This is probably the wheels, not the CG. On the
     ground, compare `y_agl` with `elevation − terrain_msl`. The difference is
     the reference point's height.
   - **`wind_speed_kt` units.** XP11's DataRefs.txt documents it as metres per
     second despite the name. Set a known wind and compare.
   - **`cloud_coverage` scale.** XP11 documents it as `0..6` with no
     description ("todo"). XP12 uses 0 = clear … 4 = overcast. Record what each
     weather-window setting gives.
   - **`wave_amplitude` meaning.** DataRefs.txt says "height of waves". Check
     whether that is amplitude (half the crest-to-trough height) or the full
     height before mapping it to a sea state.
   - **`wave_dir` convention.** It is an int, in degrees. Check whether it is
     the direction the waves travel *to* or come *from*, and whether it is
     relative to true north.
   - Also flag runs while `sim_speed_actual_ogl` < 0.98 (time dilation).
     Compare `terrain_msl` with Cesium at the airfield (HITL.md phase 1).

## Tests (no X-Plane needed)

`ctest` runs the following checks:

- **`plugin_harness`** links the plugin code against a stub XPLM
  (`tests/xplm_stub.c`). The stub has XP11-style datarefs, including the
  per-layer `[i]` names and decoy datarefs that the plugin must not read. It
  also has a manual flight loop, a terrain probe and a fake clock.
  - It receives every datagram on 127.0.0.1 and checks each field against the
    offsets in PROTOCOL.md. Those offsets are typed independently of
    `truth_packet.h`.
  - Scenarios:
    - every field over 110 frames;
    - the 1 s weather cadence, including after a 5 s stall;
    - paused and replay bits;
    - 8 Hz and disabled terrain probe, plus a probe miss;
    - missing datarefs, which read 0, leave flags clear and are logged once;
    - the X-Plane 12 array fallback;
    - `enabled = 0`, defaults and bad ini lines;
    - the real monotonic clock;
    - that handles are looked up only at enable.
- **`python_struct_decode`** decodes the harness's dumped datagram with the
  `struct` format from PROTOCOL.md.
- **`xpl_dlopen`** (Linux) loads the real `.xpl` with `RTLD_NOW` against a shared
  stub XPLM and calls the five entry points. It also checks that internal
  symbols are hidden.
- **`xpl_symbols`** (Linux) runs `nm -D`. It checks that the five `XPlugin*`
  functions are the only exports, that the XPLM imports are the expected
  read-only set (no `XPLMSetData*`), and that the plugin doesn't depend on
  libstdc++ or libgcc_s.

`src/truth_packet.h` checks every field offset against PROTOCOL.md and checks
`sizeof == 224`, both at compile time.
