@echo off
rem Mine on a pool: set POOL and WALLET below, then run start.bat
set POOL=pool.example.com:3333
set WALLET=0xYourParallaxAddress
set WORKER=%COMPUTERNAME%

cd /d "%~dp0"
hashwarp.exe --pool %POOL% --wallet %WALLET% --worker %WORKER% %*
pause
