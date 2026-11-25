@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem  This is for whoever MAKES the release. Players never need any of this: they just run GoCodeInstaller.exe.
rem  (The GitHub Actions workflow in .github\workflows runs this script for you.)
rem
rem  Usage:  build.bat                  -> installer uses the payload folder next to it
rem          build.bat <url>            -> installer downloads gocode-payload.zip from <url> (the checksum is baked in
rem                                        automatically; falls back to the local payload folder if it fails)
set "URL=%~1"
set "SHA=%~2"

echo ==============================
echo   GoCode build
echo ==============================

where go >nul 2>nul
if errorlevel 1 (
  echo [!] Go is not installed ^(only needed to build the installer^).  Install it with:  winget install GoLang.Go
  goto :fail
)

set "CXX32="
where i686-w64-mingw32-g++ >nul 2>nul && set "CXX32=i686-w64-mingw32-g++"
if not defined CXX32 where g++ >nul 2>nul && set "CXX32=g++ -m32"
if not defined CXX32 (
  echo [!] No C++ compiler found.  Geometry Dash is 32-bit, so you need a 32-bit capable MinGW-w64.
  echo     Install WinLibs from https://winlibs.com ^(the i686 build^) and put its bin folder on PATH.
  goto :fail
)

echo [1/3] Building the loader ^(32-bit, no dependencies^) with: %CXX32%
if not exist payload\gocode\mods mkdir payload\gocode\mods
if not exist payload\gocode\config mkdir payload\gocode\config
%CXX32% -std=c++17 -O2 -s -shared -static -o payload\XInput9_1_0.dll loader\loader.cpp loader\gocode_lang.cpp loader\pattern.cpp loader\core_logic.cpp -Wl,--kill-at
if errorlevel 1 (
  echo [!] Loader build failed.  If the error mentions "-m32" or "skipping incompatible", your MinGW has no
  echo     32-bit support - install the i686 build of WinLibs and put it first on PATH.
  goto :fail
)
copy /y loader\gocode.ini payload\gocode\gocode.ini >nul
copy /y mods\*.gocode payload\gocode\mods\ >nul
copy /y mods\config\*.cfg payload\gocode\config\ >nul

echo [2/3] Packaging...
if not exist dist mkdir dist
if exist dist\payload rmdir /s /q dist\payload
xcopy /e /i /y /q payload dist\payload >nul
if exist dist\gocode-payload.zip del /q dist\gocode-payload.zip
powershell -NoProfile -Command "Compress-Archive -Path 'payload\*' -DestinationPath 'dist\gocode-payload.zip' -Force"
for /f %%H in ('powershell -NoProfile -Command "(Get-FileHash dist\gocode-payload.zip -Algorithm SHA256).Hash"') do set "ZIPSHA=%%H"
if "%SHA%"=="" set "SHA=%ZIPSHA%"

echo [3/3] Building the installer ^(one self-contained exe^)...
pushd installer
go build -trimpath -ldflags "-s -w -X main.loaderURL=%URL% -X main.loaderSHA256=%SHA%" -o ..\dist\GoCodeInstaller.exe .
set "GOERR=%errorlevel%"
popd
if not "%GOERR%"=="0" (
  echo [!] Installer build failed.
  goto :fail
)

echo.
echo Done!
echo   dist\GoCodeInstaller.exe     what players run ^(keep dist\payload next to it unless you built with a URL^)
echo   dist\gocode-payload.zip      what the installer downloads when built with a URL
echo.
if not defined CI pause
exit /b 0

:fail
echo.
if not defined CI pause
exit /b 1
