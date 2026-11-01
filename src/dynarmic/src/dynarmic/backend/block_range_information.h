// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <map>

#include <boost/icl/interval_set.hpp>
#include "common/container/unordered_set.h"

#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend {

template<typename P>
class BlockRangeInformation {
public:
    void AddRange(boost::icl::discrete_interval<P> range, IR::LocationDescriptor location);
    void ClearCache();
    ::Common::unordered_set<IR::LocationDescriptor> InvalidateRanges(const boost::icl::interval_set<P>& invalidated);

private:
    /// The guest code [first, last] a block was translated from, keyed by first. An invalidation
    /// walks the blocks starting within max_span before its interval instead of every block (an
    /// interval map of block sets held ~300 bytes per block).
    struct Range {
        P last;
        IR::LocationDescriptor location;
    };
    std::multimap<P, Range> ranges;
    P max_span{};
};

}  // namespace Dynarmic::Backend
