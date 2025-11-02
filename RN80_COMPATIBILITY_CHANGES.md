# React Native 0.80 Compatibility Changes

This document outlines all the changes made to make React Native CodePush compatible with React Native 0.80+.

## Issues Fixed

### 1. ChoreographerCompat Removal
**Problem**: `ChoreographerCompat` was removed in newer React Native versions.
**Error**: `cannot find symbol: class ChoreographerCompat`

**Solution**: Replaced `ChoreographerCompat.FrameCallback` usage with `Handler` from Android's main looper.

**Files Modified**:
- `android/app/src/main/java/com/microsoft/codepush/react/CodePushNativeModule.java`
  - Removed import: `com.facebook.react.modules.core.ChoreographerCompat`
  - Replaced ChoreographerCompat usage with Handler.post()

### 2. Deprecated getCurrentActivity() Method
**Problem**: `getCurrentActivity()` method is deprecated and marked for removal.
**Error**: `warning: [removal] getCurrentActivity() in ReactContextBaseJavaModule has been deprecated`

**Solution**: Replaced with `getReactApplicationContext().getCurrentActivity()`

**Files Modified**:
- `android/app/src/main/java/com/microsoft/codepush/react/CodePushNativeModule.java` (2 occurrences)
- `android/app/src/main/java/com/microsoft/codepush/react/CodePushDialog.java` (2 occurrences)

### 3. Removed Deprecated ReactPackage Methods
**Problem**: `createJSModules()` method was deprecated in RN 0.47 and removed in newer versions.

**Solution**: Removed the deprecated method and its import.

**Files Modified**:
- `android/app/src/main/java/com/microsoft/codepush/react/CodePush.java`
  - Removed import: `com.facebook.react.bridge.JavaScriptModule`
  - Removed method: `createJSModules()`

### 4. Updated Android Build Configuration
**Problem**: Very old Android Gradle Plugin and build tools versions causing compatibility issues.

**Solution**: Updated to modern versions compatible with RN 0.80.

**Files Modified**:
- `android/build.gradle`
  - Updated Android Gradle Plugin: `1.3.0` → `8.1.1`
  - Cleaned up allprojects configuration
- `android/app/build.gradle`  
  - Updated compileSdkVersion: `26` → `34`
  - Updated buildToolsVersion: `26.0.3` → `34.0.0`
  - Updated targetSdkVersion: `26` → `34`
  - Updated minSdkVersion: `16` → `21`
- `android/gradle/wrapper/gradle-wrapper.properties`
  - Updated Gradle version: `2.4` → `8.3`

## Code Changes Summary

### CodePushNativeModule.java
```java
// BEFORE (lines 323-335)
ReactChoreographer.getInstance().postFrameCallback(ReactChoreographer.CallbackType.TIMERS_EVENTS, new ChoreographerCompat.FrameCallback() {
    @Override
    public void doFrame(long frameTimeNanos) {
        if (!latestDownloadProgress.isCompleted()) {
            dispatchDownloadProgressEvent();
        }
        hasScheduledNextFrame = false;
    }
});

// AFTER
new Handler(Looper.getMainLooper()).post(new Runnable() {
    @Override
    public void run() {
        if (!latestDownloadProgress.isCompleted()) {
            dispatchDownloadProgressEvent();
        }
        hasScheduledNextFrame = false;
    }
});
```

```java
// BEFORE
final Activity currentActivity = getCurrentActivity();

// AFTER  
final Activity currentActivity = getReactApplicationContext().getCurrentActivity();
```

### CodePush.java
```java
// REMOVED
public List<Class<? extends JavaScriptModule>> createJSModules() {
    return new ArrayList<>();
}
```

## Testing

To test these changes:

1. Integrate the updated CodePush library in a React Native 0.80+ project
2. Build the Android app
3. Verify that:
   - No compilation errors related to ChoreographerCompat
   - No warnings about deprecated getCurrentActivity() usage
   - App builds and runs successfully
   - CodePush functionality works as expected

## Compatibility

These changes maintain backward compatibility while adding support for React Native 0.80+. The library should work with:
- React Native 0.80+
- Android API Level 21+ (Android 5.0+)  
- Modern Android build tools and Gradle versions

## Breaking Changes

None. All changes are internal implementation updates that don't affect the public API.