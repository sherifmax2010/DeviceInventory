DEVICE INVENTORY - GITHUB ACTIONS BUILD FILES

Files:
- DeviceInventory.cpp: Native C++ source
- DeviceInventory.vcxproj: x86 project configured for v141_xp
- .github/workflows/build.yml: GitHub Actions workflow

How to use:
1. Create a new GitHub repository.
2. Upload the files preserving the .github/workflows/build.yml folder structure.
3. Open the repository's Actions tab.
4. Select "Build DeviceInventory (x86, Windows XP toolset)".
5. Click "Run workflow".
6. If the runner has the v141_xp toolset, the build will complete.
7. Download the artifact named DeviceInventory-x86-XP-toolset.

Important:
GitHub hosted Windows runners do not guarantee the Visual Studio v141_xp optional component.
The workflow explicitly checks for it and fails if it is missing. It does not silently build a newer EXE and claim XP compatibility.
If it fails at the toolset check, the source is still buildable, but the workflow needs a Windows build environment with Visual Studio 2017 C++ v141_xp installed (for example, a self-hosted runner).

This project file is configured to target Windows XP SP3 (5.01) using the v141_xp toolset.
The generated executable must still be tested on real XP SP3, Windows 7, Windows 10 and Windows 11 machines.

Known device-detection limitation:
On XP, WMI does not expose a universally reliable SSD/HDD media type. The current source only calls a disk SSD when its model text contains SSD, Solid State, or NVMe. Otherwise it reports Unknown.
