// Source order recovered from debug info (see include/papyrus/layout.hpp).
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/layout.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

#include "compile/dotnet_hashtable.hpp"

namespace papyrus::pex {

namespace {

constexpr size_t kNoKey = std::numeric_limits<size_t>::max();

// Index of the first debug record matching (object, state, name, kind).
size_t record_index(const Script& s, StrIdx object, StrIdx state, StrIdx name, FunctionKind kind) {
    const auto& recs = s.debug.functions;
    for (size_t i = 0; i < recs.size(); ++i) {
        const FunctionDebug& d = recs[i];
        if (d.object == object && d.state == state && d.name == name && d.kind == kind) return i;
    }
    return kNoKey;
}

template <class Key>
std::vector<size_t> order_by(size_t n, const Key& key) {
    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return key(a) < key(b); });
    return idx;
}

}  // namespace

SourceLayout source_layout(const Script& s, const Object& o) {
    SourceLayout out;
    out.functions.resize(o.states.size());
    const bool dbg = s.debug.present && !s.debug.functions.empty();
    out.fromDebugInfo = dbg;

    // Index of the default state (name ""), if any.
    size_t defaultState = kNoKey;
    for (size_t i = 0; i < o.states.size(); ++i) {
        if (s.str(o.states[i].name).empty()) {
            defaultState = i;
            break;
        }
    }

    // Find the empty-string index once; a record labelled with the default
    // state uses it. Without the string the file has no default-state label.
    StrIdx emptyIdx = 0;
    bool haveEmpty = s.find_string("", emptyIdx);

    std::vector<size_t> stateKey(o.states.size(), kNoKey);
    for (size_t si = 0; si < o.states.size(); ++si) {
        const State& st = o.states[si];
        std::vector<size_t> keys(st.functions.size(), kNoKey);
        if (dbg) {
            for (size_t fi = 0; fi < st.functions.size(); ++fi) {
                const NamedFunction& f = st.functions[fi];
                // Exact label first (the first function of a non-default state,
                // or any default-state function).
                size_t k = record_index(s, o.name, st.name, f.name, FunctionKind::Method);
                if (si != defaultState) {
                    if (k != kNoKey) {
                        // The labelled record marks the first function of the
                        // block, wherever the record sits; the state's own
                        // position is that record's index.
                        stateKey[si] = std::min(stateKey[si], k);
                        k = 0;
                    } else if (haveEmpty) {
                        // The rest of the block is labelled with the default state.
                        k = record_index(s, o.name, emptyIdx, f.name, FunctionKind::Method);
                        if (k != kNoKey) ++k;
                    }
                }
                keys[fi] = k;
            }
        }
        out.functions[si] = order_by(st.functions.size(), [&](size_t i) { return keys[i]; });
        if (si == defaultState) stateKey[si] = 0;
        if (stateKey[si] == kNoKey && !keys.empty()) {
            // No labelled record (debug stripped for this state): use the
            // earliest record any of its functions has, else the end.
            size_t best = kNoKey;
            for (size_t k : keys) best = std::min(best, k);
            if (best != kNoKey) stateKey[si] = best + 1;  // after the labelled ones at the same index
        }
    }
    out.states = order_by(o.states.size(), [&](size_t i) { return stateKey[i]; });
    // The default state always leads.
    if (defaultState != kNoKey) {
        out.states.erase(std::find(out.states.begin(), out.states.end(), defaultState));
        out.states.insert(out.states.begin(), defaultState);
    }

    std::vector<size_t> propKey(o.properties.size(), kNoKey);
    if (dbg && haveEmpty) {
        for (size_t pi = 0; pi < o.properties.size(); ++pi) {
            const Property& p = o.properties[pi];
            if (p.is_auto()) continue;
            size_t k = kNoKey;
            if (p.getter) k = std::min(k, record_index(s, o.name, emptyIdx, p.name, FunctionKind::Getter));
            if (p.setter) k = std::min(k, record_index(s, o.name, emptyIdx, p.name, FunctionKind::Setter));
            propKey[pi] = k;
        }
    }
    out.properties = order_by(o.properties.size(), [&](size_t i) { return propKey[i]; });
    return out;
}

std::vector<size_t> source_state_order(const Script& s, const Object& o, const SourceLayout& layout) {
    std::vector<size_t> order = layout.states;
    if (!layout.fromDebugInfo || order.size() < 2) return order;
    size_t first = 0;
    if (!order.empty() && s.str(o.states[order[0]].name).empty()) first = 1;
    std::vector<size_t> named(order.begin() + first, order.end());
    if (named.size() < 2 || named.size() > 8) return order;
    auto keys_of = [&](const std::vector<size_t>& states) {
        std::vector<std::string> keys;
        for (size_t si : states) {
            std::string k(s.str(o.states[si].name));
            for (char& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            keys.push_back(k);
        }
        return keys;
    };
    // Only states with a function have a debug record, so only their
    // relative order is known; a state without functions may land anywhere
    // (vanilla DLC2DunFloorPathPuzzleSCRIPT: `State Done` is empty, and the
    // records only say busy before WaitingToBeActivated).
    auto constrained = [&](const std::vector<std::string>& keys) {
        std::vector<std::string> out;
        for (const std::string& k : keys) {
            for (size_t si : named) {
                std::string n(s.str(o.states[si].name));
                for (char& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (n == k && !o.states[si].functions.empty()) out.push_back(k);
            }
        }
        return out;
    };
    const std::vector<std::string> listing = constrained(keys_of(named));
    if (constrained(dotnet::hashtable20_key_order(keys_of(named))) == listing) return order;
    std::vector<size_t> trial = named;
    std::sort(trial.begin(), trial.end());
    do {
        if (constrained(dotnet::hashtable20_key_order(keys_of(trial))) == listing) {
            std::copy(trial.begin(), trial.end(), order.begin() + first);
            return order;
        }
    } while (std::next_permutation(trial.begin(), trial.end()));
    return order;
}

}  // namespace papyrus::pex
