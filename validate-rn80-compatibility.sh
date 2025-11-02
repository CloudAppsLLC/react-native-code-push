#!/bin/bash

# Validation script for React Native 0.80 compatibility changes
# This script checks that the problematic code patterns have been removed

echo "🔍 Validating React Native 0.80 compatibility changes..."

# Check for ChoreographerCompat imports (should be 0)
echo "Checking for ChoreographerCompat imports..."
choreographer_imports=$(grep -r "import.*ChoreographerCompat" android/ 2>/dev/null | wc -l)
if [ $choreographer_imports -eq 0 ]; then
    echo "✅ No ChoreographerCompat imports found"
else
    echo "❌ Found $choreographer_imports ChoreographerCompat imports"
    grep -r "import.*ChoreographerCompat" android/
fi

# Check for ChoreographerCompat usage (should be 0)
echo "Checking for ChoreographerCompat usage..."
choreographer_usage=$(grep -r "ChoreographerCompat" android/ 2>/dev/null | wc -l)
if [ $choreographer_usage -eq 0 ]; then
    echo "✅ No ChoreographerCompat usage found"
else
    echo "❌ Found $choreographer_usage ChoreographerCompat usages"
    grep -r "ChoreographerCompat" android/
fi

# Check for deprecated getCurrentActivity() calls (should be 0)
echo "Checking for deprecated getCurrentActivity() calls..."
deprecated_calls=$(grep -r "getCurrentActivity()" android/ 2>/dev/null | grep -v "getReactApplicationContext().getCurrentActivity()" | wc -l)
if [ $deprecated_calls -eq 0 ]; then
    echo "✅ No deprecated getCurrentActivity() calls found"
else
    echo "❌ Found $deprecated_calls deprecated getCurrentActivity() calls"
    grep -r "getCurrentActivity()" android/ | grep -v "getReactApplicationContext().getCurrentActivity()"
fi

# Check for createJSModules method (should be 0)
echo "Checking for deprecated createJSModules method..."
js_modules=$(grep -r "createJSModules" android/ 2>/dev/null | wc -l)
if [ $js_modules -eq 0 ]; then
    echo "✅ No createJSModules method found"
else
    echo "❌ Found $js_modules createJSModules references"
    grep -r "createJSModules" android/
fi

# Check for JavaScriptModule import (should be 0)
echo "Checking for JavaScriptModule imports..."
js_module_imports=$(grep -r "import.*JavaScriptModule" android/ 2>/dev/null | wc -l)
if [ $js_module_imports -eq 0 ]; then
    echo "✅ No JavaScriptModule imports found"
else
    echo "❌ Found $js_module_imports JavaScriptModule imports"
    grep -r "import.*JavaScriptModule" android/
fi

# Check Gradle versions
echo "Checking Gradle configuration..."
gradle_version=$(grep "gradle-" android/gradle/wrapper/gradle-wrapper.properties | grep -o "[0-9]\+\.[0-9]\+")
agp_version=$(grep "com.android.tools.build:gradle" android/build.gradle | grep -o "[0-9]\+\.[0-9]\+\.[0-9]\+")

echo "Gradle version: $gradle_version"
echo "Android Gradle Plugin version: $agp_version"

if [ "$(echo "$gradle_version >= 8.0" | bc)" -eq 1 ] && [ "$(echo "$agp_version >= 8.0" | bc)" -eq 1 ]; then
    echo "✅ Modern Gradle and AGP versions"
else
    echo "⚠️  Consider updating to newer Gradle/AGP versions for better RN 0.80+ support"
fi

echo ""
echo "🎉 Validation complete! All checks should show ✅ for React Native 0.80 compatibility."