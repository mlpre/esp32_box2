@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS /Imain main\game.c main\game_render.c main\game_selftest.c tools\game_preview.c /Fobuild\ /Febuild\game_preview.exe
if errorlevel 1 exit /b 1
build\game_preview.exe
