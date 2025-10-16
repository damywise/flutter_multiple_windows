// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <flutter/dart_project.h>
#include <flutter/flutter_engine.h>
#include <flutter/generated_plugin_registrant.h>
#include <flutter/plugin_registrar_windows.h>

#include <iostream>
#include <vector>
#include <dwmapi.h>
#include <map>
#include <algorithm>

#include "utils.h"

#include "../flutter/ephemeral/cpp_client_wrapper/include/flutter/method_channel.h"
#include "../flutter/ephemeral/cpp_client_wrapper/include/flutter/standard_method_codec.h"

#pragma comment(lib, "dwmapi.lib")

// Custom window message for deferred window processing
#define WM_FLUTTER_WINDOW_CREATED (WM_APP + 1)

// Timer ID for delayed window setup
#define TIMER_AUTOSETUP_WINDOW 1001

// ============================================================================
// WINDOWS COMPOSITION ATTRIBUTE STRUCTURES FOR TRANSPARENCY
// ============================================================================

typedef enum _WINDOWCOMPOSITIONATTRIB {
  WCA_UNDEFINED = 0,
  WCA_NCRENDERING_ENABLED = 1,
  WCA_NCRENDERING_POLICY = 2,
  WCA_TRANSITIONS_FORCEDISABLED = 3,
  WCA_ALLOW_NCPAINT = 4,
  WCA_CAPTION_BUTTON_BOUNDS = 5,
  WCA_NONCLIENT_RTL_LAYOUT = 6,
  WCA_FORCE_ICONIC_REPRESENTATION = 7,
  WCA_EXTENDED_FRAME_BOUNDS = 8,
  WCA_HAS_ICONIC_BITMAP = 9,
  WCA_THEME_ATTRIBUTES = 10,
  WCA_NCRENDERING_EXILED = 11,
  WCA_NCADORNMENTINFO = 12,
  WCA_EXCLUDED_FROM_LIVEPREVIEW = 13,
  WCA_VIDEO_OVERLAY_ACTIVE = 14,
  WCA_FORCE_ACTIVEWINDOW_APPEARANCE = 15,
  WCA_DISALLOW_PEEK = 16,
  WCA_CLOAK = 17,
  WCA_CLOAKED = 18,
  WCA_ACCENT_POLICY = 19,
  WCA_FREEZE_REPRESENTATION = 20,
  WCA_EVER_UNCLOAKED = 21,
  WCA_VISUAL_OWNER = 22,
  WCA_HOLOGRAPHIC = 23,
  WCA_EXCLUDED_FROM_DDA = 24,
  WCA_PASSIVEUPDATEMODE = 25,
  WCA_USEDARKMODECOLORS = 26,
  WCA_LAST = 27
} WINDOWCOMPOSITIONATTRIB;

typedef struct _WINDOWCOMPOSITIONATTRIBDATA {
  WINDOWCOMPOSITIONATTRIB Attrib;
  PVOID pvData;
  SIZE_T cbData;
} WINDOWCOMPOSITIONATTRIBDATA;

typedef enum _ACCENT_STATE {
  ACCENT_DISABLED = 0,
  ACCENT_ENABLE_GRADIENT = 1,
  ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
  ACCENT_ENABLE_BLURBEHIND = 3,
  ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
  ACCENT_ENABLE_HOSTBACKDROP = 5,
  ACCENT_INVALID_STATE = 6
} ACCENT_STATE;

typedef struct _ACCENT_POLICY {
  ACCENT_STATE AccentState;
  DWORD AccentFlags;
  DWORD GradientColor;
  DWORD AnimationId;
} ACCENT_POLICY;

typedef BOOL(WINAPI* SetWindowCompositionAttribute)(
    HWND, WINDOWCOMPOSITIONATTRIBDATA*);

// ============================================================================
// FLUTTER-INTEGRATED WINDOW MESSAGE HANDLING
// ============================================================================
//
// For proper multi-window support, we need to integrate with Flutter's
// window management system rather than using global Windows hooks.
//
// The window_manager plugin uses RegisterTopLevelWindowProcDelegate()
// which is the proper Flutter way to intercept window messages.
// Our current global hook approach may conflict with Flutter's
// internal window management, especially in multi-window scenarios.
//
// For now, we'll implement a hybrid approach that:
// 1. Tracks Flutter windows specifically
// 2. Uses targeted message interception
// 3. Minimizes interference with Flutter's window procedures
// ============================================================================

// Global state to track Flutter windows with hidden title bars and their original window procedures
std::map<HWND, bool> g_flutter_hidden_title_bar_windows;
std::map<HWND, WNDPROC> g_original_window_procedures;

// Global state to track Flutter windows that are frameless
std::map<HWND, bool> g_flutter_frameless_windows;

// Global state to track Flutter windows with transparent backgrounds
std::map<HWND, bool> g_flutter_transparent_windows;

// Global function pointer for SetWindowCompositionAttribute
SetWindowCompositionAttribute g_set_window_composition_attribute = nullptr;

// Global CBT hook handle for intercepting window creation
HHOOK g_cbt_hook = nullptr;

// Message-only window for async processing
HWND g_message_window = nullptr;

// Map of windows pending auto-setup with their timer IDs
std::map<UINT_PTR, HWND> g_pending_autosetup_windows;
UINT_PTR g_next_timer_id = TIMER_AUTOSETUP_WINDOW;

// Forward declaration
bool AutoSetupFlutterWindow(HWND hwnd);

/**
 * Adjust NCCALCSIZE parameters for maximized frameless windows.
 * This mimics the window_manager plugin's adjustNCCALCSIZE function.
 */
void adjustNCCALCSIZE(HWND hwnd, NCCALCSIZE_PARAMS* sz) {
  LONG l = 8;
  LONG t = 8;

  // Get monitor information for proper border calculation
  HMONITOR monitor = MonitorFromRect(&sz->rgrc[0], MONITOR_DEFAULTTONEAREST);
  if (monitor != NULL) {
    MONITORINFO monitorInfo;
    monitorInfo.cbSize = sizeof(MONITORINFO);
    if (TRUE == GetMonitorInfo(monitor, &monitorInfo)) {
      l = sz->rgrc[0].left - monitorInfo.rcWork.left;
      t = sz->rgrc[0].top - monitorInfo.rcWork.top;
    }
  }

  sz->rgrc[0].left -= l;
  sz->rgrc[0].top -= t;
  sz->rgrc[0].right += l;
  sz->rgrc[0].bottom += t;
}

/**
 * Check if running on Windows 11 or greater.
 * Used to handle Windows version-specific behavior for title bar hiding.
 */
bool IsWindows11OrGreater() {
  DWORD dwVersion = 0;
  DWORD dwBuild = 0;

#pragma warning(push)
#pragma warning(disable : 4996)
  dwVersion = GetVersion();
  // Get the build number.
  if (dwVersion < 0x80000000)
    dwBuild = (DWORD)(HIWORD(dwVersion));
#pragma warning(pop)

  return dwBuild >= 22000;
}

/**
 * Check if a window handle belongs to a Flutter window.
 * This helps us avoid interfering with non-Flutter windows.
 */
bool IsFlutterWindow(HWND hwnd) {
  // Check window class name - Flutter windows typically have specific class names
  wchar_t className[256];
  if (GetClassNameW(hwnd, className, sizeof(className) / sizeof(wchar_t))) {
    std::wstring classStr(className);

    // Flutter windows often have class names containing "Flutter" or specific patterns
    if (classStr.find(L"FLUTTER") != std::wstring::npos ||
        classStr.find(L"flutter") != std::wstring::npos) {
      return true;
    }

    // Also check for common Flutter window class patterns
    if (classStr == L"FLUTTER_RUNNER_WIN32_WINDOW" ||
        classStr == L"FLUTTERVIEW" ||
        classStr.find(L"Flutter") != std::wstring::npos) {
      return true;
    }
  }

  // Additional check: Flutter windows are typically owned by the current process
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  return (pid == GetCurrentProcessId());
}

