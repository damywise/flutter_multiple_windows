// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter/material.dart';
import 'window_service.dart';

/// A reusable widget containing common window manipulation controls
/// including title bar toggle, frameless toggle, and transparency controls.
class WindowControlsWidget extends StatelessWidget {
  const WindowControlsWidget({
    super.key,
    this.showLabels = true,
    this.spacing = 8.0,
    this.buttonStyle,
  });

  /// Whether to show descriptive labels above buttons
  final bool showLabels;

  /// Spacing between buttons
  final double spacing;

  /// Optional custom button style
  final ButtonStyle? buttonStyle;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      mainAxisSize: MainAxisSize.min,
      children: [
        if (showLabels) ...[
          const Text(
            'Window Controls',
            style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16.0),
          ),
          const SizedBox(height: 8),
        ],
        OutlinedButton(
          style: buttonStyle,
          onPressed: () async {
            // Get focused window and toggle title bar
            final focusedHwnd = await WindowService.getFocusedFlutterWindowHandle();

            if (focusedHwnd == null) {
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('No focused Flutter window')),
              );
              return;
            }

            // Set up window interception first (required for proper title bar handling)
            final interceptionSuccess = await WindowService.setupWindowInterception(focusedHwnd);
            if (!interceptionSuccess) {
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('Failed to set up window interception')),
              );
              return;
            }

            // Toggle title bar
            final success = await WindowService.toggleTitleBar(focusedHwnd);

            if (!context.mounted) return;
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(
                content: Text(success
                  ? 'Title bar toggled for 0x${focusedHwnd.toRadixString(16)}'
                  : 'Failed to toggle title bar'
                ),
              ),
            );
          },
          child: const Text('Toggle Title Bar'),
        ),
        SizedBox(height: spacing),
        OutlinedButton(
          style: buttonStyle,
          onPressed: () async {
            // Get focused window and toggle frameless mode
            final focusedHwnd = await WindowService.getFocusedFlutterWindowHandle();

            if (focusedHwnd == null) {
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('No focused Flutter window')),
              );
              return;
            }

            // Set up window interception first (required for proper frameless handling)
            final interceptionSuccess = await WindowService.setupWindowInterception(focusedHwnd);
            if (!interceptionSuccess) {
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('Failed to set up window interception')),
              );
              return;
            }

            // Toggle frameless mode
            final success = await WindowService.toggleFrameless(focusedHwnd);

            if (!context.mounted) return;
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(
                content: Text(success
                  ? 'Frameless mode toggled for 0x${focusedHwnd.toRadixString(16)}'
                  : 'Failed to toggle frameless mode'
                ),
              ),
            );
          },
          child: const Text('Toggle Frameless'),
        ),
        SizedBox(height: spacing),
        OutlinedButton(
          style: buttonStyle,
          onPressed: () async {
            // Get focused window and set transparent background
            final focusedHwnd = await WindowService.getFocusedFlutterWindowHandle();

            if (focusedHwnd == null) {
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('No focused Flutter window')),
              );
              return;
            }

            // Set up window interception first (required for proper transparency handling)
            final interceptionSuccess = await WindowService.setupWindowInterception(focusedHwnd);
            if (!interceptionSuccess) {
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('Failed to set up window interception')),
              );
              return;
            }

            // Set transparent background
            final success = await WindowService.setTransparentBackground(focusedHwnd, transparent: true);

            if (!context.mounted) return;
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(
                content: Text(success
                  ? 'Transparent background set for 0x${focusedHwnd.toRadixString(16)}'
                  : 'Failed to set transparent background'
                ),
              ),
            );
          },
          child: const Text('Set Transparent Background'),
        ),
      ],
    );
  }
}

/// A compact version of window controls for smaller spaces
class CompactWindowControlsWidget extends StatelessWidget {
  const CompactWindowControlsWidget({
    super.key,
    this.spacing = 4.0,
    this.buttonStyle,
  });

  /// Spacing between buttons
  final double spacing;

