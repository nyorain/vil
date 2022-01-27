#pragma once

#include "../bugged.hpp"
#include <vk/vulkan.h>
#include <util/dlg.hpp>
#include "util.hpp"

using namespace tut;
extern Setup gSetup;
inline const Setup& getSetup() {
	return gSetup;
}
