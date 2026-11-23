# Adapting explorer7 to Windows 11 24H2 for the Windows Vista Shell Experience

This document details the adaptations made to `explorerwrapper` (the engine behind `explorer7`), how it resolves Windows 11 24H2 (Build 26100) compatibility hurdles, and how to configure the shell for an authentic Windows Vista look and feel.

---

## 1. Technical Changes for Windows 11 24H2 (Build 26100)

Windows 11 24H2 introduced significant internal refactoring to `twinui.pcshell.dll`, `shell32.dll`, and COM registrations. The following changes have been applied:

1. **Suppressed Unsupported OS Warning (`explorerwrapper/util.h`)**:
   - Removed the `BuildNumber >= 26100` condition from `FirstRunCompatibilityWarning()`, ensuring 24H2 is recognized as supported without throwing premature warning dialogs.
2. **Fixed Taskbar Pinning Bypass for Vista (`explorerwrapper/dllmain.cpp` & `explorerwrapper/OptionConfig.cpp`)**:
   - In 24H2, `CLSID_TaskbarPin` is removed/refactored, causing queries to hang or fail.
   - For Windows Vista shell mode, taskbar pinning ("Superbar") did not exist. Setting `UseTaskbarPinning = 0` (now default on Build 26100+) skips `CLSID_TaskbarPin` COM calls cleanly, returning `E_FAIL` immediately and preventing shell initialization crashes.
3. **Shell HotKey Hooking (`explorerwrapper/AddressImports.h`)**:
   - In 24H2, shell hotkeys (including the Windows Key) are handled by `twinui.pcshell.dll`. Added interception for `twinui.pcshell.dll` alongside `twinui.dll` to prevent start menu hotkey double-registration.
4. **Shell32 Pin Check Patch (`explorerwrapper/shell32_wrappers.cpp`)**:
   - Lifted the `< 26100` constraint so memory patches apply properly across 24H2 `shell32.dll` builds.

---

## 2. Directory Layout for Windows Vista Shell

When preparing your runtime deployment folder (e.g. `C:\explorerVista`), organize the files as follows:

```
explorerVista/
├── explorer.exe              (Windows 7 SP1 x64 explorer.exe patched to import wrp64.dll)
├── wrp64.dll                 (Compiled from this repository)
├── VistaShellConfig.reg      (Registry settings for Vista shell behavior)
├── en-US/                    (or your system's language code, e.g. es-ES)
│   ├── explorer.exe.mui      (From the original Windows source media)
│   └── shell32.dll.mui       (From the original Windows source media)
├── theme/
│   ├── vista.msstyles        (Windows Vista aero.msstyles or custom Vista msstyles)
│   └── en-US/
│       └── vista.msstyles.mui
└── orbs/
    └── vista/
        ├── 6801.bmp          (100% DPI Vista Start Orb - 52x162, 3 frames)
        ├── 6802.bmp          (125% DPI Vista Start Orb - 66x198, 3 frames)
        ├── 6803.bmp          (150% DPI Vista Start Orb - 81x243, 3 frames)
        └── 6804.bmp          (190% DPI Vista Start Orb - 106x318, 3 frames)
```

---

## 3. Patching `explorer.exe` (PE Import Redirection)

1. Obtain `explorer.exe` (x64) from Windows 7 SP1 (or Windows Vista SP2 x64).
2. Open `explorer.exe` in **CFF Explorer** (or Resource Hacker / PE editor).
3. Navigate to **Import Directory**.
4. Rename the following imported DLL names to redirect them to `wrp64.dll`:
   - `SHLWAPI.DLL` -> `wrp64.dll`
   - `OLE32.DLL`   -> `wrp64.dll`
   - `EXPLORERFRAME.DLL` -> `wrp64.dll` (if present)
5. Save the modified executable as `explorer.exe` alongside `wrp64.dll`.

---

## 4. Applying Vista Registry Settings

Import `VistaShellConfig.reg` by double-clicking it, or running:
```powershell
reg import VistaShellConfig.reg
```

Key values configured:
- `UseTaskbarPinning = 0`: Disables modern taskbar pinning, reproducing the classic Vista taskbar.
- `TaskbarGlomLevel = 2`: "Never combine" taskbar buttons (displays window titles on buttons).
- `TaskbarSmallIcons = 1`: Classic 30px taskbar height.
- `Theme = "vista"`: Loads `theme\vista.msstyles`.
- `OrbDirectory = "vista"`: Loads `orbs\vista\6801.bmp` (and DPI variants).
- `StoreAppsOnTaskbar = 0` & `StoreAppsInStart = 0`: Hides modern UWP apps from classic UI.

---

## 5. Compiling `wrp64.dll`

You can build `wrp64.dll` via:
- **GitHub Actions**: Push changes to GitHub (`git push origin main`); the `.github/workflows/msbuild.yml` action will compile Release x64 and produce `wrp64.dll` as an artifact.
- **Visual Studio / MSBuild** (if installed):
  ```cmd
  msbuild explorerwrapper.sln /p:Configuration=Release /p:Platform=x64
  ```
  The compiled binary will be placed in `Release\wrp64.dll`.

---

## 6. Testing & Running Safely

1. Open Task Manager (`Ctrl + Shift + Esc`).
2. Test the new shell side-by-side or launch it directly:
   ```cmd
   explorerVista\explorer.exe
   ```
3. To test as your main shell without overriding system defaults, pass the path through Task Manager or configure it through your preferred shell switcher.
