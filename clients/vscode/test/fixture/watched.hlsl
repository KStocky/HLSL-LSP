#include "watched.hlsli"

[numthreads(1, 1, 1)]
void watchedMain() {
  float value = watchedValue;
}
