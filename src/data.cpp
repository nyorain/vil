#include "data.hpp"

namespace vil {

std::unordered_map<std::uint64_t, void*> dispatchableTable;
std::unordered_map<void*, Device*> devByLoaderTable;
std::shared_mutex dataMutex;

} // namespace vil
