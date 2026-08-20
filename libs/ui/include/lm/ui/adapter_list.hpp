#pragma once

#include "lm/ui/export.hpp"

#include <QTreeWidget>

#include <vector>

#include "lm/core/host_facts.hpp"

namespace lm::ui {

/// The machine's network adapters: name, description, type and link state.
///
/// Shared by both detail panes rather than written twice — the client and the
/// server show the same list of the same facts, and two copies would drift the
/// way the compliance list did before RuleDetail moved here.
class LM_UI_EXPORT AdapterList : public QTreeWidget {
    Q_OBJECT

public:
    explicit AdapterList(QWidget* parent = nullptr);

    /// Replaces the contents. Connected adapters are listed first, then
    /// disconnected ones, each group alphabetical — a machine with twenty
    /// interfaces is usually being read for "what is actually up".
    void set_adapters(const std::vector<core::NetworkAdapter>& adapters);

    /// Shown instead of a list when the client does not advertise
    /// Capability::Network, which is not the same as having no adapters.
    void set_not_reported();
};

}  // namespace lm::ui
