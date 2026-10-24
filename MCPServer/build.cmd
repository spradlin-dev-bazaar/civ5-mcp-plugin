@echo off
rem Builds the plugin DLL, then the mod folder in Build\ and its package in Packages\ with ModBuddy's build targets.
setlocal
if not defined BAZAAR_SDK set "BAZAAR_SDK=%~dp0.."
if not exist "%BAZAAR_SDK%\CivPlugin.vsprops" (echo Set BAZAAR_SDK to the Bazaar SDK folder. & exit /b 1)
if not exist "%VS90COMNTOOLS%vsvars32.bat" (echo Visual C++ 2008 was not found. & exit /b 1)
call "%VS90COMNTOOLS%vsvars32.bat" >nul
vcbuild /nologo "%~dp0src\MCPServer.vcproj" "Release|Win32" || exit /b 1
copy /y "%~dp0src\out\MCPServer.dll" "%~dp0mod\" >nul || exit /b 1

set "PROJECT=%~dp0mod\MCPServer.civ5proj"
set "MOD_NAME="
for /f "tokens=3 delims=<>" %%n in ('findstr /c:"<Name>" "%PROJECT%"') do if not defined MOD_NAME set "MOD_NAME=%%n"
if not defined MOD_NAME (echo No mod name was found in mod\MCPServer.civ5proj. & exit /b 1)
for /f "tokens=2,*" %%a in ('reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 8930" /v InstallLocation 2^>nul ^| findstr /c:"REG_SZ"') do set "CIV5_PATH=%%b"
if not defined CIV5_PATH (echo Civilization V was not found in the registry. & exit /b 1)
for /f "usebackq delims=" %%d in (`powershell -NoProfile -Command "[Environment]::GetFolderPath('MyDocuments')"`) do set "DOCUMENTS=%%d"
if not defined DOCUMENTS (echo Your Documents folder was not found. & exit /b 1)
"%WINDIR%\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe" "%PROJECT%" /t:Package /nologo /v:q ^
	"/p:Configuration=Package Only" "/p:SafeName=%MOD_NAME%" "/p:Civ5Path=%CIV5_PATH%" ^
	"/p:Civ5UserPath=%DOCUMENTS%\My Games\Sid Meier's Civilization 5"
endlocal & exit /b %ERRORLEVEL%
