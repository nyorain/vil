#pragma once

#include <tracy/Tracy.hpp>

#ifdef VIL_EXTENSIVE_ZONES
	#define ExtZoneScopedN(x) ZoneScopedN(x)
	#define ExtZoneScoped ZoneScoped
#else // VIL_EXTENSIVE_ZONES
	#define ExtZoneScopedN(x)
	#define ExtZoneScoped
#endif // VIL_EXTENSIVE_ZONES

