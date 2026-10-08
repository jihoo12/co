#pragma once
#include "mir.h"

namespace co {

mir::Module buildMir(Program &prog, TypeContext &tc, Diagnostics &diag);

} // namespace co
