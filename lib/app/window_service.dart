// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'package:flutter/services.dart';

class WindowService {
  static const MethodChannel _channel = MethodChannel('com.example.window_service');

  /// Gets all Flutter window handles (HWND on Windows)
  static Future<List<int>> getFlutterWindowHandles() async {
    try {
      final List<dynamic>? handles = await _channel.invokeMethod('getFlutterWindowHandles');
      return handles?.map((e) => e as int).toList() ?? [];
    } on PlatformException catch (e) {
      print('Failed to get Flutter window handles: ${e.message}');
      return [];
    }
  }

  /// Gets all window handles in the system
  static Future<List<int>> getAllWindowHandles() async {
    try {
      final List<dynamic>? handles = await _channel.invokeMethod('getAllWindowHandles');
      return handles?.map((e) => e as int).toList() ?? [];
    } on PlatformException catch (e) {
      print('Failed to get all window handles: ${e.message}');
      return [];
    }
  }

  /// Gets information about a specific window handle
  static Future<Map<String, dynamic>?> getWindowInfo(int hwnd) async {
    try {
      final Map<dynamic, dynamic>? info = await _channel.invokeMethod('getWindowInfo', {'hwnd': hwnd});
      return info?.map((key, value) => MapEntry(key.toString(), value));
    } on PlatformException catch (e) {
      print('Failed to get window info: ${e.message}');
      return null;
    }
  }

  /// Gets the native window handle (HWND) for a Flutter view id.
  static Future<int?> getWindowHandleForViewId(int viewId) async {
    try {
      final int? handle = await _channel.invokeMethod('getWindowHandleForViewId', {'viewId': viewId});
      return handle;
    } on PlatformException catch (e) {
      print('Failed to get window handle for viewId $viewId: ${e.message}');
      return null;
    }
  }

  /// Gets the HWND of the currently focused window if it belongs to this process.
  static Future<int?> getFocusedFlutterWindowHandle() async {
    try {
      final int? handle = await _channel.invokeMethod('getFocusedFlutterWindowHandle');
      return handle;
    } on PlatformException catch (e) {
      print('Failed to get focused Flutter window handle: ${e.message}');
      return null;
    }
  }

  /// Hides or shows the title bar for a window (HWND).
  /// Set titleBarStyle to 'hidden' to hide, 'normal' to show.
  static Future<bool> setTitleBarStyle(int hwnd, {required String titleBarStyle}) async {
    try {
      final bool? success = await _channel.invokeMethod('setTitleBarStyle', {
        'hwnd': hwnd,
        'titleBarStyle': titleBarStyle,
      });
      return success ?? false;
    } on PlatformException catch (e) {
      print('Failed to set title bar style: ${e.message}');
      return false;
    }
  }

  /// Toggles the title bar for a window (HWND).
  /// If currently hidden, shows it. If currently visible, hides it.
  static Future<bool> toggleTitleBar(int hwnd) async {
    try {
      final bool? success = await _channel.invokeMethod('toggleTitleBar', {
        'hwnd': hwnd,
      });
      return success ?? false;
    } on PlatformException catch (e) {
      print('Failed to toggle title bar: ${e.message}');
      return false;
    }
  }

  /// Sets up message interception for a specific window.
  /// This is needed for proper title bar hiding in Flutter multi-window.
  static Future<bool> setupWindowInterception(int hwnd) async {
    try {
      final bool? success = await _channel.invokeMethod('setupWindowInterception', {
        'hwnd': hwnd,
      });
      return success ?? false;
    } on PlatformException catch (e) {
      print('Failed to setup window interception: ${e.message}');
      return false;
    }
  }

  /// Toggles frameless mode for a window.
  /// Frameless windows have no borders, title bar, or window controls.
  static Future<bool> toggleFrameless(int hwnd) async {
    try {
      final bool? success = await _channel.invokeMethod('toggleFrameless', {
        'hwnd': hwnd,
      });
      return success ?? false;
    } on PlatformException catch (e) {
      print('Failed to toggle frameless: ${e.message}');
      return false;
    }
  }

  /// Sets frameless mode for a window.
  /// frameless: true for frameless, false for normal window.
  static Future<bool> setFrameless(int hwnd, {required bool frameless}) async {
    try {
      final bool? success = await _channel.invokeMethod('setFrameless', {
        'hwnd': hwnd,
        'frameless': frameless,
      });
      return success ?? false;
    } on PlatformException catch (e) {
      print('Failed to set frameless: ${e.message}');
      return false;
    }
  }

  /// Sets the window background to be transparent or normal.
  /// transparent: true for transparent background, false for normal background.
  static Future<bool> setTransparentBackground(int hwnd, {required bool transparent}) async {
    try {
      final bool? success = await _channel.invokeMethod('setTransparentBackground', {
        'hwnd': hwnd,
        'transparent': transparent,
      });
      return success ?? false;
    } on PlatformException catch (e) {
      print('Failed to set transparent background: ${e.message}');
      return false;
    }
  }
}
