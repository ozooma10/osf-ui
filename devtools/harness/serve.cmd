@echo off
rem OSF UI dev harness — serve over http and open the Mods page.
rem (http is needed so the mock bridge can fetch the shipped settings schemas.)
rem Root = the Starfield modding dir (one above the repo), so the harness can
rem also reach sibling-repo views ("OSF Animation\views\osf" on osf.html).
cd /d "%~dp0..\..\.."
start "" "http://localhost:8080/OSF%%20UI/devtools/harness/index.html"
where py >nul 2>nul && (py -3 -m http.server 8080) || (python -m http.server 8080)
