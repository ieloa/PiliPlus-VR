@echo off
setlocal
set HERE=%~dp0
set APKTOOL_JAR=%HERE%tools\apktool.jar

copy /Y "%HERE%prebuilt\libp5m-vr.so" "%HERE%apktool\lib\arm64-v8a\libp5m-vr.so" >nul

if "%ANDROID_HOME%"=="" (
  if "%ANDROID_SDK_ROOT%"=="" (
    set SDK=%LOCALAPPDATA%\Android\Sdk
  ) else (
    set SDK=%ANDROID_SDK_ROOT%
  )
) else (
  set SDK=%ANDROID_HOME%
)
set BT=%SDK%\build-tools\36.0.0
if not exist "%BT%\apksigner.bat" (
  echo Cannot find apksigner in %BT%
  echo Please set ANDROID_HOME to your Android SDK path.
  exit /b 1
)
java -jar "%APKTOOL_JAR%" if "%HERE%tools\framework\1.apk"
java -jar "%APKTOOL_JAR%" if -t 127 "%HERE%tools\framework\127.apk"
java -jar "%APKTOOL_JAR%" b "%HERE%apktool" -o "%HERE%unsigned.apk"
if errorlevel 1 goto fail
"%BT%\zipalign.exe" -p -f 4 "%HERE%unsigned.apk" "%HERE%aligned.apk"
if errorlevel 1 goto fail
call "%BT%\apksigner.bat" sign --ks "%HERE%cert\debug.keystore" --ks-pass pass:android --key-pass pass:android --ks-key-alias androiddebugkey --out "%HERE%PiliPlus_VR_rebuilt.apk" "%HERE%aligned.apk"
if errorlevel 1 goto fail
call "%BT%\apksigner.bat" verify --print-certs "%HERE%PiliPlus_VR_rebuilt.apk"
if errorlevel 1 goto fail
del "%HERE%unsigned.apk" "%HERE%aligned.apk"
echo Done: PiliPlus_VR_rebuilt.apk
exit /b 0
:fail
echo Rebuild failed.
exit /b 1