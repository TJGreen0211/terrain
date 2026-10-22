<#
.SYNOPSIS
    Visual regression check for cube.exe's bench scenes.

.DESCRIPTION
    Renders each scene with --shots at fixed frames into bench/cand, then
    compares every candidate against bench/reference/<same name>.png with
    block-SSIM and PSNR (grayscale, downsampled 320x180). Small numeric
    drift is tolerated; real image changes fail. LOD popping and shimmering
    are caught because every shot frame is compared, not just one.

    NEVER edit the reference images or this script to make a check pass.
    Regenerate references only on purpose, with human review:
        powershell -File bench\visual_check.ps1 -UpdateRefs

    After -UpdateRefs the protected-files manifest is re-sealed automatically.

.EXAMPLE
    powershell -File bench\visual_check.ps1
    powershell -File bench\visual_check.ps1 -Scenes grazing-sea
#>
param(
    [string[]]$Scenes = $null,
    [int]$Frames = 0,                 # 0 = scene default; raised to cover the shots
    [int[]]$Shots = @(150, 300, 450),
    [double]$MinSsim = 0.995,
    [double]$MinPsnr = 30.0,
    [switch]$UpdateRefs,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$bench = $PSScriptRoot
$root = Split-Path -Parent $bench
$exe = Join-Path $root 'src\cube.exe'
$refDir = Join-Path $bench 'reference'
$candDir = Join-Path $bench 'cand'

$src = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class ImgDiff {
    public static double CompareSsim(string a, string b, int w, int h) {
        double[] ga = Gray(a, w, h), gb = Gray(b, w, h);
        double sum = 0; int blocks = 0;
        int bw = w / 8, bh = h / 8;
        const double C1 = 6.5025;    // (0.01*255)^2
        const double C2 = 58.5225;   // (0.03*255)^2
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
            double ma = 0, mb = 0;
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
                ma += ga[(by * 8 + y) * w + bx * 8 + x];
                mb += gb[(by * 8 + y) * w + bx * 8 + x];
            }
            ma /= 64.0; mb /= 64.0;
            double va = 0, vb = 0, cab = 0;
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
                double da = ga[(by * 8 + y) * w + bx * 8 + x] - ma;
                double db = gb[(by * 8 + y) * w + bx * 8 + x] - mb;
                va += da * da; vb += db * db; cab += da * db;
            }
            va /= 63.0; vb /= 63.0; cab /= 63.0;
            sum += ((2 * ma * mb + C1) * (2 * cab + C2)) /
                   ((ma * ma + mb * mb + C1) * (va + vb + C2));
            blocks++;
        }
        return sum / blocks;
    }

    public static double ComparePsnr(string a, string b, int w, int h) {
        double[] ga = Gray(a, w, h), gb = Gray(b, w, h);
        double mse = 0;
        for (int i = 0; i < ga.Length; i++) { double d = ga[i] - gb[i]; mse += d * d; }
        mse /= ga.Length;
        if (mse <= 0) return 999;
        return 10.0 * Math.Log10(255.0 * 255.0 / mse);
    }

    static double[] Gray(string path, int w, int h) {
        using (Bitmap bmp = new Bitmap(path))
        using (Bitmap small = new Bitmap(w, h)) {
            using (Graphics g = Graphics.FromImage(small)) g.DrawImage(bmp, 0, 0, w, h);
            Rectangle rect = new Rectangle(0, 0, w, h);
            BitmapData d = small.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
            byte[] buf = new byte[d.Stride * h];
            Marshal.Copy(d.Scan0, buf, 0, buf.Length);
            small.UnlockBits(d);
            double[] outp = new double[w * h];
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                int o = y * d.Stride + x * 3;
                outp[y * w + x] = 0.114 * buf[o] + 0.587 * buf[o + 1] + 0.299 * buf[o + 2];
            }
            return outp;
        }
    }
}
'@
Add-Type -TypeDefinition $src -ReferencedAssemblies System.Drawing

if (-not $SkipBuild) {
    Write-Host "[visual] building..."
    cmd /c "`"$root\build.bat`""
    if ($LASTEXITCODE -ne 0) { Write-Host "[visual] build failed"; exit 1 }
}
if (-not (Test-Path $exe)) { Write-Host "[visual] missing $exe"; exit 1 }

if (-not $Scenes) { $Scenes = (Get-ChildItem (Join-Path $bench 'scenes\*.bench')).BaseName }
if (-not $Scenes) { Write-Host "[visual] no scenes"; exit 1 }

$framesEff = $Frames
if ($framesEff -le 0) { $framesEff = ($Shots | Measure-Object -Maximum).Maximum + 30 }

New-Item -ItemType Directory -Force -Path $refDir, (Join-Path $bench 'out') | Out-Null
if (Test-Path $candDir) { Remove-Item -Recurse -Force $candDir }
New-Item -ItemType Directory -Force -Path $candDir | Out-Null

foreach ($scene in $Scenes) {
    $shotArg = ($Shots -join ',')
    $errFile = Join-Path $candDir "$scene.err.txt"
    cmd /c "`"$exe`" --bench $scene --frames $framesEff --shots $shotArg --shots-dir `"$candDir`" 2> `"$errFile`""
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[visual] $scene render FAILED (exit $LASTEXITCODE)"
        Get-Content $errFile -ErrorAction SilentlyContinue | Select-Object -Last 12 | ForEach-Object { Write-Host "    $_" }
        exit 1
    }
}

if ($UpdateRefs) {
    foreach ($png in (Get-ChildItem (Join-Path $candDir '*.png'))) {
        Copy-Item $png.FullName (Join-Path $refDir $png.Name) -Force
    }
    Write-Host "[visual] references updated from $candDir ($((Get-ChildItem $refDir -Filter *.png).Count) pngs)"
    & (Join-Path $bench 'bench.ps1') -SealManifest | Out-Null
    exit 0
}

$fail = $false
Write-Host ""
Write-Host ("{0,-34} {1,8} {2,8}" -f 'image', 'ssim', 'psnr')
foreach ($png in (Get-ChildItem (Join-Path $candDir '*.png') | Sort-Object Name)) {
    $ref = Join-Path $refDir $png.Name
    if (-not (Test-Path $ref)) {
        Write-Host ("{0,-34} MISSING REFERENCE" -f $png.Name)
        $fail = $true
        continue
    }
    $ssim = [ImgDiff]::CompareSsim($ref, $png.FullName, 320, 180)
    $psnr = [ImgDiff]::ComparePsnr($ref, $png.FullName, 320, 180)
    $tag = ''
    if ($ssim -lt $MinSsim -or $psnr -lt $MinPsnr) { $tag = 'FAIL'; $fail = $true }
    Write-Host ("{0,-34} {1,8:N4} {2,8:N1} {3}" -f $png.Name, $ssim, $psnr, $tag)
}
if ($fail) {
    Write-Host "[visual] VISUAL REGRESSION (min ssim $MinSsim, min psnr $MinPsnr dB). Revert or review."
    exit 1
}
Write-Host "[visual] all frames match the references."
exit 0
