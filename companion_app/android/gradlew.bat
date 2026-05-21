@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "GRADLE_BAT="
for /f "delims=" %%I in ('powershell -NoProfile -Command "Get-ChildItem -Path $env:USERPROFILE\.gradle\wrapper\dists -Recurse -Filter gradle.bat -File | Select-Object -First 1 -ExpandProperty FullName"') do (
  set "GRADLE_BAT=%%I"
)

:found
if not defined GRADLE_BAT (
  echo Could not find a local Gradle distribution under %%USERPROFILE%%\.gradle\wrapper\dists. 1>&2
  exit /b 1
)

set "JAVA_HOME=C:\Program Files\Microsoft\jdk-17.0.18.8-hotspot"
set "PATH=%JAVA_HOME%\bin;%PATH%"
set "GRADLE_USER_HOME=C:\tmp\spectre-gradle"
set "GRADLE_OPTS=%GRADLE_OPTS% -Dorg.gradle.native=false"
call "%GRADLE_BAT%" --no-daemon %*
exit /b %ERRORLEVEL%
