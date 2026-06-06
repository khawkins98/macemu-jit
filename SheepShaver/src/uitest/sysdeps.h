// Test-only stub of sysdeps.h for the offline ui_introspect serializer harness.
// Provides just the integer typedefs the serializers use. Selected over the real
// src/Unix/sysdeps.h only when this dir is first on the include path (the
// ui-introspect-serialize-test target); the real build is untouched.
#ifndef SYSDEPS_H
#define SYSDEPS_H

#include <cstdint>
#include <cstddef>

typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;
typedef int64_t  int64;

#endif // SYSDEPS_H
