@echo off
rem Wacki: lista komnat do podpowiedzi. Polozyc obok plikow gry (Dane_*.dta).
cd /d "%~dp0"
where py >nul 2>nul && (py -3 wacki_napisy.py & goto :eof)
where python >nul 2>nul && (python wacki_napisy.py & goto :eof)
echo Nie ma Pythona. Zainstaluj go z https://www.python.org/downloads/ (zaznacz "Add python.exe to PATH").
pause
