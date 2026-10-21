@echo off
rem ==========================================================================
rem  Media Countdowns' build steps. build.bat (in the project folder) calls:
rem      Code\build-app.bat <version> "<project folder>"
rem  and gets:
rem    Build\MediaCountdowns.exe (+ Qt files)                    the app
rem    Build\Notifier\MediaCountdownsNotifier.exe (+ Qt files)   the notifier, where the app looks for it
rem
rem  The version goes into MC_APP_VERSION (Manager\src\appversion.h) and MC_NOTIFIER_VERSION
rem  (Notifier\src\notifierversion.h) - the app and notifier ship together and track each other on
rem  purpose; CMake reads the number from those headers into the exe's version resource, and the app's
rem  update check compares it with the website's.
rem
rem  Needs Qt 6.7.3 for MinGW plus the MinGW, CMake and Ninja that come with it (C:\Qt). Builds in
rem  %TEMP%\media-countdowns-build. Close Media Countdowns and its notifier first if they are running
rem  from Build\. The installer is made by package-app.bat, not here.
rem  Manager\src\relaysecret.local.h (git-ignored) bakes Patrick's relay into the app when it is there;
rem  without it the app builds with blank relay defaults (see Manager\src\relayconfig.h).
rem ==========================================================================
setlocal
set "VERSION=%~1"
set "ROOT=%~2"
if "%VERSION%"=="" echo Run build.bat in the project folder, not this. & exit /b 1
set "CODE=%ROOT%\Code"
set "BUILD=%ROOT%\Build"
set "WORK=%TEMP%\media-countdowns-build"
set "QT=C:\Qt\6.7.3\mingw_64"
rem Qt's own compiler, CMake and Ninja first, whatever else is on PATH.
set "PATH=C:\Qt\Tools\mingw1120_64\bin;C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;%QT%\bin;%PATH%"

if not exist "%QT%\bin\windeployqt.exe" (
    echo Qt 6.7.3 for MinGW was not found: %QT%
    exit /b 1
)

rem Into both headers (only rewritten when the number changed, so an unchanged version doesn't make
rem CMake recompile them).
powershell -NoProfile -Command "$q = [char]34; foreach ($h in @(@('%CODE%\Manager\src\appversion.h','MC_APP_VERSION'), @('%CODE%\Notifier\src\notifierversion.h','MC_NOTIFIER_VERSION'))) { $t = [IO.File]::ReadAllText($h[0]); $p = '(#define ' + $h[1] + ' )\S+'; if ($t -notmatch $p) { exit 1 }; $n = [regex]::Replace($t, $p, '${1}' + $q + '%VERSION%' + $q); if ($n -ne $t) { [IO.File]::WriteAllText($h[0], $n) } }"
if errorlevel 1 (
    echo Could not write the version into appversion.h / notifierversion.h.
    exit /b 1
)

powershell -NoProfile -Command "if (Get-Process MediaCountdowns, MediaCountdownsNotifier -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '%BUILD%\*' }) { exit 1 }"
if errorlevel 1 (
    echo Media Countdowns or its notifier is running from Build\. Close them, then build again.
    exit /b 1
)

rem A CMake build folder made for another source folder (the project was moved) cannot be reused.
set "CODE_FWD=%CODE:\=/%"
for %%c in (Manager Notifier) do if exist "%WORK%\%%c\CMakeCache.txt" (
    findstr /i /l /c:"CMAKE_HOME_DIRECTORY:INTERNAL=%CODE_FWD%/%%c" "%WORK%\%%c\CMakeCache.txt" >nul || rmdir /s /q "%WORK%\%%c"
)
echo Building the app ...
cmake -S "%CODE%\Manager" -B "%WORK%\Manager" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=%QT%" || exit /b 1
cmake --build "%WORK%\Manager" || exit /b 1
echo Building the notifier ...
cmake -S "%CODE%\Notifier" -B "%WORK%\Notifier" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=%QT%" || exit /b 1
cmake --build "%WORK%\Notifier" || exit /b 1

rem Build\ starts empty, so nothing from an older build stays behind (and ends up in the installer).
if exist "%BUILD%" rmdir /s /q "%BUILD%"
if exist "%BUILD%" echo Could not empty Build\ - is something in it open? & exit /b 1
mkdir "%BUILD%\Notifier"
copy /y "%WORK%\Manager\MediaCountdowns.exe" "%BUILD%\" >nul || exit /b 1
copy /y "%WORK%\Notifier\MediaCountdownsNotifier.exe" "%BUILD%\Notifier\" >nul || exit /b 1
echo Deploying the Qt files ...
windeployqt --release "%BUILD%\MediaCountdowns.exe" >"%WORK%\deploy-app.log" 2>&1 || (type "%WORK%\deploy-app.log" & exit /b 1)
windeployqt --release "%BUILD%\Notifier\MediaCountdownsNotifier.exe" >"%WORK%\deploy-notifier.log" 2>&1 || (type "%WORK%\deploy-notifier.log" & exit /b 1)
exit /b 0
