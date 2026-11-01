# Dev: drive/capture a running game window. Commands: shot|pshot <png>, key|pkey <vk-hex> <ms>,
# click|pclick <x> <y> [right], move|pmove <x> <y>, wheel <delta>, focus. p* variants post to the
# window directly (work behind other windows). SDL reads click positions from the real cursor,
# so pair pclick with move. $env:GAMEIDX picks the instance (0 = oldest).
# Usage: gamectl.ps1 shot <out.png> | key <vk-hex> <ms> | click <x> <y> [right] | move <x> <y> | focus
param([string]$cmd, [string]$a1, [string]$a2, [string]$a3)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  public static void Focus(IntPtr h) { keybd_event(0x12,0,0,UIntPtr.Zero); keybd_event(0x12,0,2,UIntPtr.Zero); ShowWindow(h, 9); SetForegroundWindow(h); }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
[void][W]::SetProcessDPIAware()
$idx = 0; if ($env:GAMEIDX) { $idx = [int]$env:GAMEIDX }
$p = @(Get-Process devilutionx -ErrorAction Stop | Sort-Object StartTime)[$idx]
$h = $p.MainWindowHandle
$r = New-Object W+RECT; [void][W]::GetClientRect($h, [ref]$r)
$o = New-Object W+POINT; [void][W]::ClientToScreen($h, [ref]$o)
switch ($cmd) {
  'focus' { [W]::Focus($h) }
  'shot' {
    $bmp = New-Object System.Drawing.Bitmap ($r.R), ($r.B)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($o.X, $o.Y, 0, 0, $bmp.Size)
    $bmp.Save($a1, [System.Drawing.Imaging.ImageFormat]::Png); "saved $a1 ($($r.R)x$($r.B))"
  }
  'key' {
    [W]::Focus($h); Start-Sleep -Milliseconds 100
    $vk = [Convert]::ToByte($a1, 16); $sc = [byte][W]::MapVirtualKey($vk, 0)
    [W]::keybd_event($vk, $sc, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds ([int]$a2)
    [W]::keybd_event($vk, $sc, 2, [UIntPtr]::Zero)
  }
  'wheel' { [W]::Focus($h); Start-Sleep -Milliseconds 100; [W]::mouse_event(0x0800,0,0,[uint32]([int]$a1),[UIntPtr]::Zero) }
  'pshot' {
    $bmp = New-Object System.Drawing.Bitmap ($r.R), ($r.B)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    [void][W]::PrintWindow($h, $hdc, 3); $g.ReleaseHdc($hdc)
    $bmp.Save($a1, [System.Drawing.Imaging.ImageFormat]::Png); "saved $a1"
  }
  'pkey' {
    $vk = [Convert]::ToInt32($a1, 16); $sc = [W]::MapVirtualKey([uint32]$vk, 0)
    [void][W]::PostMessage($h, 0x100, [IntPtr]$vk, [IntPtr](1 -bor ($sc -shl 16))); Start-Sleep -Milliseconds ([int]$a2)
    [void][W]::PostMessage($h, 0x101, [IntPtr]$vk, [IntPtr](1 -bor ($sc -shl 16) -bor 0xC0000000))
  }
  'pclick' {
    $lp = [IntPtr](([int]$a2 -shl 16) -bor ([int]$a1 -band 0xFFFF))
    [void][W]::PostMessage($h, 0x200, [IntPtr]0, $lp); Start-Sleep -Milliseconds 60
    if ($a3 -eq 'right') { [void][W]::PostMessage($h, 0x204, [IntPtr]2, $lp); Start-Sleep -Milliseconds 60; [void][W]::PostMessage($h, 0x205, [IntPtr]0, $lp) }
    else { [void][W]::PostMessage($h, 0x201, [IntPtr]1, $lp); Start-Sleep -Milliseconds 60; [void][W]::PostMessage($h, 0x202, [IntPtr]0, $lp) }
  }
  'pmove' { $lp = [IntPtr](([int]$a2 -shl 16) -bor ([int]$a1 -band 0xFFFF)); [void][W]::PostMessage($h, 0x200, [IntPtr]0, $lp) }
  'move' { [void][W]::SetCursorPos($o.X + [int]$a1, $o.Y + [int]$a2) }
  'click' {
    [W]::Focus($h); Start-Sleep -Milliseconds 100
    [void][W]::SetCursorPos($o.X + [int]$a1, $o.Y + [int]$a2); Start-Sleep -Milliseconds 80
    if ($a3 -eq 'right') { [W]::mouse_event(8,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [W]::mouse_event(16,0,0,0,[UIntPtr]::Zero) }
    else { [W]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [W]::mouse_event(4,0,0,0,[UIntPtr]::Zero) }
  }
}
