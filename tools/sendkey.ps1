# sendkey.ps1 -- post a WM_KEYDOWN/WM_KEYUP pair into the game window (no focus steal).
# The plugin's window subclass consumes the keys it handles; the game itself ignores the keyboard.
param([Parameter(Mandatory=$true)][string]$vk, [int]$holdMs = 40, [switch]$up)

$sig = @'
using System; using System.Text; using System.Runtime.InteropServices;
public class SK {
 [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr p);
 [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 delegate bool EnumProc(IntPtr h, IntPtr p);
 public static IntPtr FindMain(int pid) { IntPtr res=IntPtr.Zero; EnumWindows((h,p)=>{ int q; GetWindowThreadProcessId(h,out q); if(q==pid){ var c=new StringBuilder(64); GetClassNameW(h,c,64); if(c.ToString()=="TVPMainWindow") res=h; } return true; }, IntPtr.Zero); return res; }
}
'@
if(-not ("SK" -as [type])){ Add-Type -TypeDefinition $sig -Language CSharp }

$proc = Get-Process SenrenBanka -ErrorAction SilentlyContinue | Select-Object -First 1
if(-not $proc){ Write-Output "game not running"; exit 1 }
$hwnd = [SK]::FindMain($proc.Id)
if($hwnd -eq [IntPtr]::Zero){ Write-Output "main window not found"; exit 1 }

$code = [int]$vk
$kd = [IntPtr]0x00000001
$ku = [IntPtr]0xC0000001
if($up){
  [SK]::PostMessage($hwnd, 0x0101, [IntPtr]$code, $ku) | Out-Null
  Write-Output ("KEYUP vk=0x{0:X}" -f $code)
} else {
  [SK]::PostMessage($hwnd, 0x0100, [IntPtr]$code, $kd) | Out-Null
  Start-Sleep -Milliseconds $holdMs
  [SK]::PostMessage($hwnd, 0x0101, [IntPtr]$code, $ku) | Out-Null
  Write-Output ("KEY vk=0x{0:X} held ${holdMs}ms" -f $code)
}
