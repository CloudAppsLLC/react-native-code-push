@echo off
REM Validation script for React Native 0.80 compatibility changes
REM This script checks that the problematic code patterns have been removed

echo 🔍 Validating React Native 0.80 compatibility changes...
echo.

REM Check for ChoreographerCompat imports (should be 0)
echo Checking for ChoreographerCompat imports...
findstr /s /m /c:"ChoreographerCompat" android\*.java >nul 2>&1
if %errorlevel% neq 0 (
    echo ✅ No ChoreographerCompat imports found
) else (
    echo ❌ Found ChoreographerCompat imports:
    findstr /s /n /c:"ChoreographerCompat" android\*.java
)
echo.

REM Check for deprecated getCurrentActivity calls
echo Checking for deprecated getCurrentActivity^(^) calls...
findstr /s /m /c:"getCurrentActivity()" android\*.java | findstr /v /c:"getReactApplicationContext().getCurrentActivity()" >nul 2>&1
if %errorlevel% neq 0 (
    echo ✅ No deprecated getCurrentActivity^(^) calls found
) else (
    echo ❌ Found deprecated getCurrentActivity^(^) calls:
    findstr /s /n /c:"getCurrentActivity()" android\*.java | findstr /v /c:"getReactApplicationContext().getCurrentActivity()"
)
echo.

REM Check for createJSModules method
echo Checking for deprecated createJSModules method...
findstr /s /m /c:"createJSModules" android\*.java >nul 2>&1
if %errorlevel% neq 0 (
    echo ✅ No createJSModules method found
) else (
    echo ❌ Found createJSModules method:
    findstr /s /n /c:"createJSModules" android\*.java
)
echo.

REM Check for JavaScriptModule imports
echo Checking for JavaScriptModule imports...
findstr /s /m /c:"JavaScriptModule" android\*.java >nul 2>&1
if %errorlevel% neq 0 (
    echo ✅ No JavaScriptModule imports found
) else (
    echo ❌ Found JavaScriptModule imports:
    findstr /s /n /c:"JavaScriptModule" android\*.java
)
echo.

REM Check build configuration
echo Checking Gradle configuration...
findstr /c:"gradle-" android\gradle\wrapper\gradle-wrapper.properties
findstr /c:"com.android.tools.build:gradle" android\build.gradle
echo.

echo 🎉 Validation complete! All checks should show ✅ for React Native 0.80 compatibility.
pause