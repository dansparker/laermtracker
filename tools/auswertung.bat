@echo off
rem ---------------------------------------------------------------------------
rem  Laermtracker - Auswertung starten (Doppelklick) oder Datenordner auf diese
rem  Datei ziehen. Ohne Angabe oeffnet sich ein Dialog zur Ordnerauswahl.
rem  Beim ersten Start werden pandas und matplotlib automatisch installiert.
rem ---------------------------------------------------------------------------
setlocal

rem Python finden: erst "py" (Windows-Launcher), dann "python"
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY (
    echo Python wurde nicht gefunden.
    echo Bitte Python 3 von https://www.python.org/downloads/ installieren und
    echo dabei "Add python.exe to PATH" ankreuzen. Danach diese Datei erneut starten.
    pause
    exit /b 1
)

if /i "%~1"=="/test" (
    echo Python-Aufruf: %PY%
    %PY% --version
    exit /b 0
)

rem Pakete pruefen und bei Bedarf installieren
%PY% -c "import pandas, matplotlib" >nul 2>nul
if errorlevel 1 (
    echo Installiere benoetigte Pakete ^(pandas, matplotlib^) ...
    %PY% -m pip install --user pandas matplotlib
    if errorlevel 1 (
        echo.
        echo Die Installation der Pakete ist fehlgeschlagen. Internetverbindung pruefen
        echo oder manuell ausfuehren:  pip install pandas matplotlib
        pause
        exit /b 1
    )
)

if "%~1"=="" (
    %PY% "%~dp0auswertung.py"
) else (
    %PY% "%~dp0auswertung.py" %*
)
if errorlevel 1 (
    echo.
    echo Die Auswertung wurde mit einem Fehler beendet.
)
echo.
pause
