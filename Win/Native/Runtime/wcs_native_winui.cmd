@echo off
setlocal
set "ROOT=%~dp0"
pushd "%ROOT%WcsNativeWinUI"
"%ROOT%WcsNativeWinUI\WcsNativeWinUI.exe" %*
set "EC=%ERRORLEVEL%"
popd
exit /b %EC%
