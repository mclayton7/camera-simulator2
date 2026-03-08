// rapidyaml amalgamated implementation trigger.
// This file compiles the ryml library from the single-header amalgamation.
// See https://github.com/biojppm/rapidyaml

// Suppress warnings from third-party amalgamated header
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wfloat-conversion"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wextra-semi"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wswitch-enum"
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#pragma clang diagnostic ignored "-Wundef"
#endif

// UE5 CoreDefines.h defines DEFAULTS as 0, which collides with a ryml enum member.
#pragma push_macro("DEFAULTS")
#undef DEFAULTS

#define RYML_SINGLE_HDR_DEFINE_NOW
#include "ryml_all.hpp"

#pragma pop_macro("DEFAULTS")

#ifdef __clang__
#pragma clang diagnostic pop
#endif
