# BDETECT.EXE static reconnaissance

Target: `work/ref/BDETECT.EXE` (PE32 GUI, MSVC 5.0 link, stripped).  
Methods: `sha256sum`, `objdump`, `strings`, PE/IAT parsing + `objdump -d` xref scan (no rizin/radare2 on host; Ghidra snap present, not run).

## Identity

| Field | Value |
|-------|-------|
| SHA-256 | `b56d1981369dc3166a710927e5673c807d25858bd33ccd0c6f04d7a329548bf3` |
| Size | 501248 bytes |
| Image base | `0x00400000` |
| Entry point | RVA `0x0005f530` → VA `0x0045f530` |
| Subsystem | Windows GUI (`0x2`) |
| Link timestamp | 1998-08-20 02:07:16 UTC (PE header) |

## Section table

| Name | VirtSize | VMA | File off | Align | Flags (objdump) |
|------|----------|-----|----------|-------|-----------------|
| `.text` | `0x0006ae46` | `0x00401000` | `0x00000400` | `2**2` | CODE, ALLOC, LOAD, READONLY |
| `.rdata` | `0x00002850` | `0x0046c000` | `0x0006b400` | `2**2` | ALLOC, LOAD, READONLY, DATA |
| `.data` | `0x0000b800` | `0x0046f000` | `0x0006de00` | `2**2` | ALLOC, LOAD, DATA |
| `.idata` | `0x00000e54` | `0x0048c000` | `0x00079600` | `2**2` | ALLOC, LOAD, DATA |

No `.rsrc`, `.reloc`, or debug directory (all data-directory entries zero).

## Import table (135 symbols, 8 DLLs)

| DLL | Count |
|-----|------:|
| KERNEL32.dll | 88 |
| USER32.dll | 24 |
| GDI32.dll | 16 |
| ADVAPI32.dll | 3 |
| comdlg32.dll | 1 |
| DDRAW.dll | 1 |
| DSOUND.dll | 1 |
| WINMM.dll | 1 |

### KERNEL32.dll (88)

Sleep, lstrlenA, TerminateThread, QueryPerformanceFrequency, GlobalFree, GlobalUnlock, ResumeThread, SuspendThread, GetLastError, DeleteFileA, OpenFile, GlobalReAlloc, _llseek, _lclose, _lread, GlobalHandle, GlobalAlloc, GlobalLock, ExitThread, LeaveCriticalSection, GetLocaleInfoA, ExitProcess, SetEndOfFile, RaiseException, GetStringTypeW, GetStringTypeA, LoadLibraryA, FlushFileBuffers, SetStdHandle, CreateFileA, IsBadWritePtr, IsBadReadPtr, IsBadCodePtr, SetEnvironmentVariableA, CompareStringW, GetFileType, GetStdHandle, SetUnhandledExceptionFilter, CreateThread, VirtualAlloc, GetOEMCP, GetACP, SetHandleCount, CompareStringA, GetLocaleInfoW, HeapFree, RtlUnwind, HeapAlloc, InterlockedDecrement, InterlockedIncrement, GetTimeZoneInformation, GetSystemTime, GetLocalTime, TlsSetValue, CloseHandle, GetModuleHandleA, GetStartupInfoA, GetCommandLineA, GetVersion, HeapDestroy, HeapCreate, VirtualFree, InitializeCriticalSection, DeleteCriticalSection, EnterCriticalSection, GetCurrentProcess, HeapReAlloc, GetEnvironmentStringsW, GetEnvironmentStrings, GetCurrentThreadId, TlsAlloc, SetLastError, TlsGetValue, ReadFile, GetProcAddress, SetFilePointer, TerminateProcess, LCMapStringW, GetCPInfo, HeapSize, WideCharToMultiByte, MultiByteToWideChar, LCMapStringA, WriteFile, UnhandledExceptionFilter, GetModuleFileNameA, FreeEnvironmentStringsA, FreeEnvironmentStringsW.

### USER32.dll (24)

VkKeyScanA, ReleaseDC, GetAsyncKeyState, GetCursorPos, SetCursorPos, PostMessageA, wsprintfA, MessageBoxA, PostQuitMessage, DestroyWindow, GetDC, ShowCursor, CreateWindowExA, ShowWindow, FindWindowExA, LoadCursorA, RegisterClassA, LoadIconA, WaitMessage, FindWindowA, DispatchMessageA, DefWindowProcA, PeekMessageA, TranslateMessage.

### GDI32.dll (16)

SetDIBitsToDevice, StartDocA, GetDeviceCaps, SetBkMode, SetTextColor, TextOutA, DeleteObject, CreateFontA, SelectObject, GetCharWidthA, StretchDIBits, DeleteDC, GetStockObject, StartPage, EndPage, EndDoc.

