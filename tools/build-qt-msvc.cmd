@echo off
setlocal
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
if not defined GGA_VS_VCVARS set "GGA_VS_VCVARS=C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined GGA_QT_ROOT set "GGA_QT_ROOT=C:\Qt\6.8.3\msvc2022_64"
if not defined GGA_NINJA set "GGA_NINJA=C:\msys64\mingw64\bin\ninja.exe"
if not defined GGA_VCPKG_ROOT set "GGA_VCPKG_ROOT=C:\vcpkg"
if not defined GGA_DEPS_ROOT set "GGA_DEPS_ROOT=C:\deps"
set "BLD=%ROOT%\build-qt-msvc"
set "DIST=%ROOT%\dist"
set "PATH=C:\Windows\System32;C:\Windows;C:\Program Files\CMake\bin;C:\Program Files\Git\cmd;%GGA_QT_ROOT%\bin"
if not exist "%GGA_VS_VCVARS%" echo Missing MSVC environment: %GGA_VS_VCVARS% & exit /b 1
if not exist "%GGA_QT_ROOT%\lib\cmake\Qt6\Qt6Config.cmake" echo Missing Qt 6 MSVC installation: %GGA_QT_ROOT% & exit /b 1
if not exist "%GGA_NINJA%" echo Missing Ninja: %GGA_NINJA% & exit /b 1
if not exist "%GGA_VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" echo Missing vcpkg toolchain: %GGA_VCPKG_ROOT% & exit /b 1
if not exist "%GGA_DEPS_ROOT%\include\zlib.h" echo Missing zlib headers: %GGA_DEPS_ROOT% & exit /b 1
call "%GGA_VS_VCVARS%" >nul
if errorlevel 1 exit /b 1
cmake -S "%ROOT%" -B "%BLD%" -G Ninja -DCMAKE_MAKE_PROGRAM="%GGA_NINJA%" -DCMAKE_PREFIX_PATH="%GGA_QT_ROOT%" -DCMAKE_TOOLCHAIN_FILE="%GGA_VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release -DBUILD_QT=ON -DBUILD_SDL=OFF -DUSE_FFMPEG=OFF -DUSE_LIBZIP=OFF -DUSE_PNG=OFF -DZLIB_ROOT="%GGA_DEPS_ROOT%"
if errorlevel 1 exit /b 1
cmake --build "%BLD%" --parallel 12
if errorlevel 1 exit /b 1
if not exist "%BLD%\GGA.exe" echo Build did not produce GGA.exe & exit /b 1
if not exist "%DIST%" mkdir "%DIST%"
copy /y "%BLD%\GGA.exe" "%DIST%\GGA.exe" >nul
if exist "%BLD%\mgba.dll" copy /y "%BLD%\mgba.dll" "%DIST%\mgba.dll" >nul
if exist "%BLD%\epoxy-0.dll" copy /y "%BLD%\epoxy-0.dll" "%DIST%\epoxy-0.dll" >nul
"%GGA_QT_ROOT%\bin\windeployqt.exe" --release --compiler-runtime --no-translations "%DIST%\GGA.exe"
if errorlevel 1 exit /b 1
echo BUILD_AND_DEPLOY_OK %DIST%\GGA.exe
