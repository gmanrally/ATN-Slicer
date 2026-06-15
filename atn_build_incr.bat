@echo off
setlocal
set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Strawberry\perl\bin;C:\Strawberry\c\bin;%PATH%"
set CMAKE_POLICY_VERSION_MINIMUM=3.5
cd /d "c:\Users\Graham Work\OrcaSlicer\build"
"%CMAKE%" --build . --config Release --target OrcaSlicer -- -m
echo === build exit %ERRORLEVEL% ===
