#include "lm/platform/probes.hpp"

namespace lm::platform {

/// No script execution on Linux yet: the feature is PowerShell-shaped -- the
/// shell, the LM-RESULT convention and the job-object kill are all Windows --
/// and a half-implementation that ran /bin/sh without the process-tree kill
/// would be worse than none.
///
/// Returning nullptr is how the client learns that: it drops
/// core::Capability::Scripts, so the server shows the host as unable to run
/// scripts rather than dispatching one that could only ever be refused.
std::unique_ptr<IScriptRunner> make_script_runner() { return nullptr; }

}  // namespace lm::platform
