<#
.SYNOPSIS
    Repeatable GPU/CPU benchmark for cube.exe's bench mode.

.DESCRIPTION
    Runs each scene (bench/scenes/*.bench) N times through the scripted
    camera path, collects the JSON reports cube.exe writes, and compares
    against bench/baseline.json. A run only passes when:
      * every scene's median frame time is within -MaxRegressionPct of the
        baseline median, and
      * the p99 frame time does not regress by more than that margin, and
      * the protected harness files are unchanged (SHA-256 manifest).

    NEVER edit the scenes, this script, visual_check.ps1, the reference
    images or baseline.json to make a run pass. That is the whole point of
    the manifest. Authorized harness changes: make the edit, then re-seal
    with -SealManifest (and regenerate the baseline with -UpdateBaseline
    only when the change was intentional and reviewed).

.EXAMPLE
    powershell -File bench\bench.ps1                     # build, 3 runs/scene, compare
    powershell -File bench\bench.ps1 -SkipBuild -Runs 1
    powershell -File bench\bench.ps1 -UpdateBaseline     # record a new baseline
    powershell -File bench\bench.ps1 -SealManifest
#>
param(
    [string[]]$Scenes = $null,
    [int]$Frames = 0,                 # 0 = each scene file's own frame count
    [int]$Runs = 3,
    [switch]$SkipBuild,
    [switch]$UpdateBaseline,
    [switch]$SealManifest,
    [double]$MaxRegressionPct = 2.0
)
$ErrorActionPreference = 'Stop'
$bench = $PSScriptRoot
$root = Split-Path -Parent $bench
$exe = Join-Path $root 'build\cube.exe'
$out = Join-Path $bench 'out'
$manifestPath = Join-Path $bench 'protected.sha256'
$baselinePath = Join-Path $bench 'baseline.json'

function Get-ProtectedFiles {
    $patterns = Get-Content (Join-Path $bench 'protected.txt') |
        Where-Object { $_.Trim() -and -not $_.Trim().StartsWith('#') }
    $files = @()
    foreach ($pat in $patterns) {
        $pat = $pat.Trim()
        if ($pat -match '[\*\?]') {
            $files += Get-ChildItem (Join-Path $root $pat) |
                ForEach-Object { ($_.FullName.Substring($root.Length + 1)) -replace '\\', '/' }
        }
        elseif (Test-Path (Join-Path $root $pat)) {
            $files += $pat
        }
    }
    $files | Sort-Object
}

function Get-ManifestHashes {
    foreach ($rel in (Get-ProtectedFiles)) {
        $p = Join-Path $root $rel
        "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash, $rel
    }
}

# ---- integrity manifest -----------------------------------------------------
if ($SealManifest) {
    Get-ManifestHashes | Set-Content -LiteralPath $manifestPath -Encoding ASCII
    Write-Host "[bench] sealed manifest: $(@((Get-Content $manifestPath)).Count) files -> $manifestPath"
    exit 0
}
if (Test-Path $manifestPath) {
    $sealed = @{}
    Get-Content $manifestPath | ForEach-Object {
        $h, $f = $_ -split '  ', 2
        if ($f) { $sealed[$f] = $h }
    }
    $bad = @()
    foreach ($rel in (Get-ProtectedFiles)) {
        $p = Join-Path $root $rel
        $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash
        if (-not $sealed.ContainsKey($rel)) { $bad += "$rel (not in manifest)" }
        elseif ($sealed[$rel] -ne $h) { $bad += "$rel (modified)" }
    }
    foreach ($rel in $sealed.Keys) {
        if (-not (Test-Path (Join-Path $root $rel))) { $bad += "$rel (deleted)" }
    }
    if ($bad) {
        Write-Host "[bench] BENCHMARK INTEGRITY VIOLATION - protected files changed:"
        $bad | ForEach-Object { Write-Host "    $_" }
        Write-Host "[bench] restore them or revert the edit. Do not 'fix' the harness to pass."
        exit 2
    }
}
else {
    Write-Warning "No protected.sha256 yet. Run: powershell -File bench\bench.ps1 -SealManifest"
}

# ---- build -------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host "[bench] building..."
    cmd /c "`"$root\build.bat`""
    if ($LASTEXITCODE -ne 0) { Write-Host "[bench] build failed"; exit 1 }
}
if (-not (Test-Path $exe)) { Write-Host "[bench] missing $exe"; exit 1 }

if (-not $Scenes) {
    $Scenes = (Get-ChildItem (Join-Path $bench 'scenes\*.bench')).BaseName
}
if (-not $Scenes) { Write-Host "[bench] no scenes in bench\scenes"; exit 1 }

New-Item -ItemType Directory -Force -Path $out | Out-Null
$summary = [ordered]@{}

foreach ($scene in $Scenes) {
    $rows = @()
    for ($i = 1; $i -le $Runs; $i++) {
        $json = Join-Path $out ("{0}_{1}.json" -f $scene, $i)
        if (Test-Path $json) { Remove-Item $json }
        # Run through cmd so the app's stderr goes to a file: PowerShell 5.1
        # turns merged native stderr into error records and kills the child.
        $errFile = Join-Path $out ("{0}_{1}.err.txt" -f $scene, $i)
        $framesArg = if ($Frames -gt 0) { " --frames $Frames" } else { "" }
        cmd /c "`"$exe`" --bench $scene --out `"$json`"$framesArg 2> `"$errFile`""
        $code = $LASTEXITCODE
        $log = @(Get-Content $errFile -ErrorAction SilentlyContinue)
        if ($code -ne 0 -or -not (Test-Path $json)) {
            Write-Host "[bench] $scene run $i FAILED (exit $code)"
            $log | Select-Object -Last 12 | ForEach-Object { Write-Host "    $_" }
            exit 1
        }
        $d = Get-Content $json -Raw | ConvertFrom-Json
        $rows += [pscustomobject]@{
            run = $i
            medianMs = [double]$d.frameMs.medianMs
            p95Ms = [double]$d.frameMs.p95Ms
            p99Ms = [double]$d.frameMs.p99Ms
        }
    }
    $mSorted = $rows.medianMs | Sort-Object
    $p99Sorted = $rows.p99Ms | Sort-Object
    $p95Sorted = $rows.p95Ms | Sort-Object
    $summary[$scene] = [pscustomobject]@{
        medianMs = $mSorted[[int]($mSorted.Count / 2)]
        p95Ms    = $p95Sorted[[int]($p95Sorted.Count / 2)]
        p99Ms    = $p99Sorted[[int]($p99Sorted.Count / 2)]
    }
}

# ---- report -------------------------------------------------------------------
Write-Host ""
Write-Host ("{0,-20} {1,10} {2,10} {3,10}" -f 'scene', 'medianMs', 'p95Ms', 'p99Ms')
foreach ($scene in $summary.Keys) {
    $s = $summary[$scene]
    Write-Host ("{0,-20} {1,10:N2} {2,10:N2} {3,10:N2}" -f $scene, $s.medianMs, $s.p95Ms, $s.p99Ms)
}

if ($UpdateBaseline) {
    $baseline = [ordered]@{
        generatedUtc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        runs = $Runs
        scenes = $summary
    }
    $baseline | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $baselinePath -Encoding ASCII
    Write-Host "[bench] baseline updated: $baselinePath"
    # The baseline is itself protected; re-seal so the new copy verifies.
    & $PSCommandPath -SealManifest | Out-Null
    exit 0
}
if (-not (Test-Path $baselinePath)) {
    Write-Warning "No baseline.json - record one on a known-good build: bench.ps1 -UpdateBaseline"
    exit 0
}

$baseline = Get-Content $baselinePath -Raw | ConvertFrom-Json
$fail = $false
$limit = 1.0 + $MaxRegressionPct / 100.0
foreach ($scene in $summary.Keys) {
    $cur = $summary[$scene]
    $base = $baseline.scenes.$scene
    if (-not $base) { Write-Host "[bench] ${scene}: no baseline entry"; continue }
    $dMed = ($cur.medianMs - [double]$base.medianMs) / [double]$base.medianMs * 100.0
    $d99 = ($cur.p99Ms - [double]$base.p99Ms) / [double]$base.p99Ms * 100.0
    $tag = 'OK'
    if ($cur.medianMs -gt [double]$base.medianMs * $limit -or $cur.p99Ms -gt [double]$base.p99Ms * $limit) {
        $tag = 'FAIL'; $fail = $true
    }
    Write-Host ("[bench] {0,-20} median {1:+0.0;-0.0}%  p99 {2:+0.0;-0.0}%  {3}" -f $scene, $dMed, $d99, $tag)
}
if ($fail) { Write-Host "[bench] REGRESSION: median or p99 exceeded the $MaxRegressionPct% noise floor."; exit 1 }
Write-Host "[bench] all scenes within the $MaxRegressionPct% noise floor."
exit 0
