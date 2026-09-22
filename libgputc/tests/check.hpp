#pragma once

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::abort();                                                          \
    }                                                                        \
  } while (0)
