@echo off
rem Wacki: wyciaga filmy (Dane_NN.dta) do klatek PNG + WAV. Polozyc obok plikow gry.
cd /d "%~dp0"
where py >nul 2>nul && (py -3 wacki_filmy.py %* & goto :eof)
where python >nul 2>nul && (python wacki_filmy.py %* & goto :eof)
echo Nie ma Pythona. Zainstaluj go z https://www.python.org/downloads/ (zaznacz "Add python.exe to PATH").
pause
