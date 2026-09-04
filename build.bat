@echo off
REM derleme icin MSYS2 UCRT64 lazim, normal cmd'de gcc bulunamaz
setlocal
where gcc >nul 2>nul
if errorlevel 1 (
  echo HATA: gcc bulunamadi. MSYS2 UCRT64 kabugunda calistirin veya PATH'e ekleyin.
  exit /b 1
)
where windres >nul 2>nul
if errorlevel 1 (
  echo HATA: windres bulunamadi.
  exit /b 1
)
windres resource.rc -o resource.o
if errorlevel 1 exit /b 1
gcc -O3 -march=native -flto -s -municode -mwindows -Wall -Wextra -fstack-protector-strong -D_FORTIFY_SOURCE=2 -o HydraRemuxer.exe remux.c resource.o -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi
if errorlevel 1 exit /b 1
echo.
echo OK: HydraRemuxer.exe olustu.
