@echo off
rem NSIS !finalize / !uninstfinalize hook (see CPACK_NSIS_DEFINES in CMakeLists.txt):
rem Authenticode-sign the file NSIS just wrote -- the uninstaller it generates, then
rem the finished installer.
rem
rem Inert unless the ATN_SIGN environment variable names the signing script, so a
rem build without a signing identity packages exactly as before.
if not defined ATN_SIGN exit /b 0
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ATN_SIGN%" "%~1"
exit /b %ERRORLEVEL%
