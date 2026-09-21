#pragma once

#include "LspTypes.h"
#include <string_view>

namespace lsp {

inline std::string_view toString(CompletionTriggerKind kind) {
    switch (kind) {
        case CompletionTriggerKind::Invoked:
            return "Invoked";
        case CompletionTriggerKind::TriggerCharacter:
            return "TriggerCharacter";
        case CompletionTriggerKind::TriggerForIncompleteCompletions:
            return "TriggerForIncompleteCompletions";
    }
    return "Unknown";
}

struct InactiveRegionsClientCapabilities {
    std::optional<bool> inactiveRegions;
};

struct SlangClientInfo {
    std::optional<std::string> name;
    std::optional<std::string> version;
};

struct ExperimentalClientCapabilities {
    std::optional<InactiveRegionsClientCapabilities> inactiveRegions;
    std::optional<SlangClientInfo> slangClient;
};

struct InactiveRegionsParams {
    URI uri;
    std::vector<Range> regions;
};

/// Sent when a handler failed in a way the server recovered from, so the client can point the user
/// at the log instead of letting the failure pass silently.
struct InternalErrorParams {
    /// Method being handled when the failure happened
    std::string method;
    /// Short description of the failure; the full details are in the server log
    std::string message;
};

} // namespace lsp
