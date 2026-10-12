@echo off
rem Пересборка: нужен gcc из MinGW-w64 (например через MSYS2) в PATH
gcc -Os -fno-math-errno -fno-ident -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -mwindows -nostartfiles -Wl,-e,entry -Wl,--gc-sections -s -o TrayLauncher.exe TrayLauncher.c -luser32 -lshell32 -ladvapi32 -lkernel32
pause
