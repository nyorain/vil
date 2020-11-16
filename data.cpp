#include "data.hpp"

namespace fuen {

std::unordered_map<std::uint64_t, void*> dataTable;
std::shared_mutex dataMutex;

} // namespace fuen
