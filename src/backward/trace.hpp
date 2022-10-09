#pragma once

#include <nytl/span.hpp>
#include <util/linalloc.hpp>

namespace backward {

vil::span<void*> load_here(vil::LinAllocator& alloc, size_t depth);

} // namespace backward
