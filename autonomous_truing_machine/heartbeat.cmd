@echo off
rem Live mic check: joins the board's Wi-Fi, waits for it, and prints ALIVE/DEAD for both mics.
rem Double-click it, or run it from anywhere. Ctrl+C stops it.
python "%~dp0tools\workbench.py" --heartbeat %*
pause