### ADVAPI32.dll (3)

RegCloseKey, RegQueryValueExA, RegOpenKeyExA.

### comdlg32.dll (1)

PrintDlgA.

### DDRAW.dll (1)

DirectDrawCreate.

### DSOUND.dll (1)

DirectSoundCreate.

### WINMM.dll (1)

timeGetTime.

## Imports actually referenced (IAT xrefs in `.text`)

Scan: any `objdump -d` operand in IAT VA range `0x0048c300`–`0x0048c600`. **All 135 imports in the eight DLLs above are referenced** (no dead IAT slots). First xref VA per symbol:

```
kernel32.dll
  Sleep: referenced @ 0x0040644e
  lstrlenA: referenced @ 0x004239c7
  TerminateThread: referenced @ 0x00427ca4
  QueryPerformanceFrequency: referenced @ 0x004287c9
  GlobalFree: referenced @ 0x0042ebce
  GlobalUnlock: referenced @ 0x0042ebc4
  ResumeThread: referenced @ 0x00453dcd
  SuspendThread: referenced @ 0x00453dfe
  GetLastError: referenced @ 0x0045f407
  DeleteFileA: referenced @ 0x00434c4c
  OpenFile: referenced @ 0x0042ec7b
  GlobalReAlloc: referenced @ 0x0042ed79
  _llseek: referenced @ 0x0042edaf
  _lclose: referenced @ 0x0042edcb
  _lread: referenced @ 0x0042ebe6
  GlobalHandle: referenced @ 0x0042ebba
  GlobalAlloc: referenced @ 0x0042ec5d
  GlobalLock: referenced @ 0x0042ecc0
  ExitThread: referenced @ 0x00406102
  LeaveCriticalSection: referenced @ 0x0045f80e
  GetLocaleInfoA: referenced @ 0x00469780
  ExitProcess: referenced @ 0x00462588
  SetEndOfFile: referenced @ 0x004680b9
  RaiseException: referenced @ 0x0046908b
  GetStringTypeW: referenced @ 0x0046798b
  GetStringTypeA: referenced @ 0x004679c0
  LoadLibraryA: referenced @ 0x00467cf3
  FlushFileBuffers: referenced @ 0x004658ce
  SetStdHandle: referenced @ 0x0046568b
  CreateFileA: referenced @ 0x00465b47
  IsBadWritePtr: referenced @ 0x00465140
  IsBadReadPtr: referenced @ 0x00465120
  IsBadCodePtr: referenced @ 0x0046515b
  SetEnvironmentVariableA: referenced @ 0x00469e5f
  CompareStringW: referenced @ 0x00469a0c
  GetFileType: referenced @ 0x00464d90
  GetStdHandle: referenced @ 0x00464dc8
  SetUnhandledExceptionFilter: referenced @ 0x004650e5
  CreateThread: referenced @ 0x00406163
  VirtualAlloc: referenced @ 0x0045f931
  GetOEMCP: referenced @ 0x00464b7d
  GetACP: referenced @ 0x00464b92
  SetHandleCount: referenced @ 0x00464e4e
  CompareStringA: referenced @ 0x004699cb
  GetLocaleInfoW: referenced @ 0x00469758
  HeapFree: referenced @ 0x0045c57f
  RtlUnwind: referenced @ 0x0046a690
  HeapAlloc: referenced @ 0x0045ccdc
  InterlockedDecrement: referenced @ 0x0045d1c7
  InterlockedIncrement: referenced @ 0x0045d1b4
  GetTimeZoneInformation: referenced @ 0x0045df6e
  GetSystemTime: referenced @ 0x0045df16
  GetLocalTime: referenced @ 0x0045df0b
  TlsSetValue: referenced @ 0x0045f460
  CloseHandle: referenced @ 0x0045f50a
  GetModuleHandleA: referenced @ 0x0045f676
  GetStartupInfoA: referenced @ 0x0045f655
  GetCommandLineA: referenced @ 0x0045f5be
  GetVersion: referenced @ 0x0045f556
  HeapDestroy: referenced @ 0x0045f738
  HeapCreate: referenced @ 0x0045f719
  VirtualFree: referenced @ 0x0045fa3f
  InitializeCriticalSection: referenced @ 0x0045f756
  DeleteCriticalSection: referenced @ 0x00467c5d
  EnterCriticalSection: referenced @ 0x0045f7ed
  GetCurrentProcess: referenced @ 0x004624f7
  HeapReAlloc: referenced @ 0x00462758
  GetEnvironmentStringsW: referenced @ 0x004647f3
  GetEnvironmentStrings: referenced @ 0x004647e7
  GetCurrentThreadId: referenced @ 0x00460bb2
  TlsAlloc: referenced @ 0x00460b76
  SetLastError: referenced @ 0x00460c49
  TlsGetValue: referenced @ 0x00460c00
  ReadFile: referenced @ 0x004612e6
  GetProcAddress: referenced @ 0x004617e5
  SetFilePointer: referenced @ 0x00461eb2
  TerminateProcess: referenced @ 0x004624fe
  LCMapStringW: referenced @ 0x004639d4
  GetCPInfo: referenced @ 0x004649ab
  HeapSize: referenced @ 0x004627e6
  WideCharToMultiByte: referenced @ 0x0046376e
  MultiByteToWideChar: referenced @ 0x00463a5f
  LCMapStringA: referenced @ 0x00463996
  WriteFile: referenced @ 0x00463df5
  UnhandledExceptionFilter: referenced @ 0x00464391
  GetModuleFileNameA: referenced @ 0x00464541
  FreeEnvironmentStringsA: referenced @ 0x00464900
  FreeEnvironmentStringsW: referenced @ 0x004648a7
user32.dll
  VkKeyScanA: referenced @ 0x00401c39
  ReleaseDC: referenced @ 0x0045374b
  GetAsyncKeyState: referenced @ 0x00401bf1
  GetCursorPos: referenced @ 0x004247c5
  SetCursorPos: referenced @ 0x00401946
  PostMessageA: referenced @ 0x00422370
  wsprintfA: referenced @ 0x0042ec93
  MessageBoxA: referenced @ 0x0042ecaa
  PostQuitMessage: referenced @ 0x00453dba
  DestroyWindow: referenced @ 0x0045347b
  GetDC: referenced @ 0x004536e6
  ShowCursor: referenced @ 0x004341cf
  CreateWindowExA: referenced @ 0x004536cf
  ShowWindow: referenced @ 0x00453a58
  FindWindowExA: referenced @ 0x00453703
  LoadCursorA: referenced @ 0x0045364c
  RegisterClassA: referenced @ 0x004536a0
  LoadIconA: referenced @ 0x0045365c
  WaitMessage: referenced @ 0x00453d48
  FindWindowA: referenced @ 0x00453620
  DispatchMessageA: referenced @ 0x00453d4f
  DefWindowProcA: referenced @ 0x00454061
  PeekMessageA: referenced @ 0x00453d41
  TranslateMessage: referenced @ 0x00453d3a
gdi32.dll
  SetDIBitsToDevice: referenced @ 0x0042ee6f
  StartDocA: referenced @ 0x0043812d
  GetDeviceCaps: referenced @ 0x004380b3
  SetBkMode: referenced @ 0x00423941
  SetTextColor: referenced @ 0x00423986
  TextOutA: referenced @ 0x004239ea
  DeleteObject: referenced @ 0x004236ce
  CreateFontA: referenced @ 0x0042371a
  SelectObject: referenced @ 0x0042375e
  GetCharWidthA: referenced @ 0x00423786
  StretchDIBits: referenced @ 0x0042ee3f
  DeleteDC: referenced @ 0x004381b0
  GetStockObject: referenced @ 0x00453675
  StartPage: referenced @ 0x00438138
  EndPage: referenced @ 0x00438168
  EndDoc: referenced @ 0x00438173
advapi32.dll
  RegCloseKey: referenced @ 0x004341ac
  RegQueryValueExA: referenced @ 0x0043419d
  RegOpenKeyExA: referenced @ 0x0043417b
comdlg32.dll
  PrintDlgA: referenced @ 0x0045c4f0
ddraw.dll
  DirectDrawCreate: referenced @ 0x0045c4fc
dsound.dll
  DirectSoundCreate: referenced @ 0x0045c4f6
winmm.dll
  timeGetTime: referenced @ 0x00401021
```

