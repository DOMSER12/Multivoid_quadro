// ue_wrap/core/class_census.h -- one diagnostic walk of the object index's class log: the loaded
// classes whose short name contains one of the caller's substrings, each with the UFunctions its
// class object declares, named in the log. A lane that names a class the running build spells
// otherwise would otherwise stay silent -- a name watch whose class never loads waits forever,
// and a FindClass miss retries without a word -- so the lane runs this while it cannot resolve,
// and the real spelling (if this build carries one) lands in the log.
//
// Engine-wrapper layer (principle 7): a census is a read, never a writer, and knows no gameplay.
// Game thread only (the class log and the reflection walks are).

#pragma once

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <cstddef>
#include <cwchar>
#include <string>

namespace ue_wrap::class_census {
namespace R = ue_wrap::reflection;

// True when `s` contains `sub`, compared without case, the way the engine compares names.
inline bool NameContains(const std::wstring& s, const wchar_t* sub) {
    if (!sub || !*sub) return false;
    const size_t n = std::wcslen(sub);
    if (s.size() < n) return false;
    for (size_t i = 0; i + n <= s.size(); ++i) {
        size_t j = 0;
        while (j < n) {
            wchar_t a = s[i + j], b = sub[j];
            if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
            if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
            if (a != b) break;
            ++j;
        }
        if (j == n) return true;
    }
    return false;
}

// The census: every listed, finished-loading class whose short name contains one of `subs`, one
// log line each with its declared UFunctions (capped). Returns how many classes matched, so the
// caller can say "none" -- which is itself the answer (the family does not exist here, or no
// instance of it has loaded yet). `who` names the lane in the lines.
inline int LogMatchingClasses(const char* who, const wchar_t* const* subs, size_t nSubs,
                              int maxClasses = 12, int maxFnsPerClass = 32) {
    const size_t listed = object_index::ClassLogSize();
    int said = 0;
    for (size_t i = 0; i < listed && said < maxClasses; ++i) {
        bool loaded = false;
        void* cls = object_index::ClassLogAt(i, &loaded);
        if (!cls || !loaded) continue;
        const std::wstring name = R::ToString(R::NameOf(cls));
        if (name.rfind(L"Default__", 0) == 0) continue;  // the CDO's class object carries the real name too
        bool match = false;
        for (size_t k = 0; k < nSubs && !match; ++k) match = NameContains(name, subs[k]);
        if (!match) continue;
        std::wstring fns;
        int saidFns = 0;
        for (const R::ObjectRef& ref : R::ChildObjectsOf(cls)) {
            if (ref.className != L"Function") continue;
            if (!fns.empty()) fns += L" ";
            fns += ref.name;
            if (++saidFns >= maxFnsPerClass) { fns += L" ..."; break; }
        }
        UE_LOGI("%s: census -- class '%ls' declares: %ls", who, name.c_str(),
                fns.empty() ? L"<no functions>" : fns.c_str());
        ++said;
    }
    return said;
}

}  // namespace ue_wrap::class_census
