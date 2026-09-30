# Genera assets\perfoverlay.ico (16..256 px) e assets\perfoverlay-256.png.
# Stile delle altre app: quadrato scuro arrotondato + simbolo nel colore d'accento.
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$accent = [System.Drawing.Color]::FromArgb(59, 158, 255)

function New-IconFrame([int]$size) {
  $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'
  $g.PixelOffsetMode = 'HighQuality'
  $g.Clear([System.Drawing.Color]::Transparent)

  $s = [double]$size
  $pad = [Math]::Max(0.5, $s * 0.03)
  $rect = New-Object System.Drawing.RectangleF ([float]$pad), ([float]$pad), ([float]($s - 2 * $pad)), ([float]($s - 2 * $pad))
  $rad = [float]($s * 0.22)
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $d = $rad * 2
  $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
  $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
  $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
  $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
  $path.CloseFigure()
  $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, [System.Drawing.Color]::FromArgb(38, 40, 50),
    [System.Drawing.Color]::FromArgb(15, 15, 17), 90.0)
  $g.FillPath($bg, $path)
  if ($size -ge 32) {
    $g.DrawPath((New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(64, 66, 80)), ([float]($s / 128))), $path)
  }

  # Grafico FPS: area sfumata + linea spessa con estremi arrotondati.
  $pts = @(@(0.20, 0.70), @(0.37, 0.52), @(0.52, 0.62), @(0.68, 0.36), @(0.81, 0.44))
  $pf = foreach ($p in $pts) { New-Object System.Drawing.PointF ([float]($p[0] * $s)), ([float]($p[1] * $s)) }
  if ($size -ge 24) {
    $area = New-Object System.Drawing.Drawing2D.GraphicsPath
    $area.AddLines([System.Drawing.PointF[]]$pf)
    $area.AddLine($pf[-1], (New-Object System.Drawing.PointF $pf[-1].X, ([float]($s * 0.80))))
    $area.AddLine((New-Object System.Drawing.PointF $pf[-1].X, ([float]($s * 0.80))), (New-Object System.Drawing.PointF $pf[0].X, ([float]($s * 0.80))))
    $area.CloseFigure()
    $ab = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.RectangleF 0, ([float]($s * 0.3)), ([float]$s), ([float]($s * 0.5))),
      [System.Drawing.Color]::FromArgb(110, $accent), [System.Drawing.Color]::FromArgb(0, $accent), 90.0)
    $g.FillPath($ab, $area)
  }
  $w = [float][Math]::Max(1.6, $s * 0.085)
  $pen = New-Object System.Drawing.Pen $accent, $w
  $pen.StartCap = 'Round'; $pen.EndCap = 'Round'; $pen.LineJoin = 'Round'
  $g.DrawLines($pen, [System.Drawing.PointF[]]$pf)
  # punto "live" all'estremità
  $r = [float][Math]::Max(1.2, $s * 0.065)
  $g.FillEllipse([System.Drawing.Brushes]::White, $pf[-1].X - $r, $pf[-1].Y - $r, 2 * $r, 2 * $r)
  $g.Dispose()
  return $bmp
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$pngs = foreach ($sz in $sizes) {
  $b = New-IconFrame $sz
  $ms = New-Object System.IO.MemoryStream
  $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  if ($sz -eq 256) { $b.Save((Join-Path $dir 'perfoverlay-256.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
  $b.Dispose()
  , $ms.ToArray()
}

# ICO con frame PNG (supportato da Windows Vista in poi)
$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $out
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $sz = $sizes[$i]; $len = $pngs[$i].Length
  $bw.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz }))); $bw.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz })))
  $bw.Write([byte]0); $bw.Write([byte]0); $bw.Write([UInt16]1); $bw.Write([UInt16]32)
  $bw.Write([UInt32]$len); $bw.Write([UInt32]$offset)
  $offset += $len
}
foreach ($p in $pngs) { $bw.Write($p) }
[System.IO.File]::WriteAllBytes((Join-Path $dir 'perfoverlay.ico'), $out.ToArray())
"OK: perfoverlay.ico ($($sizes -join ', ') px)"
