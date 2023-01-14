// STL
#include <vector>
#include <string>
#include <memory>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <list>
#include <chrono>
#include <initializer_list>
#include <array>
#include <cstdint>
#include <climits>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <new>
#include <any>
#include <bitset>
#include <functional>
#include <optional>
#include <variant>
#include <string_view>
#include <algorithm>
#include <utility>
#include <tuple>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <atomic>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <future>
#include <type_traits>

#include <vk/vulkan.h>
#include <vk/dispatch_table.h>
#include <vk/object_types.h>
#include <vk/typemap_helper.h>
#include <vkutil/enumString.hpp>

#include <spirv-cross/spirv_cross.hpp>

#include <nytl/span.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/flags.hpp>

#include <util/syncedMap.hpp>
#include <util/intrusive.hpp>
#include <util/debugMutex.hpp>
#include <util/profiling.hpp>

#include <util/dlg.hpp>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <imgui/textedit.h>
