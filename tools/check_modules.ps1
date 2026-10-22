<#
.SYNOPSIS
    Module-boundary lint for src/ (docs/module-refactor-plan.md, section 3).

.DESCRIPTION
    Scans every .c/.h and every shader under src/ for #include lines and checks
    them against the module rules:

      tier      A file may include only headers (or shader chunks) of its own
                tier or a lower one. Tiers: engine (0) < world (1) < app (2).
      internal  A *_internal.h header may be included only from its own module.
      config    Engine code must not include WorldConfig.h; it gets values
                through the config structs passed to its _Init.
      gl        Only engine/core/gl.h may include windows.h, glad or GLFW.
      unmapped  Every file must live in a module: src/engine/<m>/, src/world/<m>/
                or src/app/ (src/world/WorldConfig.h belongs to "world").
      missing   An #include of ours that resolves to no file.

    Includes resolve the way the compiler and the shader loader do:
      C:      the includer's folder, then src/ (-I "."); <...> the same way,
              because our own headers have been included with <> before.
      shader: the includer's folder, then the shader name
              ("world/clouds/cloudlayer.glsl" -> world/clouds/shaders/...).
    A C include that matches nothing is a system or third-party header.

    Report-only by default (exit 0). With -Strict, any violation exits 1;
    the build switches to -Strict in phase 5.

.EXAMPLE
    powershell -File tools\check_modules.ps1
    powershell -File tools\check_modules.ps1 -Strict
    powershell -File tools\check_modules.ps1 -Rule tier,internal
#>
param(
    [switch]$Strict,
    [string[]]$Rule = @('tier', 'internal', 'config', 'gl', 'unmapped', 'missing')
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root 'src'

$TierRank = @{ engine = 0; world = 1; app = 2 }
$GlHeaders = @('windows.h', 'glad/glad.h', 'GLFW/glfw3.h')
$GlHome = 'engine/core/gl.h'
$ShaderExt = @('.vert', '.frag', '.geom', '.tcsh', '.tesh', '.glsl')

function Get-Rel([string]$full) {
    ($full.Substring($src.Length + 1)) -replace '\\', '/'
}

# Module = the folder that owns the file; a shader's module is the folder
# above its shaders/ subfolder.
function Get-Module([string]$rel) {
    $rel = $rel -replace '/shaders/[^/]+$', '/x'
    if ($rel -match '^(engine|world)/([^/]+)/') { return "$($Matches[1])/$($Matches[2])" }
    if ($rel -match '^world/[^/]+$') { return 'world' }
    if ($rel -match '^app/') { return 'app' }
    return $null
}

function Get-Tier([string]$module) { $TierRank[($module -split '/')[0]] }

# ---- collect files -------------------------------------------------------------
$all = Get-ChildItem -Path $src -Recurse -File |
    Where-Object { @('.c', '.h') + $ShaderExt -contains $_.Extension }
$exists = @{}
foreach ($f in $all) { $exists[(Get-Rel $f.FullName).ToLowerInvariant()] = Get-Rel $f.FullName }

function Find([string]$rel) {
    $rel = ($rel -replace '\\', '/') -replace '^\./', ''
    $exists[$rel.ToLowerInvariant()]
}

function Resolve-Include([string]$includerRel, [string]$name, [bool]$isShader) {
    $dir = Split-Path -Parent $includerRel
    $local = if ($dir) { "$dir/$name" } else { $name }
    $hit = Find $local
    if ($hit) { return $hit }
    if ($isShader) {
        $i = $name.LastIndexOf('/')
        $named = if ($i -ge 0) { $name.Substring(0, $i + 1) + 'shaders/' + $name.Substring($i + 1) } else { "shaders/$name" }
        return Find $named
    }
    return Find $name
}

# ---- check --------------------------------------------------------------------
$violations = New-Object System.Collections.Generic.List[object]
function Add-Violation($rule, $file, $line, $msg) {
    if ($Rule -contains $rule) {
        $violations.Add([pscustomobject]@{ Rule = $rule; File = $file; Line = $line; Msg = $msg })
    }
}

foreach ($f in $all) {
    $rel = Get-Rel $f.FullName
    $isShader = $ShaderExt -contains $f.Extension
    $mod = Get-Module $rel
    if (-not $mod) {
        Add-Violation 'unmapped' $rel 0 'not under src/engine/<m>, src/world/<m> or src/app'
        continue
    }
    $tier = Get-Tier $mod

    $n = 0
    foreach ($text in [System.IO.File]::ReadLines($f.FullName)) {
        $n++
        if ($text -notmatch '^\s*#\s*include\s*([<"])([^>"]+)[>"]') { continue }
        $name = $Matches[2]

        if (-not $isShader -and $GlHeaders -contains $name -and $rel -ine $GlHome) {
            Add-Violation 'gl' $rel $n "includes <$name>; include engine/core/gl.h instead"
        }
        $target = Resolve-Include $rel $name $isShader
        if (-not $target) {
            if ($isShader) { Add-Violation 'missing' $rel $n "includes `"$name`", which resolves to no file" }
            continue
        }
        $tmod = Get-Module $target
        if (-not $tmod) { continue }   # reported as unmapped on its own
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
    Write-Host "[modules] no violations ($($all.Count) files checked)"
    exit 0
}
foreach ($g in ($violations | Group-Object Rule | Sort-Object Name)) {
    Write-Host ""
    Write-Host "[$($g.Name)] $($g.Count)"
    foreach ($v in ($g.Group | Sort-Object File, Line)) {
        $loc = if ($v.Line) { "$($v.File):$($v.Line)" } else { $v.File }
        Write-Host ("  {0,-40} {1}" -f $loc, $v.Msg)
    }
}
Write-Host ""
Write-Host "[modules] $($violations.Count) violations in $($all.Count) files"
if ($Strict) { exit 1 }
exit 0