/**
 * Set up window subclassing for proper message interception.
 * This is called before manipulating a window's title bar to ensure
 * proper handling of WM_NCCALCSIZE messages during resizing.
 */

// Forward declaration - FlutterWindowSubclassProc is defined later in the file
LRESULT CALLBACK FlutterWindowSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

bool setupWindowInterception(HWND hwnd) {
  if (!hwnd || !::IsWindow(hwnd)) {
    return false;
  }

  // Only subclass if not already subclassed
  auto it = g_original_window_procedures.find(hwnd);
  if (it == g_original_window_procedures.end()) {
    // Store original procedure
    WNDPROC originalProc = reinterpret_cast<WNDPROC>(GetWindowLongPtr(hwnd, GWLP_WNDPROC));
    g_original_window_procedures[hwnd] = originalProc;

    // Set up subclassing for proper message interception
    // Forward declaration needed - FlutterWindowSubclassProc is defined later
    if (SetWindowSubclass(hwnd, FlutterWindowSubclassProc, 1, 0)) {
      std::cout << "Window subclassing set up for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
      return true;
    } else {
      std::cerr << "Failed to set up window subclassing for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
      return false;
    }
  }

  // Already subclassed
  return true;
}


/**
 * Window procedure for subclassed Flutter windows with hidden title bars.
 * This intercepts WM_NCCALCSIZE messages to properly handle frame calculations.
 */
LRESULT CALLBACK FlutterWindowSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
  // Check if this window needs special frame handling (frameless or title bar hidden)
  auto titleBarIt = g_flutter_hidden_title_bar_windows.find(hwnd);
  auto framelessIt = g_flutter_frameless_windows.find(hwnd);

  bool needsSpecialHandling = (titleBarIt != g_flutter_hidden_title_bar_windows.end() && titleBarIt->second) ||
                              (framelessIt != g_flutter_frameless_windows.end() && framelessIt->second);

  if (needsSpecialHandling) {
    // This window needs special frame handling

    if (message == WM_NCCALCSIZE && wParam) {
      // Handle non-client area size calculation for frameless or title bar hidden windows
      NCCALCSIZE_PARAMS* sz = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);

      // Check if this window is frameless
      bool isFrameless = (framelessIt != g_flutter_frameless_windows.end() && framelessIt->second);

      if (::IsZoomed(hwnd)) {
        // For maximized windows - use window_manager's adjustNCCALCSIZE for frameless
        if (isFrameless) {
          adjustNCCALCSIZE(hwnd, sz);
          std::cout << "[NCCALCSIZE] Maximized frameless window adjusted with monitor info" << std::endl;
        } else {
          // For maximized windows with hidden title bar - standard adjustment
          sz->rgrc[0].left += 8;
          sz->rgrc[0].top += 8;
          sz->rgrc[0].right -= 8;
          sz->rgrc[0].bottom -= 8;
          std::cout << "[NCCALCSIZE] Maximized title bar hidden window adjusted" << std::endl;
        }
      } else {
        // For normal windows - handle frameless vs title bar hidden differently
        if (isFrameless) {
          // For frameless windows: return 0 to indicate no non-client area
          // This removes all borders and rounded corners completely
          std::cout << "[NCCALCSIZE] Frameless window - no borders" << std::endl;
          return 0;
        } else {
          // For title bar hidden windows - use window_manager's exact approach
          sz->rgrc[0].top += IsWindows11OrGreater() ? 0 : 1;
          sz->rgrc[0].right -= 8;   // Remove right border
          sz->rgrc[0].bottom -= 8;  // Remove bottom border
          sz->rgrc[0].left -= -8;   // Remove left border

          std::cout << "[NCCALCSIZE] Title bar hidden window: left=" << sz->rgrc[0].left
                    << " top=" << sz->rgrc[0].top << " right=" << sz->rgrc[0].right
                    << " bottom=" << sz->rgrc[0].bottom << std::endl;
        }
      }

      return 0;  // Don't call original window procedure for this message
    }

    if (message == WM_NCACTIVATE) {
      // Prevent default frame painting during activation for custom frames
      // This prevents the flicker when focusing/unfocusing frameless or hidden title bar windows
      std::cout << "[NCACTIVATE] Intercepted activation message for custom frame window" << std::endl;
      return 1;  // Tell Windows we handled the activation painting
    }
  }

  // For all other messages, call the original window procedure
  auto origProcIt = g_original_window_procedures.find(hwnd);
  if (origProcIt != g_original_window_procedures.end()) {
    return CallWindowProc(origProcIt->second, hwnd, message, wParam, lParam);
  }

  // Fallback to default window procedure
  return DefWindowProc(hwnd, message, wParam, lParam);
}

// ============================================================================
// UTILITY FUNCTIONS FOR WINDOW HANDLE RETRIEVAL
// ============================================================================

/**
 * Gets all Flutter window handles from the engine.
 *
 * This function retrieves the main Flutter window handle by getting the plugin
 * registrar and accessing the implicit view. In a multi-window Flutter app,
 * this would return all Flutter windows if multiple views exist.
 *
 * @param engine The Flutter engine instance
 * @return Vector of window handles (HWND)
 */
std::vector<HWND> GetFlutterWindowHandles(flutter::FlutterEngine* engine) {
  std::vector<HWND> handles;

  // Get the plugin registrar for a dummy plugin name
  auto registrar = engine->GetRegistrarForPlugin("dummy_plugin");
  if (!registrar) {
    return handles; // Return empty vector if no registrar
  }

  // The registrar is returned as a PluginRegistrar*, but we need to access
  // Windows-specific functionality. In a real plugin, you'd use the
  // PluginRegistrarWindows class directly. For demonstration, we'll use
  // the C API to get the view handle directly.
  FlutterDesktopPluginRegistrarRef desktop_registrar =
      reinterpret_cast<FlutterDesktopPluginRegistrarRef>(registrar);

  FlutterDesktopViewRef view = FlutterDesktopPluginRegistrarGetView(desktop_registrar);
  if (view) {
    handles.push_back(FlutterDesktopViewGetHWND(view));
  }

  return handles;
}

/**
 * Enumerates all windows in the system.
 *
 * Uses the Windows EnumWindows API to get handles for all top-level windows.
 * This includes windows from all processes, not just the current Flutter app.
 *
 * @return Vector of all window handles in the system
 */
std::vector<HWND> GetAllWindowHandles() {
  std::vector<HWND> handles;

  ::EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
    std::vector<HWND>* pHandles = reinterpret_cast<std::vector<HWND>*>(lParam);
    pHandles->push_back(hwnd);
    return TRUE; // Continue enumeration
  }, reinterpret_cast<LPARAM>(&handles));

  return handles;
}


/**
 * Automatically set up a Flutter window with frameless and transparency.
 * This function applies all necessary window modifications for a Flutter window.
 */
