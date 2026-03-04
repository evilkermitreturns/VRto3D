# VRto3D - Monitor Mode Fork

Fork of [oneup03/VRto3D](https://github.com/oneup03/VRto3D) with added support for receiving stereo parameters from UEVR in real time. This lets UEVR control the 3D output dynamically — depth, convergence, and calibration all stay in sync between the two programs.

Works together with a modified [UEVR](https://github.com/TheEvilKermit/UEVR) that adds monitor-native stereo rendering for Unreal Engine games.

## How It Works

VRto3D is a SteamVR driver that pretends to be a VR headset. Instead of sending frames to a headset display, it composites the left and right eye views side-by-side and puts them in a window on your 3D monitor.

This fork adds a shared memory bridge (protocol v3.3) that UEVR writes to and VRto3D reads from. UEVR sends stereo calibration data (eye separation, convergence, depth multiplier, FOV scale) and VRto3D applies it to the virtual headset parameters. This keeps the 3D effect matched to what the game is doing — when you zoom in or aim down sights, the depth adjusts automatically.

```
UE Game -> UEVR (stereo rendering) -> SteamVR -> VRto3D (SBS output) -> 3D Monitor
```

## What I Changed

- **Shared memory bridge receiver** — reads UEVR's stereo data from a 256-byte shared memory block every frame
- **Auto-detect monitor mode** — when UEVR connects and sends valid data, VRto3D switches to monitor mode automatically
- **Symmetric frustum for monitor output** — VR headsets use asymmetric frustums (wider on the nose side). Monitors need symmetric ones. This fork switches to symmetric projection when in monitor mode.
- **Scene-aware convergence** — dynamic convergence blending that tracks depth mode and world scale
- **Stereo calibration sync** — applies UEVR's calculated eye separation and convergence values instead of using static config values
- **Stereo depth hint** — reads UEVR's stereo_depth calibration value so overlay IPD matches game stereo
- **Staleness detection** — if UEVR stops sending data (game closed, UEVR unloaded), VRto3D latches the last known state instead of reverting to defaults
- **Profile modifiers** — game profiles can include UEVR overrides (depth curve, floors, thresholds) sent over the bridge

## Setup

1. Install [VRto3D](https://github.com/oneup03/VRto3D) following oneup03's setup guide
2. Register the driver with SteamVR
3. Configure your display settings in VRto3D's config (resolution, refresh rate, SBS format)
4. Launch SteamVR — VRto3D should show up as the active headset
5. Inject UEVR (the [monitor mode fork](https://github.com/TheEvilKermit/UEVR)) into your game
6. The bridge connects automatically — you should see stereo output on your 3D monitor

Check VRto3D's headset window. If it's showing side-by-side stereo, it's working.

## Configuration

All the standard VRto3D config options still work. The monitor mode additions are automatic — when UEVR sends data over the bridge, VRto3D uses it. When there's no UEVR data, VRto3D behaves like normal.

The bridge protocol file (`uevr_vrto3d_protocol.h`) must be identical in both this repo and the UEVR repo. If you modify the protocol, copy the file to both locations.

## Building From Source

```bash
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" vrto3d.sln -p:Configuration=Release -p:Platform=x64 -m -verbosity:minimal
```


## Credits

- [oneup03](https://github.com/oneup03) for VRto3D — the virtual headset driver that makes all of this possible
- [praydog](https://github.com/praydog) for UEVR — the stereo rendering engine on the game side
- [Asxcvbn](https://github.com/Asxcvbn) for getting convergence and depth controls working in OpenXR mode ([#371](https://github.com/praydog/UEVR/issues/371))
