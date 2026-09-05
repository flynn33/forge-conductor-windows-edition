# P01-A: Discover actual Windows/VS/MSVC/SDK/CMake/vcpkg/WinUI tools - Facts

## VERIFIED

- **CMake 3.28** is installed and available at `C:\Program Files\CMake\bin\cmake.exe`
- **PowerShell** is installed and available at `C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe`
- **Git** is installed and available at `C:\Program Files\Git\cmd\git.exe`
- **MSVC Compiler (cl.exe)** is installed in multiple versions:
  - Visual Studio Community 2022 (14.16.27023, 14.29.30133, 14.44.35207, 14.51.36231, 14.50.35717)
  - Visual Studio Insiders (14.16.27023, 14.29.30133, 14.44.35207, 14.50.35717)
- **Visual Studio 17 2022 generator** is available (required by CMakeLists.txt)
- **v143 platform toolset** is available (required by CMakeLists.txt)
- **Windows SDK requirements**: The CMakeLists.txt requires Windows SDK 10.0.26100.0
- **Target architecture**: C++20 with WinUI 3/C++/WinRT, Windows App SDK

## INFERRED

- **Visual Studio Community** is installed with MSVC compiler toolchain
- **Windows App SDK** may be available but not found in standard locations
- **vcpkg** package manager is not installed (not required by the CMakeLists.txt but may be needed for dependencies)

## UNKNOWN

- Exact Windows SDK version 10.0.26100.0 location and installation status
- Whether Windows App SDK is properly installed
- If vcpkg is available for dependency management

## BLOCKED

- Cannot fully verify Windows SDK 10.0.26100.0 installation without additional discovery tools
- Cannot verify exact Windows App SDK version availability