DirectDraw/DirectSound entry thunks: `DirectDrawCreate` / `DirectSoundCreate` imported at IAT `0x0048c314` / `0x0048c31c`, tail-called from `0x0045c4fc` / `0x0045c4f6`.

## How the executable locates game data

### Relative `data\` and `audio\` paths

- **311** distinct `data\…` path literals embedded (e.g. `data\a6.pal`, `data\magicext.cmp`, `data\wnda1a.mov`); **183** `audio\…` wave paths (e.g. `audio\area1.wav`).
- Paths are appended to a heap buffer at global `0x0047fc2c` via `0x0041ac00` / `0x0041ac50` (string concat used before `OpenFile` at `0x0042ec7b` and asset loaders).
- Pack/bitmap probe at `0x0042ec7b`: `OpenFile` + `_lread` checks `0x4d42` (`"BM"`) and size `0x28` at `0x0042ece1`–`0x0042ecf7`.

### Module directory (not CD-ROM APIs)

- `GetModuleFileNameA` at `0x00464541` fills buffer `0x0048a430` (max `0x104` bytes at `0x00464535`–`0x0046453a`).
- **No** import or string for `GetDriveType`, `GetVolumeInformation`, `mciSendString`, `cdaudio`, or `*.ini` files in the image.

### Registry (install gate)

