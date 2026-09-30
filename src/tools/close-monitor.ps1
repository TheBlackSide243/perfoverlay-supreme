# Chiude PerfOverlaySupreme.exe (anche se gira come amministratore) inviandogli il messaggio di uscita.
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class PoQuit {
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, int m, IntPtr w, IntPtr l);
}
"@
$h = [PoQuit]::FindWindow("PerfOverlaySupremeMonitor", [NullString]::Value)
if ($h -ne [IntPtr]::Zero) {
  [PoQuit]::PostMessage($h, 0x8004, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # kMsgQuit
  for ($i = 0; $i -lt 20 -and (Get-Process PerfOverlaySupreme -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 250 }
}
