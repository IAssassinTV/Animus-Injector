@echo off
setlocal

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

echo building Animus Injector (%CONFIG%) with vs2022...

cmake -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config %CONFIG%

echo.
echo build complete: build\bin\%CONFIG%\AnimusInjector.asi + dinput8.dll
