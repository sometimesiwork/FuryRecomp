@echo off
rem Start the statically recompiled Fury of the Furries and walk the
rem presentation into the first level (desert 1) automatically.
cd /d "%~dp0"
set FURY_AUTOSTART=1
start "" fury.exe
