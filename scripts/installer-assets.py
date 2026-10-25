#!/usr/bin/env python3
"""Render the product artwork into WiX bitmaps, preserving its aspect ratio."""
from pathlib import Path
import subprocess

# Windows ships System.Drawing; use it without adding a Python imaging dependency.
subprocess.run(['powershell', '-NoProfile', '-Command', r'''
Add-Type -AssemblyName System.Drawing
$logo = [System.Drawing.Image]::FromFile((Resolve-Path 'installer/driver_logo.png'))
foreach ($spec in @(@('banner.bmp',493,58,437,7,44), @('dialog.bmp',493,312,22,30,120))) {
  $bmp = New-Object System.Drawing.Bitmap($spec[1], $spec[2])
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.Clear([System.Drawing.Color]::White)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.DrawImage($logo, [int]$spec[3], [int]$spec[4], [int]$spec[5], [int]$spec[5])
  $g.Dispose()
  $bmp.Save((Join-Path (Resolve-Path installer) $spec[0]), [System.Drawing.Imaging.ImageFormat]::Bmp)
  $bmp.Dispose()
}
$logo.Dispose()
if (-not (Test-Path 'installer/driver.ico')) {
  $image = [System.Drawing.Image]::FromFile((Resolve-Path 'installer/driver_logo.png'))
  $bitmap = New-Object System.Drawing.Bitmap($image, 128, 128)
  $icon = [System.Drawing.Icon]::FromHandle($bitmap.GetHicon())
  $stream = [System.IO.File]::Create((Join-Path (Resolve-Path installer) 'driver.ico'))
  $icon.Save($stream)
  $stream.Dispose()
  $icon.Dispose()
  $bitmap.Dispose()
  $image.Dispose()
}
'''], check=True)
license_text = Path('LICENSE').read_text().replace('\\', '\\\\').replace('{', '\\{').replace('}', '\\}').replace('\n', '\\par\n')
Path('installer/license.rtf').write_text('{\\rtf1\\ansi\\deff0 {\\fonttbl {\\f0 Consolas;}}\\f0\\fs18 ' + license_text + '}')
