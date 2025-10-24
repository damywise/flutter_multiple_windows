import 'package:flutter/foundation.dart';

// Shared state using ValueNotifier - this will work across windows in the same process
final sharedCounter = ValueNotifier<int>(0);

class SharedState {
  static int get counter => sharedCounter.value;
  static set counter(int value) => sharedCounter.value = value;

  static void incrementCounter() {
    sharedCounter.value++;
  }

  static void decrementCounter() {
    sharedCounter.value--;
  }

  static void resetCounter() {
    sharedCounter.value = 0;
  }
}
