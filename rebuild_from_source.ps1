param(
    [string]$OutApk = ""
)
$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutApk) {
    $OutApk = Join-Path $here "PiliPlus_VR_rebuilt.apk"
}

$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { "$env:LOCALAPPDATA\Android\Sdk" }
if (-not (Test-Path $sdk)) { throw "Android SDK not found at $sdk. Set ANDROID_HOME." }

$cmakeDir = Get-ChildItem "$sdk\cmake" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
$cmake = if ($cmakeDir) { Join-Path $cmakeDir "bin\cmake.exe" } else { (Get-Command cmake).Source }
$ninja = if ($cmakeDir -and (Test-Path (Join-Path $cmakeDir "bin\ninja.exe"))) { Join-Path $cmakeDir "bin\ninja.exe" } else { "ninja" }

$ndkDir = if (Test-Path "$sdk\ndk\28.2.13676358") { "$sdk\ndk\28.2.13676358" } else { Get-ChildItem "$sdk\ndk" -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName }
if (-not $ndkDir) { throw "Android NDK not found under $sdk\ndk" }
$toolchain = "$ndkDir\build\cmake\android.toolchain.cmake"
$strip = "$ndkDir\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"

$bt = if (Test-Path "$sdk\build-tools\36.0.0") { "$sdk\build-tools\36.0.0" } else { Get-ChildItem "$sdk\build-tools" -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName }
$androidJar = if (Test-Path "$sdk\platforms\android-34\android.jar") { "$sdk\platforms\android-34\android.jar" } else { Join-Path (Get-ChildItem "$sdk\platforms" -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName) "android.jar" }

$workTmp = Join-Path ([System.IO.Path]::GetTempPath()) ("pili_vr_rebuild_" + [System.Guid]::NewGuid().ToString("N").Substring(0, 8))
$nativeBuild = Join-Path $workTmp "native_build"
$tmpClasses = Join-Path $workTmp "classes"
$tmpDex = Join-Path $workTmp "dex"
$tmpTool = Join-Path $workTmp "tool"
if (Test-Path $workTmp) { Remove-Item $workTmp -Recurse -Force }
New-Item -ItemType Directory -Force $nativeBuild, $tmpClasses, $tmpDex, $tmpTool | Out-Null

Write-Host "[1/6] Building native libp5m-vr.so via CMake ($ndkDir)..."
& $cmake -S "$here\native" -B $nativeBuild -G Ninja `
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" `
    -DCMAKE_MAKE_PROGRAM="$ninja" `
    -DANDROID_ABI=arm64-v8a `
    -DANDROID_PLATFORM=android-29 `
    -DANDROID_STL=c++_shared `
    -DCMAKE_BUILD_TYPE=Release `
    -DOPENXR_DIR="$here\deps\openxr_aar"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& $cmake --build $nativeBuild -j 8
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

& $strip -s "$nativeBuild\libp5m-vr.so" -o "$here\apktool\lib\arm64-v8a\libp5m-vr.so"
if ($LASTEXITCODE -ne 0) { throw "llvm-strip failed" }

Write-Host "[2/6] Compiling Java VR sources..."
$cp = @(
    $androidJar,
    "$here\deps\stub_classes",
    "$here\deps\app_classes.jar",
    "$here\deps\tflite\lite\classes.jar",
    "$here\deps\tflite\gpu\classes.jar",
    "$here\deps\tflite\tensorflow-lite-api-2.16.1\classes.jar",
    "$here\deps\tflite\tensorflow-lite-gpu-api-2.16.1\classes.jar"
) -join ";"

$javaFiles = Get-ChildItem -Recurse "$here\java" -Filter "*.java" | ForEach-Object { $_.FullName }
& javac --release 11 -encoding UTF-8 -cp $cp -d $tmpClasses $javaFiles
if ($LASTEXITCODE -ne 0) { throw "javac failed" }

Write-Host "[3/6] Running d8 and baksmali -> apktool/smali..."
Add-Type -AssemblyName System.IO.Compression.FileSystem
$classesJar = Join-Path $workTmp "classes_in.jar"
if (Test-Path $classesJar) { Remove-Item $classesJar -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory($tmpClasses, $classesJar)
& "$bt\d8.bat" --min-api 29 --output $tmpDex $classesJar
if ($LASTEXITCODE -ne 0) { throw "d8 failed" }

$apktoolJar = "$here\tools\apktool.jar"
$disasmSrc = @(
    "import java.io.File;",
    "import com.android.tools.smali.baksmali.Baksmali;",
    "import com.android.tools.smali.baksmali.BaksmaliOptions;",
    "import com.android.tools.smali.dexlib2.DexFileFactory;",
    "import com.android.tools.smali.dexlib2.Opcodes;",
    "import com.android.tools.smali.dexlib2.dexbacked.DexBackedDexFile;",
    "public class Disasm {",
    "    public static void main(String[] args) throws Exception {",
    "        var container = DexFileFactory.loadDexContainer(new File(args[0]), (Opcodes) null);",
    "        var entry = container.getEntry((String) container.getDexEntryNames().get(0));",
    "        Baksmali.disassembleDexFile((DexBackedDexFile) entry.getDexFile(), new File(args[1]), 4, new BaksmaliOptions());",
    "    }",
    "}"
) -join "`n"
Set-Content -Path "$tmpTool\Disasm.java" -Value $disasmSrc -Encoding Ascii
& javac -cp $apktoolJar -d $tmpTool "$tmpTool\Disasm.java"
if ($LASTEXITCODE -ne 0) { throw "Disasm javac failed" }

Remove-Item "$here\apktool\smali\com\example\piliplus\vr\*.smali" -Force -ErrorAction SilentlyContinue
& java -cp "$tmpTool;$apktoolJar" Disasm "$tmpDex\classes.dex" "$here\apktool\smali"
if ($LASTEXITCODE -ne 0) { throw "Disasm failed" }

Write-Host "[4/6] Building APK with apktool..."
& java -jar $apktoolJar if "$here\tools\framework\1.apk"
& java -jar $apktoolJar if -t 127 "$here\tools\framework\127.apk"
$unsignedApk = Join-Path $workTmp "unsigned.apk"
$alignedApk = Join-Path $workTmp "aligned.apk"
Remove-Item "$here\apktool\build", "$here\apktool\dist" -Recurse -Force -ErrorAction SilentlyContinue
& java -jar $apktoolJar b "$here\apktool" -o $unsignedApk
if ($LASTEXITCODE -ne 0) { throw "apktool b failed" }

Write-Host "[5/6] Aligning and signing with cert/debug.keystore..."
& "$bt\zipalign.exe" -p -f 4 $unsignedApk $alignedApk
if ($LASTEXITCODE -ne 0) { throw "zipalign failed" }

& "$bt\apksigner.bat" sign --ks "$here\cert\debug.keystore" --ks-pass pass:android --key-pass pass:android --ks-key-alias androiddebugkey --out $OutApk $alignedApk
if ($LASTEXITCODE -ne 0) { throw "apksigner sign failed" }

& "$bt\apksigner.bat" verify --print-certs $OutApk
if ($LASTEXITCODE -ne 0) { throw "apksigner verify failed" }

Write-Host "[6/6] Cleaning temporary build directories..."
Remove-Item $workTmp -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$here\apktool\build", "$here\apktool\dist" -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "REBUILT_OK: $OutApk"