bool AutoSetupFlutterWindow(HWND hwnd) {
  std::cout << "[AUTOSETUP] Starting auto-setup for window: 0x" << std::hex << hwnd << std::dec << std::endl;
  
  // Setup window interception
  if (!setupWindowInterception(hwnd)) {
    std::cerr << "[AUTOSETUP] Failed to setup window interception" << std::endl;
    return false;
  }
  std::cout << "[AUTOSETUP] Window interception setup complete" << std::endl;
  
  // Make frameless
  LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
  std::cout << "[AUTOSETUP] Original style: 0x" << std::hex << style << std::dec << std::endl;
  
  style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
            WS_BORDER | WS_DLGFRAME | WS_SIZEBOX);
  style |= WS_THICKFRAME;
  ::SetWindowLongPtrW(hwnd, GWL_STYLE, style);
  std::cout << "[AUTOSETUP] Applied frameless style: 0x" << std::hex << style << std::dec << std::endl;
  
  LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  exStyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
              WS_EX_STATICEDGE | WS_EX_TOOLWINDOW | WS_EX_APPWINDOW);
  ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
  std::cout << "[AUTOSETUP] Applied extended style" << std::endl;
  
  DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_DONOTROUND;
  ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
  std::cout << "[AUTOSETUP] Set corner preference" << std::endl;

  // Explicitly disable shadows
  DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
  ::DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
  std::cout << "[AUTOSETUP] Disabled NC rendering (removes shadow)" << std::endl;

  MARGINS margins = {0, 0, 0, 0};
  ::DwmExtendFrameIntoClientArea(hwnd, &margins);
  std::cout << "[AUTOSETUP] Extended DWM frame for frameless" << std::endl;
  
  g_flutter_frameless_windows[hwnd] = true;
  
  // Make transparent (if function available)
  if (g_set_window_composition_attribute) {
    std::cout << "[AUTOSETUP] Applying transparency..." << std::endl;
    
    // Don't change margins - keep {0,0,0,0} for shadow removal
    // Transparency works fine without extending DWM frame
    
    ACCENT_POLICY accent = {
      ACCENT_ENABLE_TRANSPARENTGRADIENT,
      2,
      0x00000000,
      0
    };
    
    WINDOWCOMPOSITIONATTRIBDATA data;
    data.Attrib = WCA_ACCENT_POLICY;
    data.pvData = &accent;
    data.cbData = sizeof(accent);
    
    if (g_set_window_composition_attribute(hwnd, &data)) {
      g_flutter_transparent_windows[hwnd] = true;
      std::cout << "[AUTOSETUP] Transparency applied successfully" << std::endl;
    } else {
      std::cerr << "[AUTOSETUP] Failed to apply transparency" << std::endl;
    }
  } else {
    std::cerr << "[AUTOSETUP] SetWindowCompositionAttribute not available" << std::endl;
  }
  
  // Force redraw
  RECT rect;
  ::GetWindowRect(hwnd, &rect);
  ::SetWindowPos(hwnd, nullptr, rect.left, rect.top,
               rect.right - rect.left, rect.bottom - rect.top,
               SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  std::cout << "[AUTOSETUP] Force redraw complete" << std::endl;
  
  std::cout << "[AUTOSETUP] Auto-setup complete for window: 0x" << std::hex << hwnd << std::dec << std::endl;
  return true;
}

/**
 * Message window procedure for async window processing.
 * Handles WM_FLUTTER_WINDOW_CREATED messages posted by the CBT hook.
 */
LRESULT CALLBACK MessageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_FLUTTER_WINDOW_CREATED) {
    HWND created_hwnd = reinterpret_cast<HWND>(wParam);
    
    if (::IsWindow(created_hwnd)) {
      wchar_t className[256] = {0};
      if (::GetClassNameW(created_hwnd, className, sizeof(className) / sizeof(wchar_t))) {
        std::wstring classStr(className);
        
        // Only process FLUTTER_HOST_WINDOW (the actual window, not FLUTTERVIEW)
        if (classStr == L"FLUTTER_HOST_WINDOW") {
          std::cout << "[CBT] FLUTTER_HOST_WINDOW detected: 0x" << std::hex << created_hwnd << std::dec << std::endl;
          
          // Set timer for delayed auto-setup (200ms delay)
          UINT_PTR timer_id = g_next_timer_id++;
          g_pending_autosetup_windows[timer_id] = created_hwnd;
          
          // Use message window handle instead of nullptr to preserve timer ID
          if (SetTimer(g_message_window, timer_id, 100, nullptr)) {
            std::cout << "[CBT] Scheduled delayed auto-setup (200ms) for window: 0x" << std::hex << created_hwnd 
                      << std::dec << " with timer ID: " << timer_id << std::endl;
          } else {
            std::cerr << "[CBT] Failed to schedule delayed auto-setup" << std::endl;
            g_pending_autosetup_windows.erase(timer_id);
          }
        }
      }
    }
    return 0;
  }
  
  if (msg == WM_TIMER) {
    UINT_PTR timer_id = wParam;
    std::cout << "[TIMER] Timer message received for ID: " << timer_id << std::endl;
    
    auto it = g_pending_autosetup_windows.find(timer_id);
    if (it != g_pending_autosetup_windows.end()) {
      HWND target_hwnd = it->second;
      std::cout << "[TIMER] Found window: 0x" << std::hex << target_hwnd << std::dec << std::endl;
      
      if (::IsWindow(target_hwnd)) {
        std::cout << "[TIMER] Window is valid, applying auto-setup..." << std::endl;
        
        if (AutoSetupFlutterWindow(target_hwnd)) {
          std::cout << "[TIMER] ✓ Auto-setup SUCCESS" << std::endl;
        } else {
          std::cerr << "[TIMER] ✗ Auto-setup FAILED" << std::endl;
        }
      } else {
        std::cout << "[TIMER] Window no longer exists" << std::endl;
      }
      
      g_pending_autosetup_windows.erase(it);
      KillTimer(hwnd, timer_id);
    } else {
      std::cout << "[TIMER] Timer ID not found in pending windows" << std::endl;
    }
    return 0;
  }
  
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

/**
 * CBT Hook callback for intercepting window creation.
 * This catches windows at the earliest possible stage (WM_NCCREATE).
 */
