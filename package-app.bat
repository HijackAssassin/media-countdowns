@echo off
rem ==========================================================================
rem  Media Countdowns' download. Website\"2 archive this build.bat" calls:
rem      Code\package-app.bat <version> "<project folder>"
rem  and gets Website\Media-Countdowns-V<version>-Setup.exe: the Inno Setup installer (Media Countdowns.iss
rem  here) of everything in Website\MediaCountdowns\ (the app + Notifier\, the copy Website\1 build.bat made
rem  from Build\). The name must not change - the website and every installed copy go by it.
rem  Needs Inno Setup 6.
rem ==========================================================================
setlocal
set "VERSION=%~1"
set "ROOT=%~2"
if "%VERSION%"=="" echo Run "2 archive this build.bat" in Website\, not this. & exit /b 1
set "WEB=%ROOT%\Website"
set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" echo Inno Setup 6 was not found: "%ISCC%" & exit /b 1
if not exist "%WEB%\MediaCountdowns\MediaCountdowns.exe" echo No Website\MediaCountdowns\MediaCountdowns.exe - run Website\1 build.bat first. & exit /b 1

"%ISCC%" /Q "/DAppVersion=%VERSION%" "/DSourceDir=%WEB%\MediaCountdowns" "/DOutDir=%WEB%" "%~dp0Media Countdowns.iss"
if errorlevel 1 exit /b 1
if not exist "%WEB%\Media-Countdowns-V%VERSION%-Setup.exe" echo Inno Setup did not make Media-Countdowns-V%VERSION%-Setup.exe & exit /b 1
exit /b 0
