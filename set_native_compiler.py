"""
set_native_compiler.py — PlatformIO pre-script for [env:native]
Sets the C/C++ compiler to the WinLibs GCC 15.2 installation added by winget.

winget modifies the user PATH but VS Code tasks run in cmd.exe which does not
reload the user PATH from the registry, so g++ is not found by default.
This script injects the GCC bin directory at runtime so PlatformIO can find it.
"""
Import("env")
import os

# Use the 8.3 short path — avoids spaces in "Mohamed Khaled" which break SCons
# AR/LINK command construction when the path is not quoted by PlatformIO.
WINLIBS_BIN = r"C:\Users\MOHAME~1\AppData\Local\MICROS~1\WinGet\Packages\BRECHT~1.SOU\mingw64\bin"

if os.path.isdir(WINLIBS_BIN):
    os.environ["PATH"] = WINLIBS_BIN + os.pathsep + os.environ.get("PATH", "")
    env.Replace(
        CC   = os.path.join(WINLIBS_BIN, "gcc.exe"),
        CXX  = os.path.join(WINLIBS_BIN, "g++.exe"),
        AR   = os.path.join(WINLIBS_BIN, "ar.exe"),
        LINK = os.path.join(WINLIBS_BIN, "g++.exe"),
        RANLIB = os.path.join(WINLIBS_BIN, "ranlib.exe"),
    )
    print("[native] Using WinLibs GCC 15.2 (short path, spaces-safe)")
else:
    print("[native] WARNING: WinLibs not found at expected path, falling back to system g++")