LRESULT CALLBACK CBTProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HCBT_CREATEWND) {
    // HWND hwnd = reinterpret_cast<HWND>(wParam);
    
    // Post message for async processing - returns immediately, no blocking
    if (g_message_window) {
      PostMessage(g_message_window, WM_FLUTTER_WINDOW_CREATED, wParam, 0);
    }
  }
  
  return CallNextHookEx(g_cbt_hook, nCode, wParam, lParam);
}

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t* command_line, _In_ int show_command) {
  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  if (!::AttachConsole(ATTACH_PARENT_PROCESS) && ::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }

  // Initialize COM, so that it is available for use in the library and/or
  // plugins.
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  flutter::DartProject project(L"data");

  auto command_line_arguments{GetCommandLineArguments()};

  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));

  auto const engine{std::make_shared<flutter::FlutterEngine>(project)};
  RegisterPlugins(engine.get());
  engine->Run();

  // Set to true to skip automatic window setup (message window, CBT hook, timers)
  bool skip_autosetup = true;
  
  if (skip_autosetup) {
    std::cout << "Auto-setup disabled — skipping window auto-setup and hooks" << std::endl;
  }

  // Load SetWindowCompositionAttribute function for transparency support
  HMODULE user32 = ::GetModuleHandleA("user32.dll");
  if (user32) {
    g_set_window_composition_attribute = reinterpret_cast<SetWindowCompositionAttribute>(
        ::GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (g_set_window_composition_attribute) {
      std::cout << "SetWindowCompositionAttribute loaded successfully" << std::endl;
    } else {
      std::cerr << "Failed to load SetWindowCompositionAttribute" << std::endl;
    }
  } else {
    std::cerr << "Failed to get user32.dll handle" << std::endl;
  }

  // Create message-only window for async processing and install CBT hook
  // unless NO_AUTOSETUP is set.
  if (!skip_autosetup) {
    WNDCLASS wc = {0};
    wc.lpfnWndProc = MessageWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"FlutterWindowDetectorMessageWindow";
    RegisterClass(&wc);

    g_message_window = CreateWindowEx(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, 
                                     HWND_MESSAGE, NULL, instance, NULL);
    if (g_message_window) {
      std::cout << "Message window created for async processing" << std::endl;
    } else {
      std::cerr << "Failed to create message window" << std::endl;
    }

    // Install CBT hook for automatic window creation interception
    g_cbt_hook = SetWindowsHookEx(WH_CBT, CBTProc, NULL, GetCurrentThreadId());
    if (g_cbt_hook) {
      std::cout << "CBT hook installed successfully for window creation tracking" << std::endl;
    } else {
      std::cerr << "Failed to install CBT hook: " << GetLastError() << std::endl;
    }
  }

  // ============================================================================
  // FLUTTER WINDOW SUBCLASSING SETUP
  // ============================================================================
  //
  // For proper multi-window support, we need to subclass individual Flutter windows
  // rather than using global hooks. This approach:
  // 1. Uses SetWindowSubclass to intercept messages for specific windows
  // 2. Avoids interfering with Flutter's internal window management
  // 3. Provides precise control over title bar manipulation
  // 4. Works correctly with Flutter's multi-window architecture
  //
  // The subclassing approach is more reliable than global hooks for Flutter
  // because it integrates at the window level rather than the message loop level.
  // ============================================================================

  // ============================================================================
  // FLUTTER METHOD CHANNEL SETUP
  // ============================================================================
  //
  // Set up a MethodChannel to handle requests from Dart for window operations.
  // This enables communication between the Flutter/Dart UI and native Windows APIs.
  //
  // Channel name must match the one used in Dart: 'com.example.window_service'
  //
  // IMPORTANT: Type Conversion Issue
  // -------------------------------
  // Dart integers are typically sent as int32_t to native code, but we need int64_t
  // for window handles (HWND). This requires explicit type checking and conversion
  // to prevent crashes. Always check std::holds_alternative<type>() before calling
  // std::get<type>().
  //
  // Window Title Bar Manipulation
  // ----------------------------
  // We use the Windows DWM (Desktop Window Manager) API to hide/show title bars:
  // 1. Modify window styles (GWL_STYLE) to add/remove WS_CAPTION
  // 2. Use DwmExtendFrameIntoClientArea with proper margins:
  //    - Hidden: MARGINS {0, 0, 1, 0} (top margin = 1) extends client area into title bar
  //    - Normal: MARGINS {0, 0, 0, 0} resets DWM to standard frame
  // 3. Force window redraw with SetWindowPos + SWP_FRAMECHANGED
  //
  // Key Insight: The margin top value of 1 tells DWM to treat the entire window
  // as client area, effectively "removing" the title bar from the non-client area.
  //
  // This approach is based on the window_manager plugin implementation but
  // simplified for our specific use case.
  // ============================================================================

  flutter::MethodChannel<flutter::EncodableValue> channel(
      engine->messenger(), "com.example.window_service",
      &flutter::StandardMethodCodec::GetInstance());

  channel.SetMethodCallHandler(
      [&](const flutter::MethodCall<flutter::EncodableValue>& call,
          std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
        const std::string& method = call.method_name();
        // ========================================================================
        // getFlutterWindowHandles: Get all Flutter window handles
        // ========================================================================
        // Returns a list of all Flutter window handles (HWND) as 64-bit integers.
        // This is the main entry point for getting window handles from Dart.
        // ========================================================================
        if (method == "getFlutterWindowHandles") {
          std::vector<flutter::EncodableValue> reply;
          auto flutter_hwnds = GetFlutterWindowHandles(engine.get());
          for (HWND hwnd : flutter_hwnds) {
            // Return as 64-bit integers to Dart
            reply.push_back(static_cast<int64_t>(reinterpret_cast<intptr_t>(hwnd)));
          }
          result->Success(flutter::EncodableValue(reply));
          return;
        }
        // ========================================================================
        // getAllWindowHandles: Get all system window handles
        // ========================================================================
        // Returns a list of ALL window handles in the system (not just Flutter windows).
        // Useful for debugging or finding other application windows.
        // ========================================================================
        if (method == "getAllWindowHandles") {
          std::vector<flutter::EncodableValue> reply;
          auto all_hwnds = GetAllWindowHandles();
          for (HWND hwnd : all_hwnds) {
            reply.push_back(static_cast<int64_t>(reinterpret_cast<intptr_t>(hwnd)));
          }
          result->Success(flutter::EncodableValue(reply));
          return;
        }
        // ========================================================================
        // getWindowInfo: Get detailed information about a window
        // ========================================================================
        // Returns window title and class name for the given HWND.
        // Useful for debugging and identifying windows.
        // ========================================================================
        if (method == "getWindowInfo") {
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          if (!args) {
            result->Error("bad_args", "Expected map with 'hwnd'");
            return;
          }
          auto it = args->find(flutter::EncodableValue("hwnd"));
          if (it == args->end()) {
            result->Error("bad_args", "Missing 'hwnd'");
            return;
          }
          int64_t hwnd_val = std::get<int64_t>(it->second);
          HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd_val));
          // Example info: window text and class name
          wchar_t title[256] = {0};
          wchar_t class_name[256] = {0};
          ::GetWindowTextW(hwnd, title, sizeof(title) / sizeof(wchar_t));
          ::GetClassNameW(hwnd, class_name, sizeof(class_name) / sizeof(wchar_t));
          flutter::EncodableMap info;
          // Convert wide strings to UTF-8 for Dart
          std::string title_utf8 = Utf8FromUtf16(title);
          std::string class_utf8 = Utf8FromUtf16(class_name);
          info[flutter::EncodableValue("title")] = flutter::EncodableValue(title_utf8);
          info[flutter::EncodableValue("className")] = flutter::EncodableValue(class_utf8);
          result->Success(flutter::EncodableValue(info));
          return;
        }
        // ========================================================================
        // getWindowHandleForViewId: Get window handle for specific Flutter view
        // ========================================================================
        // Maps a Flutter view ID to its corresponding Windows window handle (HWND).
        // Useful for multi-window Flutter applications where you need to manipulate
        // specific windows by their view IDs.
        // ========================================================================
        if (method == "getWindowHandleForViewId") {
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          if (!args) {
            result->Error("bad_args", "Expected map with 'viewId'");
            return;
          }
          auto it = args->find(flutter::EncodableValue("viewId"));
          if (it == args->end()) {
            result->Error("bad_args", "Missing 'viewId'");
            return;
          }
          int64_t view_id = std::get<int64_t>(it->second);
          FlutterDesktopPluginRegistrarRef desktop_registrar =
              reinterpret_cast<FlutterDesktopPluginRegistrarRef>(engine->GetRegistrarForPlugin("dummy_plugin"));
          FlutterDesktopViewRef view = FlutterDesktopPluginRegistrarGetViewById(desktop_registrar, view_id);
          if (view) {
            HWND hwnd = FlutterDesktopViewGetHWND(view);
            result->Success(flutter::EncodableValue(static_cast<int64_t>(reinterpret_cast<intptr_t>(hwnd))));
            return;
          }
          result->Success(flutter::EncodableValue());
          return;
        }

        // ========================================================================
        // setupWindowInterception: Set up message interception for a specific window
        // ========================================================================
        // Sets up window subclassing for proper title bar handling.
        // This is called from Flutter when we need to manipulate a window's title bar.
        // ========================================================================
        if (method == "setupWindowInterception") {
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          if (!args) {
            result->Error("bad_args", "Expected map with 'hwnd'");
            return;
          }
          auto it_hwnd = args->find(flutter::EncodableValue("hwnd"));
          if (it_hwnd == args->end()) {
            result->Error("bad_args", "Missing 'hwnd'");
            return;
          }

          try {
            // Handle type conversion from Dart
            int64_t hwnd_val = 0;
            auto& hwnd_value = it_hwnd->second;

            if (std::holds_alternative<int64_t>(hwnd_value)) {
              hwnd_val = std::get<int64_t>(hwnd_value);
            } else if (std::holds_alternative<int32_t>(hwnd_value)) {
              hwnd_val = static_cast<int64_t>(std::get<int32_t>(hwnd_value));
            } else if (std::holds_alternative<double>(hwnd_value)) {
              hwnd_val = static_cast<int64_t>(std::get<double>(hwnd_value));
            } else {
              result->Error("bad_type", "HWND value is not a supported numeric type");
              return;
            }

            HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd_val));

            if (!::IsWindow(hwnd)) {
              result->Error("invalid_hwnd", "Invalid window handle");
              return;
            }

            // Set up window subclassing for proper message interception
            WNDPROC originalProc = reinterpret_cast<WNDPROC>(GetWindowLongPtr(hwnd, GWLP_WNDPROC));

            // Only subclass if not already subclassed
            auto it = g_original_window_procedures.find(hwnd);
            if (it == g_original_window_procedures.end()) {
              // Store original procedure
              g_original_window_procedures[hwnd] = originalProc;

              // Set up subclassing
              if (SetWindowSubclass(hwnd, FlutterWindowSubclassProc, 1, 0)) {
                std::cout << "Window subclassing set up for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
                result->Success(flutter::EncodableValue(true));
              } else {
                std::cerr << "Failed to set up window subclassing for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
                result->Error("subclass_failed", "Failed to set up window subclassing");
              }
            } else {
              // Already subclassed
              result->Success(flutter::EncodableValue(true));
            }

            return;
          } catch (const std::exception& e) {
            std::cerr << "Exception in setupWindowInterception: " << e.what() << std::endl;
            result->Error("exception", std::string("Exception: ") + e.what());
            return;
          } catch (...) {
            std::cerr << "Unknown exception in setupWindowInterception" << std::endl;
            result->Error("exception", "Unknown exception occurred");
            return;
          }
        }

        // ========================================================================
        // toggleFrameless: Toggle frameless mode for a window
        // ========================================================================
        // Toggles between normal window and frameless window.
        // Frameless windows have no borders, title bar, or window controls.
        // ========================================================================
        if (method == "toggleFrameless") {
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          if (!args) {
            result->Error("bad_args", "Expected map with 'hwnd'");
            return;
          }
          auto it_hwnd = args->find(flutter::EncodableValue("hwnd"));
          if (it_hwnd == args->end()) {
            result->Error("bad_args", "Missing 'hwnd'");
            return;
          }

          try {
            // Handle type conversion from Dart
            int64_t hwnd_val = 0;
            auto& hwnd_value = it_hwnd->second;

            if (std::holds_alternative<int64_t>(hwnd_value)) {
              hwnd_val = std::get<int64_t>(hwnd_value);
            } else if (std::holds_alternative<int32_t>(hwnd_value)) {
              hwnd_val = static_cast<int64_t>(std::get<int32_t>(hwnd_value));
            } else if (std::holds_alternative<double>(hwnd_value)) {
              hwnd_val = static_cast<int64_t>(std::get<double>(hwnd_value));
            } else {
              result->Error("bad_type", "HWND value is not a supported numeric type");
              return;
            }

            HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd_val));

            if (!::IsWindow(hwnd)) {
              result->Error("invalid_hwnd", "Invalid window handle");
              return;
            }

            // Set up window interception first (required for proper frameless handling)
            if (!setupWindowInterception(hwnd)) {
              std::cerr << "Failed to set up window interception for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
              result->Error("interception_failed", "Failed to set up window interception");
              return;
            }

            // Check current window style to determine if it's frameless
            LONG_PTR current_style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
            bool is_frameless = (current_style & WS_CAPTION) == 0;
            std::cout << "[TOGGLE] Current frameless state detected: " << (is_frameless ? "YES" : "NO") << std::endl;

            // Toggle: if frameless -> make normal, if normal -> make frameless
            if (is_frameless) {
              // Make window normal (restore frame)
              LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
              style |= (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
              ::SetWindowLongPtrW(hwnd, GWL_STYLE, style);

              // Restore extended styles for normal windows
              LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
              exStyle |= (WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);  // Add standard window edges
              ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

              // Restore DWM attributes for normal windows
              // Enable window shadows for normal windows

              // Restore default corner preference for normal windows
              DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_DEFAULT;
              ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

              // Reset NC rendering policy to enable shadows
              DWMNCRENDERINGPOLICY policy = DWMNCRP_ENABLED;
              ::DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));

              // Reset DWM frame to normal
              MARGINS margins = {0, 0, 0, 0};
              ::DwmExtendFrameIntoClientArea(hwnd, &margins);

              // Unregister from frameless tracking
              g_flutter_frameless_windows.erase(hwnd);

              std::cout << "Window made normal for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
            } else {
              // Make window frameless (following window_manager approach exactly)
              LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
              // Remove ALL possible border and frame styles for completely clean appearance
              style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                        WS_BORDER | WS_DLGFRAME | WS_SIZEBOX);
              style |= WS_THICKFRAME;  // Keep resizing capability
              ::SetWindowLongPtrW(hwnd, GWL_STYLE, style);

              // For frameless windows, remove ALL extended styles that create visual effects
              LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
              exStyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
                          WS_EX_STATICEDGE | WS_EX_TOOLWINDOW | WS_EX_APPWINDOW);
              ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

              // Additional DWM attributes to ensure no visual artifacts
              // Disable window transitions and animations
              DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_DONOTROUND;
              ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

              // Disable DWM frame extension to remove shadow
              MARGINS margins = {0, 0, 0, 0};
              ::DwmExtendFrameIntoClientArea(hwnd, &margins);

              // Register for frameless tracking
              g_flutter_frameless_windows[hwnd] = true;

              std::cout << "Window made frameless for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
            }

            // Force window redraw
            RECT rect;
            ::GetWindowRect(hwnd, &rect);
            ::SetWindowPos(hwnd, nullptr, rect.left, rect.top,
                         rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

            result->Success(flutter::EncodableValue(true));
            return;
          } catch (const std::exception& e) {
            std::cerr << "Exception in toggleFrameless: " << e.what() << std::endl;
            result->Error("exception", std::string("Exception: ") + e.what());
            return;
          } catch (...) {
            std::cerr << "Unknown exception in toggleFrameless" << std::endl;
            result->Error("exception", "Unknown exception occurred");
            return;
          }
        }

        // ========================================================================
        // setFrameless: Explicitly set frameless mode
        // ========================================================================
        // Explicitly sets frameless mode for a window.
        // frameless: true for frameless, false for normal window.
        // ========================================================================
        if (method == "setFrameless") {
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          if (!args) {
            result->Error("bad_args", "Expected map with 'hwnd' and 'frameless'");
            return;
          }
          auto it_hwnd = args->find(flutter::EncodableValue("hwnd"));
          if (it_hwnd == args->end()) {
            result->Error("bad_args", "Missing 'hwnd'");
            return;
          }
          auto it_frameless = args->find(flutter::EncodableValue("frameless"));
          if (it_frameless == args->end()) {
            result->Error("bad_args", "Missing 'frameless'");
            return;
          }

          try {
            // Handle type conversion from Dart
            int64_t hwnd_val = 0;
            auto& hwnd_value = it_hwnd->second;

            if (std::holds_alternative<int64_t>(hwnd_value)) {
              hwnd_val = std::get<int64_t>(hwnd_value);
            } else if (std::holds_alternative<int32_t>(hwnd_value)) {
              hwnd_val = static_cast<int64_t>(std::get<int32_t>(hwnd_value));
            } else if (std::holds_alternative<double>(hwnd_value)) {
              hwnd_val = static_cast<int64_t>(std::get<double>(hwnd_value));
            } else {
              result->Error("bad_type", "HWND value is not a supported numeric type");
              return;
            }

            // Validate boolean parameter
            auto& frameless_value = it_frameless->second;
            if (!std::holds_alternative<bool>(frameless_value)) {
              result->Error("bad_type", "frameless value is not a boolean");
              return;
            }

            bool frameless = std::get<bool>(frameless_value);
            HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd_val));

            if (!::IsWindow(hwnd)) {
              result->Error("invalid_hwnd", "Invalid window handle");
              return;
            }

            // Check if transparency is active and preserve its state
            auto transparentIt = g_flutter_transparent_windows.find(hwnd);
            bool isTransparent = (transparentIt != g_flutter_transparent_windows.end() && transparentIt->second);
            bool wasTransparent = isTransparent; // Remember original state

            // Set up window interception first
            if (!setupWindowInterception(hwnd)) {
              std::cerr << "Failed to set up window interception for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
              result->Error("interception_failed", "Failed to set up window interception");
              return;
            }

            if (frameless) {
              // Make window frameless (following window_manager approach exactly)
              LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
              // Remove ALL possible border and frame styles for completely clean appearance
              style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                        WS_BORDER | WS_DLGFRAME | WS_SIZEBOX);
              style |= WS_THICKFRAME;  // Keep resizing capability
              ::SetWindowLongPtrW(hwnd, GWL_STYLE, style);

              // For frameless windows, remove ALL extended styles that create visual effects
              LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
              exStyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
                          WS_EX_STATICEDGE | WS_EX_TOOLWINDOW | WS_EX_APPWINDOW);
              ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

              // Additional DWM attributes to ensure no visual artifacts
              // Disable window transitions and animations
              DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_DONOTROUND;
              ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

              // Disable DWM frame extension to remove shadow  
              MARGINS margins = {0, 0, 0, 0};  // Zero margins remove shadow
              ::DwmExtendFrameIntoClientArea(hwnd, &margins);

              // Register for frameless tracking
              g_flutter_frameless_windows[hwnd] = true;

              std::cout << "Window set to frameless for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
            } else {
              // Make window normal
              LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
              style |= (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
              ::SetWindowLongPtrW(hwnd, GWL_STYLE, style);

              // Restore extended styles for normal windows
              LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
              exStyle |= (WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);  // Add standard window edges
              ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

              // Restore DWM attributes for normal windows
              // Enable window shadows for normal windows

              // Restore default corner preference for normal windows
              DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_DEFAULT;
              ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

              // Reset NC rendering policy to enable shadows
              DWMNCRENDERINGPOLICY policy = DWMNCRP_ENABLED;
              ::DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));

              // Reset DWM frame to normal
              MARGINS margins = {0, 0, 0, 0};
              ::DwmExtendFrameIntoClientArea(hwnd, &margins);

              // Unregister from frameless tracking
              g_flutter_frameless_windows.erase(hwnd);

              std::cout << "Window set to normal for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
            }

            // Force window redraw
            RECT rect;
            ::GetWindowRect(hwnd, &rect);
            ::SetWindowPos(hwnd, nullptr, rect.left, rect.top,
                         rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

            // If transparency was active before, reapply it after frameless change
            if (wasTransparent && g_set_window_composition_attribute) {
              ACCENT_POLICY accent = {
                  ACCENT_ENABLE_TRANSPARENTGRADIENT,  // AccentState
                  2,                                  // AccentFlags
                  0x00000000,                         // GradientColor (fully transparent in ABGR format)
                  0                                   // AnimationId
              };

              WINDOWCOMPOSITIONATTRIBDATA data;
              data.Attrib = WCA_ACCENT_POLICY;
              data.pvData = &accent;
              data.cbData = sizeof(accent);

              g_set_window_composition_attribute(hwnd, &data);
              std::cout << "Reapplied transparency after frameless change for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
            }

            result->Success(flutter::EncodableValue(true));
            return;
          } catch (const std::exception& e) {
            std::cerr << "Exception in setFrameless: " << e.what() << std::endl;
            result->Error("exception", std::string("Exception: ") + e.what());
            return;
          } catch (...) {
            std::cerr << "Unknown exception in setFrameless" << std::endl;
            result->Error("exception", "Unknown exception occurred");
            return;
          }
        }

        // ========================================================================
        // getFocusedFlutterWindowHandle: Get currently focused Flutter window
        // ========================================================================
        // Returns the window handle of the currently focused window if it belongs
        // to this Flutter process. Returns null if no Flutter window is focused
        // or if the focused window belongs to a different process.
        // ========================================================================
        if (method == "getFocusedFlutterWindowHandle") {
          HWND fg = ::GetForegroundWindow();
          if (!fg) {
            result->Success(flutter::EncodableValue());
            return;
          }
          DWORD pid = 0;
          ::GetWindowThreadProcessId(fg, &pid);
          if (pid != ::GetCurrentProcessId()) {
            // Foreground window isn't in this process; no focused Flutter window
            result->Success(flutter::EncodableValue());
            return;
          }
          result->Success(flutter::EncodableValue(static_cast<int64_t>(reinterpret_cast<intptr_t>(fg))));
          return;
        }

        // ========================================================================
        // toggleTitleBar: Toggle title bar visibility (SMART TOGGLE)
        // ========================================================================
        // Automatically detects current title bar state and toggles it.
        // - If title bar is visible → hides it
        // - If title bar is hidden → shows it
        // This is the most user-friendly method for UI toggles.
        // ========================================================================
        if (method == "toggleTitleBar") {
          // Toggle title bar visibility for a window.
          // This method automatically detects if the title bar is currently hidden
          // and toggles it to the opposite state.
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          if (!args) {
            result->Error("bad_args", "Expected map with 'hwnd'");
            return;
          }
          auto it_hwnd = args->find(flutter::EncodableValue("hwnd"));
          if (it_hwnd == args->end()) {
            result->Error("bad_args", "Missing 'hwnd'");
            return;
          }

          try {
            // Handle type conversion from Dart (which may send int32_t instead of int64_t)
            int64_t hwnd_val = 0;
            auto& value = it_hwnd->second;

            // Try different numeric types that Dart might send
            if (std::holds_alternative<int64_t>(value)) {
              hwnd_val = std::get<int64_t>(value);
            } else if (std::holds_alternative<int32_t>(value)) {
              hwnd_val = static_cast<int64_t>(std::get<int32_t>(value));
            } else if (std::holds_alternative<double>(value)) {
              hwnd_val = static_cast<int64_t>(std::get<double>(value));
            } else {
              result->Error("bad_type", "HWND value is not a supported numeric type");
              return;
            }

            HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd_val));

            if (!::IsWindow(hwnd)) {
              result->Error("invalid_hwnd", "Invalid window handle");
              return;
            }

            // Set up window interception first (required for proper title bar handling)
            if (!setupWindowInterception(hwnd)) {
              std::cerr << "Failed to set up window interception for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
              result->Error("interception_failed", "Failed to set up window interception");
              return;
            }

            // Check current window style to determine title bar state
            LONG_PTR current_style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
            bool has_caption = (current_style & WS_CAPTION) != 0;

            // Check if transparency is active and preserve its state
            auto transparentIt = g_flutter_transparent_windows.find(hwnd);
            bool isTransparent = (transparentIt != g_flutter_transparent_windows.end() && transparentIt->second);
            bool wasTransparent = isTransparent; // Remember original state

            // Toggle: if has caption -> hide, if no caption -> show
            if (has_caption) {
              // Hide title bar using proper DWM frame extension
              LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
              style &= ~(WS_CAPTION);  // Remove caption but keep resizing
              ::SetWindowLongPtrW(hwnd, GWL_STYLE, style);

              // Only modify DWM margins if transparency is not active
              if (!isTransparent) {
                // Extend client area into title bar area (key insight from window_manager)
                MARGINS margins = {-1, -1, -1, -1};  // Extend all sides into client area
                ::DwmExtendFrameIntoClientArea(hwnd, &margins);
              }

              // Register this window for message interception
              g_flutter_hidden_title_bar_windows[hwnd] = true;
            } else {
              // Show title bar: restore WS_CAPTION and related styles
              LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
              style |= (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME);
              ::SetWindowLongPtrW(hwnd, GWL_STYLE, style);

              // Only modify DWM margins if transparency is not active
              if (!isTransparent) {
                // Reset DWM frame to normal (no extension)
                MARGINS margins = {0, 0, 0, 0};
                ::DwmExtendFrameIntoClientArea(hwnd, &margins);
              }

              // Unregister this window from message interception
              g_flutter_hidden_title_bar_windows.erase(hwnd);
            }

            // Force window redraw to apply changes
            RECT rect;
            ::GetWindowRect(hwnd, &rect);
            ::SetWindowPos(hwnd, nullptr, rect.left, rect.top,
                         rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

            // If transparency was active before, reapply it after title bar change
            if (wasTransparent && g_set_window_composition_attribute) {
              ACCENT_POLICY accent = {
                  ACCENT_ENABLE_TRANSPARENTGRADIENT,  // AccentState
                  2,                                  // AccentFlags
                  0x00000000,                         // GradientColor (fully transparent in ABGR format)
                  0                                   // AnimationId
              };

              WINDOWCOMPOSITIONATTRIBDATA data;
              data.Attrib = WCA_ACCENT_POLICY;
              data.pvData = &accent;
              data.cbData = sizeof(accent);

              g_set_window_composition_attribute(hwnd, &data);
              std::cout << "Reapplied transparency after title bar toggle for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
            }

            result->Success(flutter::EncodableValue(true));
            return;
          } catch (const std::exception& e) {
            std::cerr << "Exception in toggleTitleBar: " << e.what() << std::endl;
            result->Error("exception", std::string("Exception: ") + e.what());
            return;
          } catch (...) {
            std::cerr << "Unknown exception in toggleTitleBar" << std::endl;
            result->Error("exception", "Unknown exception occurred");
            return;
          }
        }

        // ========================================================================
        // setTitleBarStyle: Explicitly set title bar visibility
        // ========================================================================
        // Explicitly sets the title bar to hidden or normal state.
        // Unlike toggleTitleBar, this method requires you to specify the desired state.
        // Useful when you need precise control over the title bar state.
        // ========================================================================
        if (method == "setTitleBarStyle") {
          // Set title bar visibility for a window.
          // This method explicitly sets the title bar to hidden or normal state.
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          if (!args) {
            result->Error("bad_args", "Expected map with 'hwnd' and 'titleBarStyle'");
            return;
          }
          auto it_hwnd = args->find(flutter::EncodableValue("hwnd"));
          if (it_hwnd == args->end()) {
            result->Error("bad_args", "Missing 'hwnd'");
            return;
          }
          auto it_style = args->find(flutter::EncodableValue("titleBarStyle"));
          if (it_style == args->end()) {
            result->Error("bad_args", "Missing 'titleBarStyle'");
            return;
          }

          try {
            // Handle type conversion from Dart (which may send int32_t instead of int64_t)
            int64_t hwnd_val = 0;
            auto& hwnd_value = it_hwnd->second;

            // Try different numeric types that Dart might send
            if (std::holds_alternative<int64_t>(hwnd_value)) {
              hwnd_val = std::get<int64_t>(hwnd_value);
            } else if (std::holds_alternative<int32_t>(hwnd_value)) {
              hwnd_val = static_cast<int64_t>(std::get<int32_t>(hwnd_value));
            } else if (std::holds_alternative<double>(hwnd_value)) {
              hwnd_val = static_cast<int64_t>(std::get<double>(hwnd_value));
            } else {
              result->Error("bad_type", "HWND value is not a supported numeric type");
              return;
            }

            // Validate string parameter
            auto& style_value = it_style->second;
            if (!std::holds_alternative<std::string>(style_value)) {
              result->Error("bad_type", "titleBarStyle value is not a string");
              return;
            }

            std::string title_bar_style = std::get<std::string>(style_value);
            HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd_val));

            if (!::IsWindow(hwnd)) {
              result->Error("invalid_hwnd", "Invalid window handle");
              return;
            }

            // Set up window interception first (required for proper title bar handling)
            // This ensures the window is properly subclassed before we modify its title bar
            if (!setupWindowInterception(hwnd)) {
              std::cerr << "Failed to set up window interception for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
              result->Error("interception_failed", "Failed to set up window interception");
              return;
            }

            // Check if transparency is active and preserve its state
            auto transparentIt = g_flutter_transparent_windows.find(hwnd);
            bool isTransparent = (transparentIt != g_flutter_transparent_windows.end() && transparentIt->second);
            bool wasTransparent = isTransparent; // Remember original state

            // Apply the requested title bar style
            if (title_bar_style == "hidden") {
              // Hide title bar using window_manager approach
              LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
              style &= ~(WS_CAPTION);  // Remove caption but keep resizing
              ::SetWindowLongPtrW(hwnd, GWL_STYLE, style);

              // Only modify DWM margins if transparency is not active
              if (!isTransparent) {
                // Extend client area into title bar area (key insight from window_manager)
                // For hidden title bar, extend frame into client area
                MARGINS margins = {-1, -1, -1, -1};  // Extend all sides into client area
                ::DwmExtendFrameIntoClientArea(hwnd, &margins);
              }

              // Register this Flutter window for message interception
              g_flutter_hidden_title_bar_windows[hwnd] = true;

              std::cout << "Title bar hidden - DWM extended client area (top margin = 1)" << std::endl;
            } else if (title_bar_style == "normal" || title_bar_style == "visible") {
              // Show title bar: restore WS_CAPTION and related styles
              LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
              style |= (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME);
              ::SetWindowLongPtrW(hwnd, GWL_STYLE, style);

              // Only modify DWM margins if transparency is not active
              if (!isTransparent) {
                // Reset DWM frame to normal (no extension)
                MARGINS margins = {0, 0, 0, 0};
                ::DwmExtendFrameIntoClientArea(hwnd, &margins);
              }

              // Unregister this window from message interception
              g_flutter_hidden_title_bar_windows.erase(hwnd);

              std::cout << "Title bar shown - DWM frame reset to normal" << std::endl;
            } else {
              result->Error("invalid_style", "titleBarStyle must be 'hidden' or 'normal'");
              return;
            }

            // Force complete window redraw (essential for DWM changes to take effect)
            RECT rect;
            ::GetWindowRect(hwnd, &rect);
            ::SetWindowPos(hwnd, nullptr, rect.left, rect.top,
                         rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

            // Additional redraw for maximized windows (window_manager handles this in WM_NCCALCSIZE)
            if (::IsZoomed(hwnd)) {
              ::ShowWindow(hwnd, SW_HIDE);
              ::ShowWindow(hwnd, SW_SHOWMAXIMIZED);
            }

            // If transparency was active before, reapply it after title bar change
            if (wasTransparent && g_set_window_composition_attribute) {
              ACCENT_POLICY accent = {
                  ACCENT_ENABLE_TRANSPARENTGRADIENT,  // AccentState
                  2,                                  // AccentFlags
                  0x00000000,                         // GradientColor (fully transparent in ABGR format)
                  0                                   // AnimationId
              };

              WINDOWCOMPOSITIONATTRIBDATA data;
              data.Attrib = WCA_ACCENT_POLICY;
              data.pvData = &accent;
              data.cbData = sizeof(accent);

              g_set_window_composition_attribute(hwnd, &data);
              std::cout << "Reapplied transparency after title bar change for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
            }

            result->Success(flutter::EncodableValue(true));
            return;
          } catch (const std::exception& e) {
            std::cerr << "Exception in setTitleBarStyle: " << e.what() << std::endl;
            result->Error("exception", std::string("Exception: ") + e.what());
            return;
          } catch (...) {
            std::cerr << "Unknown exception in setTitleBarStyle" << std::endl;
            result->Error("exception", "Unknown exception occurred");
            return;
          }
        }

        // ========================================================================
        // setTransparentBackground: Set window background transparency
        // ========================================================================
        // Sets the window background to be fully transparent or normal.
        // Uses Windows composition attributes to achieve transparency effect.
        // ========================================================================
        if (method == "setTransparentBackground") {
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          if (!args) {
            result->Error("bad_args", "Expected map with 'hwnd' and 'transparent'");
            return;
          }
          auto it_hwnd = args->find(flutter::EncodableValue("hwnd"));
          if (it_hwnd == args->end()) {
            result->Error("bad_args", "Missing 'hwnd'");
            return;
          }
          auto it_transparent = args->find(flutter::EncodableValue("transparent"));
          if (it_transparent == args->end()) {
            result->Error("bad_args", "Missing 'transparent'");
            return;
          }

          try {
            // Handle type conversion from Dart
            int64_t hwnd_val = 0;
            auto& hwnd_value = it_hwnd->second;

            if (std::holds_alternative<int64_t>(hwnd_value)) {
              hwnd_val = std::get<int64_t>(hwnd_value);
            } else if (std::holds_alternative<int32_t>(hwnd_value)) {
              hwnd_val = static_cast<int64_t>(std::get<int32_t>(hwnd_value));
            } else if (std::holds_alternative<double>(hwnd_value)) {
              hwnd_val = static_cast<int64_t>(std::get<double>(hwnd_value));
            } else {
              result->Error("bad_type", "HWND value is not a supported numeric type");
              return;
            }

            // Validate boolean parameter
            auto& transparent_value = it_transparent->second;
            if (!std::holds_alternative<bool>(transparent_value)) {
              result->Error("bad_type", "transparent value is not a boolean");
              return;
            }

            bool transparent = std::get<bool>(transparent_value);
            HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd_val));

            if (!::IsWindow(hwnd)) {
              result->Error("invalid_hwnd", "Invalid window handle");
              return;
            }

            // Check if SetWindowCompositionAttribute is available
            if (!g_set_window_composition_attribute) {
              result->Error("function_not_loaded", "SetWindowCompositionAttribute not available");
              return;
            }

            // Check if transparency is already in the desired state
            auto transparentIt = g_flutter_transparent_windows.find(hwnd);
            bool currentlyTransparent = (transparentIt != g_flutter_transparent_windows.end() && transparentIt->second);
            std::cout << "[TOGGLE] Current transparent state: " << (currentlyTransparent ? "YES" : "NO") 
                      << ", Requested: " << (transparent ? "YES" : "NO") << std::endl;

            if (transparent == currentlyTransparent) {
              // Already in desired state, no need to do anything
              std::cout << "Window transparency already in desired state for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
              result->Success(flutter::EncodableValue(true));
              return;
            }

            if (transparent) {
              // Make window background transparent
              // Only disable existing accent policy if transitioning from non-transparent to transparent
              if (!currentlyTransparent) {
                ACCENT_POLICY disable_accent = {
                    ACCENT_DISABLED,  // AccentState
                    2,                // AccentFlags
                    0x00000000,       // GradientColor
                    0                 // AnimationId
                };

                WINDOWCOMPOSITIONATTRIBDATA disable_data;
                disable_data.Attrib = WCA_ACCENT_POLICY;
                disable_data.pvData = &disable_accent;
                disable_data.cbData = sizeof(disable_accent);
                g_set_window_composition_attribute(hwnd, &disable_data);
              }

              // For transparency, we need to extend DWM frame, but only if not already transparent
              // The frameless operation sets {0, 0, 1, 0} which works better with transparency
              // So we'll use that instead of {-1, -1, -1, -1}
              if (!currentlyTransparent) {
                MARGINS margins = {0, 0, 1, 0};  // Use frameless-style margins for transparency
                ::DwmExtendFrameIntoClientArea(hwnd, &margins);
              }

              // Apply transparent gradient accent
              ACCENT_POLICY accent = {
                  ACCENT_ENABLE_TRANSPARENTGRADIENT,  // AccentState
                  2,                                  // AccentFlags
                  0x00000000,                         // GradientColor (fully transparent in ABGR format)
                  0                                   // AnimationId
              };

              WINDOWCOMPOSITIONATTRIBDATA data;
              data.Attrib = WCA_ACCENT_POLICY;
              data.pvData = &accent;
              data.cbData = sizeof(accent);

              if (g_set_window_composition_attribute(hwnd, &data)) {
                // Track this window as having transparent background
                g_flutter_transparent_windows[hwnd] = true;
                std::cout << "Window background set to transparent for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
              } else {
                std::cerr << "Failed to set transparent background for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
                result->Error("transparency_failed", "Failed to set transparent background");
                return;
              }
            } else {
              // Restore normal window background
              // Only disable accent policy if transitioning from transparent to non-transparent
              if (currentlyTransparent) {
                ACCENT_POLICY accent = {
                    ACCENT_DISABLED,  // AccentState
                    2,                // AccentFlags
                    0x00000000,       // GradientColor
                    0                 // AnimationId
                };

                WINDOWCOMPOSITIONATTRIBDATA data;
                data.Attrib = WCA_ACCENT_POLICY;
                data.pvData = &accent;
                data.cbData = sizeof(accent);

                if (g_set_window_composition_attribute(hwnd, &data)) {
                  // Check if title bar is hidden to determine correct margins
                  auto titleBarIt = g_flutter_hidden_title_bar_windows.find(hwnd);
                  bool titleBarHidden = (titleBarIt != g_flutter_hidden_title_bar_windows.end() && titleBarIt->second);

                  // Reset DWM margins appropriately
                  MARGINS margins;
                  if (titleBarHidden) {
                    // If title bar is hidden, use the margins for hidden title bar
                    margins = {-1, -1, -1, -1};
                  } else {
                    // Normal window margins
                    margins = {0, 0, 0, 0};
                  }
                  ::DwmExtendFrameIntoClientArea(hwnd, &margins);

                  // Untrack this window from transparent tracking
                  g_flutter_transparent_windows.erase(hwnd);
                  std::cout << "Window background restored to normal for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
                } else {
                  std::cerr << "Failed to restore normal background for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
                  result->Error("restore_failed", "Failed to restore normal background");
                  return;
                }
              } else {
                // Already in desired state (not transparent)
                std::cout << "Window background already normal for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
                result->Success(flutter::EncodableValue(true));
                return;
              }
            }

            // Force window redraw to apply changes
            RECT rect;
            ::GetWindowRect(hwnd, &rect);
            ::SetWindowPos(hwnd, nullptr, rect.left, rect.top,
                         rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

            result->Success(flutter::EncodableValue(true));
            return;
          } catch (const std::exception& e) {
            std::cerr << "Exception in setTransparentBackground: " << e.what() << std::endl;
            result->Error("exception", std::string("Exception: ") + e.what());
            return;
          } catch (...) {
            std::cerr << "Unknown exception in setTransparentBackground" << std::endl;
            result->Error("exception", "Unknown exception occurred");
            return;
          }
        }

        // ========================================================================
        // isWindowCreationHookActive: Check if CBT hook is active
        // ========================================================================
        // Returns true if the window creation interception hook is active.
        // Useful for debugging and verifying the hook is working.
        // ========================================================================
        if (method == "isWindowCreationHookActive") {
          result->Success(flutter::EncodableValue(g_cbt_hook != nullptr));
          return;
        }

        result->NotImplemented();
      });

  // Get Flutter window handles
  auto flutter_handles = GetFlutterWindowHandles(engine.get());
  std::cout << "Found " << flutter_handles.size() << " Flutter window(s):" << std::endl;
  for (size_t i = 0; i < flutter_handles.size(); ++i) {
    std::cout << "Flutter Window " << i + 1 << " Handle: 0x" << std::hex << flutter_handles[i] << std::endl;
  }

  // Get all window handles in the system
  auto all_handles = GetAllWindowHandles();
  std::cout << "Total windows in system: " << all_handles.size() << std::endl;

  ::MSG msg;
  while (::GetMessage(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
  }

  // Clean up window subclassing for all tracked windows
  for (auto& pair : g_original_window_procedures) {
    HWND hwnd = pair.first;
    if (::IsWindow(hwnd)) {
      // Remove our subclass and restore original window procedure
      RemoveWindowSubclass(hwnd, FlutterWindowSubclassProc, 1);
      std::cout << "Cleaned up subclassing for hwnd: 0x" << std::hex << hwnd << std::dec << std::endl;
    }
  }

  // Clear the tracking maps
  g_flutter_hidden_title_bar_windows.clear();
  g_original_window_procedures.clear();

  // Kill any pending auto-setup timers (only if a message window was created).
  if (g_message_window) {
    for (const auto& pair : g_pending_autosetup_windows) {
      KillTimer(g_message_window, pair.first);
    }
  }
  g_pending_autosetup_windows.clear();

  // Destroy message window (only if we created one)
  if (g_message_window) {
    DestroyWindow(g_message_window);
    g_message_window = nullptr;
  }

  // Unhook CBT hook (only if we installed one)
  if (g_cbt_hook) {
    UnhookWindowsHookEx(g_cbt_hook);
    std::cout << "CBT hook uninstalled" << std::endl;
  }

  ::CoUninitialize();
  return EXIT_SUCCESS;
}
