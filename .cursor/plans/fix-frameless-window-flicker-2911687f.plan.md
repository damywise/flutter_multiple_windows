<!-- 2911687f-5fe2-4d1e-8416-f0f72a5d2f0f 7cf560ac-f3af-43dd-b0e1-5b472d0508f3 -->
# Add Transparent Background Method

## Goal

Implement a native method to set window background color to transparent for Flutter multi-window applications, allowing frameless windows with transparent backgrounds.

## Implementation Approach

Use the `SetWindowCompositionAttribute` API with `ACCENT_ENABLE_TRANSPARENTGRADIENT` state, following the proven approach from `window_manager` (lines 662-716 of window_manager.cpp).

## Changes Required

### 1. Native C++ Implementation (windows/runner/main.cpp)

Add new MethodChannel handler `setBackgroundColor` that:

- Takes ARGB color values as parameters
- Uses `SetWindowCompositionAttribute` with `ACCENT_ENABLE_TRANSPARENTGRADIENT` for transparency
- Properly loads user32.dll and GetProcAddress for the undocumented API
- Handles error cases gracefully

**Key Implementation Details:**

```cpp
// Define structures for undocumented API
typedef enum _ACCENT_STATE {
  ACCENT_DISABLED = 0,
  ACCENT_ENABLE_GRADIENT = 1,
  ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
  ACCENT_ENABLE_BLURBEHIND = 3,
  ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
  ACCENT_ENABLE_HOSTBACKDROP = 5,
  ACCENT_INVALID_STATE = 6
} ACCENT_STATE;

struct ACCENTPOLICY {
  int nAccentState;
  int nFlags;
  int nColor;
  int nAnimationId;
};

struct WINCOMPATTRDATA {
  int nAttribute;
  PVOID pData;
  ULONG ulDataSize;
};
```

### 2. Dart Service Layer (lib/app/window_service.dart)

Add Flutter method:

```dart
static Future<bool> setBackgroundColor(int hwnd, {
  required int alpha,
  required int red,
  required int green,
  required int blue,
}) async {
  // Call native method with ARGB values
}
```

### 3. UI Integration (lib/app/main_window.dart)

Add button to test transparent background functionality.

## Technical Notes

- `SetWindowCompositionAttribute` is an undocumented Windows API
- For full transparency: A=0, R=0, G=0, B=0
- Color format: `(A << 24) + (B << 16) + (G << 8) + R`
- Attribute code 19 is used for `WCA_ACCENT_POLICY`
- Must dynamically load from user32.dll for compatibility

### To-dos

- [ ] Implement setBackgroundColor MethodChannel handler in main.cpp with SetWindowCompositionAttribute API
- [ ] Add setBackgroundColor method to WindowService in window_service.dart
- [ ] Add button in main_window.dart to test transparent background
- [ ] Test transparent background with frameless window