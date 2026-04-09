param(
    [string]$Pcsx2Exe = "",
    [string]$TraceOut = "",
    [ValidateSet("Focused", "Firehose")]
    [string]$Profile = "Focused"
)

$ErrorActionPreference = "Stop"

function Resolve-Pcsx2Exe {
    param([string]$Candidate)

    if ($Candidate -and (Test-Path -LiteralPath $Candidate)) {
        return (Resolve-Path -LiteralPath $Candidate).Path
    }

    $scriptDir = if ($PSScriptRoot) {
        $PSScriptRoot
    } elseif ($MyInvocation.MyCommand.Path) {
        Split-Path -Parent $MyInvocation.MyCommand.Path
    } else {
        (Get-Location).Path
    }
    $rootDir = Resolve-Path (Join-Path $scriptDir "..")
    $defaultExe = Join-Path $rootDir "bin\pcsx2-qt.exe"
    if (Test-Path -LiteralPath $defaultExe) {
        return (Resolve-Path -LiteralPath $defaultExe).Path
    }

    throw "Could not find pcsx2-qt.exe. Pass -Pcsx2Exe <full path>."
}

function Resolve-TraceOut {
    param([string]$Candidate)

    if ($Candidate) {
        $dir = Split-Path -Parent $Candidate
        if ($dir -and -not (Test-Path -LiteralPath $dir)) {
            New-Item -ItemType Directory -Path $dir -Force | Out-Null
        }
        return $Candidate
    }

    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    return (Join-Path $env:TEMP ("pcsx2_simpskate_trace_{0}.jsonl" -f $stamp))
}

$exePath = Resolve-Pcsx2Exe -Candidate $Pcsx2Exe
$tracePath = Resolve-TraceOut -Candidate $TraceOut

$env:PCSX2_SIMPTRACE = "1"
$env:PCSX2_SIMPTRACE_OUT = $tracePath
$env:PCSX2_SIMPTRACE_EE_FUNCS = "0x00131410,0x00131510,0x001359D0,0x00136A10,0x0013E510,0x0013E910,0x00190830,0x00190870,0x001908A0,0x00191360,0x00191430,0x0011C4D8,0x002D5150,0x0030FD00"

if ($Profile -eq "Firehose") {
    $env:PCSX2_SIMPTRACE_EE_RANGES = "0x00130000-0x00141FFF,0x0011C000-0x0011DFFF,0x002D0000-0x002D9FFF,0x0030F000-0x00311FFF"
    $env:PCSX2_SIMPTRACE_LOG_PTR_WINDOWS = "1"
    $env:PCSX2_SIMPTRACE_LOG_PARSER_SNAPSHOTS = "1"
    $env:PCSX2_SIMPTRACE_LOG_OBJECT_CANDIDATES = "1"
    $env:PCSX2_SIMPTRACE_PTR_WINDOW_BYTES = "128"
    $env:PCSX2_SIMPTRACE_STACK_WORDS = "64"
    $env:PCSX2_SIMPTRACE_MAX_EVENTS = "1500000"
} else {
    $env:PCSX2_SIMPTRACE_EE_RANGES = "off"
    $env:PCSX2_SIMPTRACE_LOG_PTR_WINDOWS = "0"
    $env:PCSX2_SIMPTRACE_LOG_PARSER_SNAPSHOTS = "0"
    $env:PCSX2_SIMPTRACE_LOG_OBJECT_CANDIDATES = "1"
    $env:PCSX2_SIMPTRACE_PTR_WINDOW_BYTES = "32"
    $env:PCSX2_SIMPTRACE_STACK_WORDS = "8"
    $env:PCSX2_SIMPTRACE_MAX_EVENTS = "25000"
}

Write-Host "Launching trace-enabled PCSX2..."
Write-Host "  EXE  : $exePath"
Write-Host "  TRACE: $tracePath"
Write-Host "  PROFILE: $Profile"
Write-Host "  EE_FUNCS : $env:PCSX2_SIMPTRACE_EE_FUNCS"
Write-Host "  EE_RANGES: $env:PCSX2_SIMPTRACE_EE_RANGES"
Write-Host "  PTR_BYTES: $env:PCSX2_SIMPTRACE_PTR_WINDOW_BYTES"
Write-Host "  STACK_WDS: $env:PCSX2_SIMPTRACE_STACK_WORDS"
Write-Host "  MAX_EVENTS: $env:PCSX2_SIMPTRACE_MAX_EVENTS"
Write-Host ""
Write-Host "After you load Springfield Elementary -> Skatefest and exit PCSX2,"
Write-Host "send this JSONL trace file back for analysis."

Start-Process -FilePath $exePath -WorkingDirectory (Split-Path -Parent $exePath)

