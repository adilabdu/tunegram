// Internal access for tests and tools linked against the static library.
// Not part of the public C API.
#pragma once

#include "session.hpp"
#include "tunegram/lt_core.h"

namespace lt {
Session* session_of(lt_core* core);
}  // namespace lt
