<#
.SYNOPSIS
    Module-boundary lint for src/ (docs/module-refactor-plan.md, section 3).

.DESCRIPTION
    Scans every .c/.h under src/ for #include lines and checks them against the
    module rules:

      tier      A file may include only headers of its own tier or a lower one.
                Tiers: engine (0) < world (1) < app (2).
      internal  A *_internal.h header may be included only from its own module.
      config    Engine code must not include WorldConfig.h; it gets values
                through the config structs passed to its _Init.
      gl        Only engine/core/gl.h may include windows.h, glad or GLFW.
      unmapped  Every source file must belong to a module.

    Module ownership comes from the path: src/engine/<m>/, src/world/<m>/,
    src/app/. Files still in the pre-refactor flat layout are mapped through
    $LegacyMap below, to the module the plan assigns them. Delete each entry
    once phase 4 has moved its file, and delete the whole map at the end.

    Report-only by default (exit 0). With -Strict, any violation exits 1;
    the build switches to -Strict in phase 5.

.EXAMPLE
    powershell -File tools\check_modules.ps1
    powershell -File tools\check_modules.ps1 -Strict
    powershell -File tools\check_modules.ps1 -Rule tier,internal
#>
param(
    [switch]$Strict,
    [string[]]$Rule = @('tier', 'internal', 'config', 'gl', 'unmapped')
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root 'src'

$TierRank = @{ engine = 0; world = 1; app = 2 }

# Pre-refactor file -> target module (src-relative, forward slashes).
# 'dead' = scheduled for deletion; 'third_party' = not our code.
$LegacyMap = @{
    'WorldConfig.h'            = 'world'
    'Bodies.c'                 = 'world/bodies'
    'Bodies.h'                 = 'world/bodies'
    'SolarSystem.c'            = 'world/bodies'
    'SolarSystem.h'            = 'world/bodies'
    'Planets.c'                = 'world/bodies'
    'Planets.h'                = 'world/bodies'
    'Sun.c'                    = 'world/bodies'
    'Sun.h'                    = 'world/bodies'
    'geometry/QuadCube.c'      = 'world/terrain'
    'geometry/QuadCube.h'      = 'world/terrain'
    'geometry/QuadTree.c'      = 'world/terrain'
    'geometry/QuadTree.h'      = 'world/terrain'
    'TerrainGen.c'             = 'world/terrain'
    'TerrainGen.h'             = 'world/terrain'
    'TerrainDetail.c'          = 'world/terrain'
    'TerrainDetail.h'          = 'world/terrain'
    'Water.c'                  = 'world/ocean'
    'Water.h'                  = 'world/ocean'
    'Atmosphere.c'             = 'world/atmosphere'
    'Atmosphere.h'             = 'world/atmosphere'
    'Clouds.c'                 = 'world/clouds'
    'Clouds.h'                 = 'world/clouds'
    'Skybox.c'                 = 'world/sky'
    'Skybox.h'                 = 'world/sky'
    'Nebula.c'                 = 'world/sky'
    'Nebula.h'                 = 'world/sky'
    'Rings.c'                  = 'world/rings'
    'Rings.h'                  = 'world/rings'
    'Shadows.c'                = 'world/shadows'
    'Shadows.h'                = 'world/shadows'
    'Main.c'                   = 'app'
    'Main.h'                   = 'app'
    'LoadingScreen.c'          = 'app'
    'LoadingScreen.h'          = 'app'
    'Bench.c'                  = 'app'
    'Bench.h'                  = 'app'
    'DebugGUI.c'               = 'app'
    'DebugGUI.h'               = 'app'
}
$GlHeaders = @('windows.h', 'glad/glad.h', 'GLFW/glfw3.h')
$GlHome = 'engine/core/gl.h'
$SkipDirs = @('backups', 'shaders', 'third_party')

function Get-Rel([string]$full) {
    ($full.Substring($src.Length + 1)) -replace '\\', '/'
}

function Get-Module([string]$rel) {
    if ($rel -match '^(engine|world)/([^/]+)/') { return "$($Matches[1])/$($Matches[2])" }
    if ($rel -match '^world/[^/]+$') { return 'world' }
    if ($rel -match '^app/') { return 'app' }
    foreach ($k in $LegacyMap.Keys) {
        if ($k -ieq $rel) { return $LegacyMap[$k] }
    }
    return $null
}

function Get-Tier([string]$module) {
    if (-not $module) { return $null }
    $TierRank[($module -split '/')[0]]
}

# ---- collect sources ----------------------------------------------------------
$files = Get-ChildItem -Path $src -Recurse -File -Include *.c, *.h | Where-Object {
    $parts = (Get-Rel $_.FullName) -split '/'
    -not ($parts[0..($parts.Count - 2)] | Where-Object { $SkipDirs -contains $_ })
}
$relFiles = @($files | ForEach-Object { Get-Rel $_.FullName })

# Quoted include -> src-relative path. Same lookup order as the compiler: the
# includer's directory, then any -I directory (approximated by a unique suffix
# match anywhere under src/). $null = not ours (system or third-party).
function Resolve-Include([string]$includerRel, [string]$name) {
    $name = $name -replace '\\', '/'
    $dir = Split-Path -Parent $includerRel
    $local = if ($dir) { "$dir/$name" -replace '\\', '/' } else { $name }
    $hit = $relFiles | Where-Object { $_ -ieq $local }
    if ($hit) { return @($hit)[0] }
    $hits = @($relFiles | Where-Object { $_ -ieq $name -or $_ -ilike "*/$name" })
    if ($hits.Count -ge 1) { return $hits[0] }
    return $null
}

# ---- check --------------------------------------------------------------------
$violations = New-Object System.Collections.Generic.List[object]
function Add-Violation($rule, $file, $line, $msg) {
    if ($Rule -contains $rule) {
        $violations.Add([pscustomobject]@{ Rule = $rule; File = $file; Line = $line; Msg = $msg })
    }
}

foreach ($f in $files) {
    $rel = Get-Rel $f.FullName
    $mod = Get-Module $rel
    if (-not $mod) {
        Add-Violation 'unmapped' $rel 0 'not under src/engine|world|app and not in the legacy map'
        continue
    }
    if ($mod -eq 'dead' -or $mod -eq 'third_party') { continue }
    $tier = Get-Tier $mod

    $n = 0
    foreach ($text in [System.IO.File]::ReadLines($f.FullName)) {
        $n++
        if ($text -notmatch '^\s*#\s*include\s*([<"])([^>"]+)[>"]') { continue }
        $quote = $Matches[1]; $name = $Matches[2]

        if ($GlHeaders -contains $name -and $rel -ine $GlHome) {
            Add-Violation 'gl' $rel $n "includes <$name>; include engine/core/gl.h instead"
        }
        # <...> is resolved too: our own headers have been included that way
        # (QuadTree.h had <Camera.h>), and system headers never match a file
        # under src/.
        $target = Resolve-Include $rel $name
        if (-not $target) { continue }
        $tmod = Get-Module $target
        if (-not $tmod -or $tmod -eq 'third_party') { continue }
        if ($tmod -eq 'dead') {
            Add-Violation 'tier' $rel $n "includes $target, which is scheduled for deletion"
            continue
        }
        $ttier = Get-Tier $tmod

        if ($ttier -gt $tier) {
            Add-Violation 'tier' $rel $n "[$mod] includes $target [$tmod]: upward include"
        }
        if ((Split-Path -Leaf $target) -like '*_internal.h' -and $tmod -ne $mod) {
            Add-Violation 'internal' $rel $n "[$mod] includes $target, private to [$tmod]"
        }
        if ($tier -eq 0 -and (Split-Path -Leaf $target) -ieq 'WorldConfig.h') {
            Add-Violation 'config' $rel $n "[$mod] includes WorldConfig.h; pass values through its config struct"
        }
    }
}

# ---- report -------------------------------------------------------------------
if ($violations.Count -eq 0) {
    Write-Host "[modules] no violations ($($files.Count) files checked)"
    exit 0
}
foreach ($g in ($violations | Group-Object Rule | Sort-Object Name)) {
    Write-Host ""
    Write-Host "[$($g.Name)] $($g.Count)"
    foreach ($v in ($g.Group | Sort-Object File, Line)) {
        $loc = if ($v.Line) { "$($v.File):$($v.Line)" } else { $v.File }
        Write-Host ("  {0,-34} {1}" -f $loc, $v.Msg)
    }
}
Write-Host ""
Write-Host "[modules] $($violations.Count) violations in $($files.Count) files"
if ($Strict) { exit 1 }
exit 0
