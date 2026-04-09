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

## Analyze trace

```powershell
python .\simpskate\analyze_simpskate_trace.py <trace.jsonl>
```

