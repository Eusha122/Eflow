@echo off
cd /d "%~dp0"
rem Speakers (default): Solu never listens while it talks. Headphones: add --headphones to talk over it. Voice: --voice cozy|bubbly|tiny|calm|natural.
venv\Scripts\python.exe solu_chat.py %*
pause
