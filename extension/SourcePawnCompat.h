#ifndef _INCLUDE_ACCELERATOR_SOURCEPAWN_COMPAT_H_
#define _INCLUDE_ACCELERATOR_SOURCEPAWN_COMPAT_H_

#include "smsdk_ext.h"

// Compile a distinct binary for each SourceMod target. API 10 is intentionally
// excluded: the pinned 1.13 loader rejects that transitional interface.
#if SMINTERFACE_EXTENSIONAPI_VERSION == 8
using AcceleratorPublicRuntime = SourcePawn::IPluginRuntime;
inline AcceleratorPublicRuntime *GetAcceleratorPublicRuntime(SourcePawn::IPluginRuntime *runtime) {
	return runtime;
}
#elif SMINTERFACE_EXTENSIONAPI_VERSION == 9 || SMINTERFACE_EXTENSIONAPI_VERSION == 11
#include <base-runtime.h>
using AcceleratorPublicRuntime = sp::BaseRuntime;
inline AcceleratorPublicRuntime *GetAcceleratorPublicRuntime(SourcePawn::IPluginRuntime *runtime) {
	return runtime ? runtime->GetBaseRuntime() : nullptr;
}
#else
#error Unsupported SourceMod extension API; update the compatibility layer and dependency pins.
#endif

#endif