  /// Optional custom button style
  final ButtonStyle? buttonStyle;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.center,
      mainAxisSize: MainAxisSize.min,
      children: [
        OutlinedButton(
          style: buttonStyle,
          onPressed: () async {
            final focusedHwnd = await WindowService.getFocusedFlutterWindowHandle();
            if (focusedHwnd == null) {
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('No focused Flutter window')),
              );
              return;
            }

            final interceptionSuccess = await WindowService.setupWindowInterception(focusedHwnd);
            if (!interceptionSuccess) {
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('Failed to set up window interception')),
              );
              return;
            }

            final success = await WindowService.toggleTitleBar(focusedHwnd);
            if (!context.mounted) return;
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(content: Text(success ? 'Title bar toggled' : 'Failed to toggle title bar')),
            );
          },
          child: const Text('Title Bar'),
        ),
        SizedBox(height: spacing),
        OutlinedButton(
          style: buttonStyle,
          onPressed: () async {
            final focusedHwnd = await WindowService.getFocusedFlutterWindowHandle();
            if (focusedHwnd == null) {
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('No focused Flutter window')),
              );
              return;
            }

            final interceptionSuccess = await WindowService.setupWindowInterception(focusedHwnd);
            if (!interceptionSuccess) {
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('Failed to set up window interception')),
              );
              return;
            }

            final success = await WindowService.toggleFrameless(focusedHwnd);
            if (!context.mounted) return;
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(content: Text(success ? 'Frameless toggled' : 'Failed to toggle frameless')),
            );
          },
          child: const Text('Frameless'),
        ),
        SizedBox(height: spacing),
        OutlinedButton(
          style: buttonStyle,
          onPressed: () async {
            final focusedHwnd = await WindowService.getFocusedFlutterWindowHandle();
            if (focusedHwnd == null) {
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('No focused Flutter window')),
              );
              return;
            }

            final interceptionSuccess = await WindowService.setupWindowInterception(focusedHwnd);
            if (!interceptionSuccess) {
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('Failed to set up window interception')),
              );
              return;
            }

            final success = await WindowService.setTransparentBackground(focusedHwnd, transparent: true);
            if (!context.mounted) return;
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(content: Text(success ? 'Transparency set' : 'Failed to set transparency')),
            );
          },
          child: const Text('Transparency'),
        ),
      ],
    );
  }
}

/// A constrained version of window controls for very tight spaces
class ConstrainedWindowControlsWidget extends StatelessWidget {
  const ConstrainedWindowControlsWidget({
    super.key,
    this.spacing = 4.0,
    this.buttonStyle,
  });

  /// Spacing between buttons
  final double spacing;

  /// Optional custom button style
  final ButtonStyle? buttonStyle;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.center,
      mainAxisSize: MainAxisSize.min,
      children: [
        SizedBox(
          width: 200, // Fixed width to prevent layout issues
          child: OutlinedButton(
            style: buttonStyle,
            onPressed: () async {
              final focusedHwnd = await WindowService.getFocusedFlutterWindowHandle();
              if (focusedHwnd == null) {
                if (!context.mounted) return;
                ScaffoldMessenger.of(context).showSnackBar(
                  const SnackBar(content: Text('No focused Flutter window')),
                );
                return;
              }

              final interceptionSuccess = await WindowService.setupWindowInterception(focusedHwnd);
              if (!interceptionSuccess) {
                if (!context.mounted) return;
                ScaffoldMessenger.of(context).showSnackBar(
                  const SnackBar(content: Text('Failed to set up window interception')),
                );
                return;
              }

              final success = await WindowService.toggleTitleBar(focusedHwnd);
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                SnackBar(content: Text(success ? 'Title bar toggled' : 'Failed to toggle title bar')),
              );
            },
            child: const Text('Title Bar'),
          ),
        ),
        SizedBox(height: spacing),
        SizedBox(
          width: 200,
          child: OutlinedButton(
            style: buttonStyle,
            onPressed: () async {
              final focusedHwnd = await WindowService.getFocusedFlutterWindowHandle();
              if (focusedHwnd == null) {
                if (!context.mounted) return;
                ScaffoldMessenger.of(context).showSnackBar(
                  const SnackBar(content: Text('No focused Flutter window')),
                );
                return;
              }

              final interceptionSuccess = await WindowService.setupWindowInterception(focusedHwnd);
              if (!interceptionSuccess) {
                if (!context.mounted) return;
                ScaffoldMessenger.of(context).showSnackBar(
                  const SnackBar(content: Text('Failed to set up window interception')),
                );
                return;
              }

              final success = await WindowService.toggleFrameless(focusedHwnd);
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                SnackBar(content: Text(success ? 'Frameless toggled' : 'Failed to toggle frameless')),
              );
            },
            child: const Text('Frameless'),
          ),
        ),
        SizedBox(height: spacing),
        SizedBox(
          width: 200,
          child: OutlinedButton(
            style: buttonStyle,
            onPressed: () async {
              final focusedHwnd = await WindowService.getFocusedFlutterWindowHandle();
              if (focusedHwnd == null) {
                if (!context.mounted) return;
                ScaffoldMessenger.of(context).showSnackBar(
                  const SnackBar(content: Text('No focused Flutter window')),
                );
                return;
              }

              final interceptionSuccess = await WindowService.setupWindowInterception(focusedHwnd);
              if (!interceptionSuccess) {
                if (!context.mounted) return;
                ScaffoldMessenger.of(context).showSnackBar(
                  const SnackBar(content: Text('Failed to set up window interception')),
                );
                return;
              }

              final success = await WindowService.setTransparentBackground(focusedHwnd, transparent: true);
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                SnackBar(content: Text(success ? 'Transparency set' : 'Failed to set transparency')),
              );
            },
            child: const Text('Transparency'),
          ),
        ),
      ],
    );
  }
}
