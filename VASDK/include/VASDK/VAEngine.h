#pragma once

#include "vasdk_export.h"

namespace va {

/**
 * @brief VAEngine - UNIMPLEMENTED STUB
 *
 * This class is a placeholder for a future high-level engine API.
 * All methods return failure or are no-ops.  Do NOT use this class in
 * production code - it will be fleshed out in a future release.
 *
 * Use the Pipeline / FrameProcessor API directly for now.
 *
 * @deprecated Use va::pipeline::Pipeline instead.
 */
class [[deprecated("VAEngine is a non-functional stub; use va::pipeline::Pipeline instead")]]
VASDK_EXPORT VAEngine {
public:
    VAEngine();

    bool initialize() { return false; }
    bool start()      { return false; }
    void stop()       {}
};

} // namespace va