Function `0x00434150` (called from `0x00434ccc` during startup):

| Step | VA | Detail |
|------|-----|--------|
| `RegOpenKeyExA` | `0x0043417b` | `hKey = 0x80000002` (`HKEY_LOCAL_MACHINE`), subkey VA `0x00475a54` → `SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\bdetect.exe`, `sam = 0x000f003f` |
| `RegQueryValueExA` | `0x0043419d` | value name VA `0x00475a4c` → **`Path`**, type/size buffers on stack |
| `RegCloseKey` | `0x004341ac` / `0x004341c0` | on success/failure |
| Failure UI | `0x004341cf`–`0x004341da` | `ShowCursor(1)`; string VA `0x00475a28` → `Detective Barbie(R) Not Installed!` |

Success path returns `1` at `0x004341b2`; failure returns `0` at `0x004341e2`.

### Writable sidecar files (strings only)

`SavGame%d.ar1` / `.ar2` / `.ar3`, `current.ar1`–`.ar3`, `GameHist.ory`, `data\NewGame.ar1`, `data\NewGame.db1`, etc. — used with `CreateFileA` / `WriteFile` / `DeleteFileA` (`DeleteFileA` xref `0x00434c4c`).

## Window class, title, display

| Item | VA / site | Value |
|------|-----------|-------|
| Window title string | `0x004760c0` | `Detective Barbie(R)` |
| Class/title pointer used by `CreateWindowExA` | `0x004760b4` → `0x004760c0` | same string for class and title at `0x00453666`–`0x004536cf` |
| `RegisterClassA` | `0x004536a0` | `WNDCLASS.style = 0x100b` at `0x004536688`; `lpfnWndProc = 0x00453da0` at `0x004536690` |
| `CreateWindowExA` | `0x004536cf` | `dwStyle = 0x80080000` (`0x004536c7`); size **640×480** (`push 0x280`, `push 0x1e0` at `0x004536bb`–`0x004536c0`) |
| `FindWindowA` (single-instance) | `0x00453620` | class/name from `0x004760b4` |

### DirectDraw display mode (`IDirectDraw::SetDisplayMode`, vtable +`0x54`)

Not imported by name; call at **`0x004537c0`**:

- Width **`0x280` (640)** — `push` at `0x004537ba`
- Height **`0x1e0` (480)** — `push` at `0x004537b5`
- BPP **`8`** — `movsx edx, byte [0x004760b0]` (`0x004537a3`), `shl edx, 3` (`0x004537af`); on-disk byte at `0x004760b0` is `1` → `1*8`

Related:

- `IDirectDraw::SetCooperativeLevel` vtable +`0x50` at **`0x00453787`**, flags **`0x13`** (`push 0x13` at `0x00453781`).
- `IDirectDraw::RestoreDisplayMode` vtable +`0x6c` at `0x00453408` / `0x00453417`.
- Failure string VA `0x00474f18`: `DirectDraw initialisation failed!!` (shown via `MessageBoxA` path near `0x004534d3`).

Audio timing: `timeGetTime` at `0x00401021`; samples loaded from `audio\*.wav` via `OpenFile`/`_lread` path, not MCI.

## Risks running from a read-only ISO

- **Install registry dependency**: Startup calls `0x00434150` (`0x00434ccc`). Without `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\bdetect.exe` value **`Path`**, the process shows `Detective Barbie(R) Not Installed!` (`0x00475a28`) and aborts the init path (`0x004341e2`).
- **CWD / exe-directory relative assets**: Hundreds of `data\` and `audio\` relative paths assume a writable or readable tree beside the executable; ISO mount point must be the working directory or the registry `Path` must point at the ISO root.
- **Save/delete I/O on read-only media**: `WriteFile` (`0x00463df5`), `CreateFileA` (`0x00465b47`), and `DeleteFileA` (`0x00434c4c`) back save slots (`SavGame*`, `current.ar*`, `GameHist.ory`, `data\NewGame.*`); these fail on a read-only volume.
- **Fullscreen DirectDraw**: `SetCooperativeLevel` with `0x13` at `0x00453787` requests exclusive/fullscreen-style cooperation; may conflict with modern compositors when launched from removable media.
- **No redistributable path fallback**: No `GetDriveType` / volume-label CD detection; only registry install path + module-relative files — copying `BDETECT.EXE` alone off the ISO without registry still hits the not-installed gate.
- **Printer dialog**: `PrintDlgA` (`0x0045c4f0`) plus GDI print APIs may invoke spooler/temp writes unrelated to the ISO.
