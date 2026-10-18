# 内存增长判定：翻译多次，看私有提交是"一次性"还是"每次累加"
# 这是决定能否优化的关键实验。

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class MM{
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]static extern bool EnumWindows(EnumProc cb,IntPtr l);
 [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")]public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")]public static extern void mouse_event(uint f,uint x,uint y,uint d,UIntPtr e);
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [StructLayout(LayoutKind.Sequential)]public struct KI{public ushort vk;public ushort scan;public uint flags;public uint time;public IntPtr extra;}
 [StructLayout(LayoutKind.Sequential)]public struct IN{public uint type;public KI ki;public int p1;public int p2;}
 [DllImport("user32.dll")]static extern uint SendInput(uint n,IN[] a,int cb);

 public static IntPtr Find(uint pid,string cls){
   IntPtr f=IntPtr.Zero;
   EnumWindows((h,l)=>{uint p;GetWindowThreadProcessId(h,out p);
     if(p==pid){var sb=new StringBuilder(256);GetClassNameW(h,sb,256);
       if(sb.ToString()==cls){f=h;return false;}}
     return true;},IntPtr.Zero);
   return f;
 }
 public static IntPtr FindPrefix(string prefix){
   IntPtr f=IntPtr.Zero;
   EnumWindows((h,l)=>{var sb=new StringBuilder(256);GetClassNameW(h,sb,256);
     if(sb.ToString().StartsWith(prefix)){f=h;return false;} return true;},IntPtr.Zero);
   return f;
 }
 public static void Keys(params ushort[] ks){
   var l=new System.Collections.Generic.List<IN>();
   foreach(var k in ks){var i=new IN();i.type=1;i.ki.vk=k;l.Add(i);}
   for(int n=ks.Length-1;n>=0;n--){var i=new IN();i.type=1;i.ki.vk=ks[n];i.ki.flags=2;l.Add(i);}
   SendInput((uint)l.Count,l.ToArray(),Marshal.SizeOf(typeof(IN)));
 }
 public static void Drag(int x1,int y1,int x2,int y2){
   SetCursorPos(x1,y1); System.Threading.Thread.Sleep(200);
   mouse_event(0x0002,0,0,0,UIntPtr.Zero); System.Threading.Thread.Sleep(160);
   for(int i=1;i<=6;i++){ SetCursorPos(x1+(x2-x1)*i/6, y1+(y2-y1)*i/6); System.Threading.Thread.Sleep(45); }
   System.Threading.Thread.Sleep(120); mouse_event(0x0004,0,0,0,UIntPtr.Zero);
 }
}
"@

function Mem($tag){
  $p=Get-Process LightTransAI -ErrorAction SilentlyContinue
  if($p){ Write-Host ("  {0,-24} 私有提交 {1,7:N2} MB   工作集 {2,7:N2} MB" -f $tag,($p.PrivateMemorySize64/1MB),($p.WorkingSet64/1MB)) }
}

$pid_=(Get-Process LightTransAI).Id
$main=[MM]::Find([uint32]$pid_,'LightTransAI_Main')

Mem "刚启动（闲置）"

# 打开/关闭设置窗口：验证 D2D 初始化的一次性成本
[void][MM]::SendMessageW($main,0x0111,[IntPtr]2011,[IntPtr]0)
Start-Sleep -Seconds 3
Mem "打开设置窗口后"
$set=[MM]::FindPrefix('LightTransAI_Settings')
if($set -ne [IntPtr]::Zero){ [void][MM]::SendMessageW($set,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) }
Start-Sleep -Seconds 1
Mem "关闭设置窗口后"

# 反复框选翻译 3 次
for($i=1;$i -le 3;$i++){
  Write-Host ""
  Write-Host "=== 第 $i 次框选翻译 ==="
  [MM]::Keys(0x12,0x51)
  Start-Sleep -Seconds 2
  [MM]::Drag(500,300,1200,600)
  Start-Sleep -Seconds 7
  Mem "第 $i 次完成后"
}
