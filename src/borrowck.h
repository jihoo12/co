#pragma once
#include "mir.h"

namespace co {

// Checks ownership and borrowing rules on a MIR function:
//  * no use of moved or uninitialized values (and every path returns a value)
//  * no conflicting borrows: many `&` or exactly one `&mut` at a time
//  * no borrow outlives the value it points to
// Borrows last only as long as they are used (non-lexical lifetimes).
void borrowCheck(const mir::Function &f, Diagnostics &diag);

} // namespace co
