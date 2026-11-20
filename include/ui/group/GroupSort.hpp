#pragma once

#include <QString>

namespace GroupSortMethod {
    enum GroupSortMethod {
        Raw,
        ByType,
        ByAddress,
        ByName,
        ByTestResult,
        ById,
        ByTraffic,
        BySecurity,
        // ByTestResult pinned to latency, whatever the group's test_sort_by.
        ByLatency,
        // Shadowlos: by one checked service (GroupSortAction::service): opens, slow, fails, unchecked.
        ByService,
    };
}

struct GroupSortAction {
    GroupSortMethod::GroupSortMethod method = GroupSortMethod::Raw;
    bool descending = false;
    QString service;
};
