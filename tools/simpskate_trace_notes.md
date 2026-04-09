# SimpSkate Trace Build Notes

This build includes high-verbosity trace hooks intended for Simpsons Skateboarding load-chain reconstruction.

## Trace environment variables

- `PCSX2_SIMPTRACE=1`
- `PCSX2_SIMPTRACE_OUT=<path to jsonl>`
- high-signal defaults are set automatically by `run_simpskate_trace.ps1`:
  - `PCSX2_SIMPTRACE_EE_FUNCS` (key loader/dispatch functions)
  - `PCSX2_SIMPTRACE_EE_RANGES` (broad loader neighborhoods)
  - `PCSX2_SIMPTRACE_LOG_PTR_WINDOWS=1`
  - `PCSX2_SIMPTRACE_LOG_PARSER_SNAPSHOTS=1`
  - `PCSX2_SIMPTRACE_LOG_OBJECT_CANDIDATES=1`
  - `PCSX2_SIMPTRACE_PTR_WINDOW_BYTES=128`
  - `PCSX2_SIMPTRACE_STACK_WORDS=64`
  - `PCSX2_SIMPTRACE_MAX_EVENTS=1500000`

You can still override any of these per run.

## Fast launch helper

Use:

```powershell
powershell -ExecutionPolicy Bypass -File .\simpskate\run_simpskate_trace.ps1
```

from the artifact root (or pass `-Pcsx2Exe` explicitly).

## What gets logged

- EE tracepoint/range hits for loader-related functions.
- Full register snapshots on every hit (`a*`, `v*`, `t*`, `s*`, `sp/fp/gp/ra`).
- Parsed EE strings from primary argument/working registers.
- Stack window snapshots.
- Parser-struct candidate snapshots for likely parser pointers.
- Object-record candidate snapshots (including dereferenced pointers).
- Pointer-window hex/ascii dumps for candidate addresses and pointer indirections.
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

