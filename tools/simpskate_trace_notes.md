# SimpSkate Trace Build Notes

This build includes trace hooks intended for Simpsons Skateboarding load-chain reconstruction.

## Trace environment variables

- `PCSX2_SIMPTRACE=1`
- `PCSX2_SIMPTRACE_OUT=<path to jsonl>`
- `run_simpskate_trace.ps1` supports two capture profiles:
  - `Focused` (default): exact EE functions only, object candidates on, pointer/parser windows off, small stack snapshots, low event cap
  - `Firehose`: broad EE ranges plus pointer/parser/object windows for one-off deep dives

You can still override any of these per run.
If you want to explicitly disable EE range tracing, set `PCSX2_SIMPTRACE_EE_RANGES=off`.

## Fast launch helper

Use:

```powershell
powershell -ExecutionPolicy Bypass -File .\simpskate\run_simpskate_trace.ps1
```

For the heavy version only:

```powershell
powershell -ExecutionPolicy Bypass -File .\simpskate\run_simpskate_trace.ps1 -Profile Firehose
```

from the artifact root (or pass `-Pcsx2Exe` explicitly).

## What gets logged

- EE trace hits for loader-related functions.
- Request-block snapshots for the School menu/load dispatcher chain (`0x50A8A0`).
- Full register snapshots on every hit (`a*`, `v*`, `t*`, `s*`, `sp/fp/gp/ra`).
- Parsed EE strings from primary argument/working registers.
- Object-record candidate snapshots.
- Optional stack / parser / pointer-window dumps depending on profile.
- ISO open/map metadata.
- Aggregated ISO read runs with:
  - start LSN
  - sector count
  - mode
  - current EE PC / IOP PC
  - owning file path and file-relative offset when available

## Focused profile hook set

The default `Focused` profile now traces:

- menu/location confirm: `0x0030FD00`
- request/dispatch chain: `0x002D5150`, `0x00190830`, `0x00190870`, `0x001908A0`, `0x00191360`, `0x00191430`
- location/index mapping: `0x00131410`, `0x00131510`
- load-stage gates: `0x001359D0`, `0x00136A10`
- DAT object conversion: `0x0013E510`, `0x0013E910`, `0x0011C4D8`

## Analyze trace

```powershell
python .\simpskate\analyze_simpskate_trace.py <trace.jsonl>
```

