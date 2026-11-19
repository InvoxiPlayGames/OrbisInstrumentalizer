@echo off
rem Duplo clique para instalar o OrbisInstrumentalizer no PS4 pela rede.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0instalar.ps1" %*
echo.
pause
