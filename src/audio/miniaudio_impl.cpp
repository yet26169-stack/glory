// Single compilation unit for the miniaudio implementation.
// miniaudio.h is a single-header library; defining MINIAUDIO_IMPLEMENTATION
// in exactly one .cpp pulls in all the platform-specific code.

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
