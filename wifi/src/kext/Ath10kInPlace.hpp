#pragma once

// A private placement-new tag avoids <new> and a global two-argument overload
// conflicting with kernel/user-space headers. This overload never allocates.
struct Ath10kInPlaceTag {};
inline void *operator new(decltype(sizeof(0)), Ath10kInPlaceTag, void *storage) noexcept {
    return storage;
}
inline void operator delete(void *, Ath10kInPlaceTag, void *) noexcept {}

namespace Ath10k {
// The caller must have quiesced owners before resetting protocol state.
// Construct directly in its existing storage: T{} followed by assignment can
// place a 28 KB BSS cache (or an 8 KB connection) on the kernel stack in Clang.
// Default member initializers are preserved, including nonzero sentinels.
template<class T> inline void resetInPlace(T &state) {
    static_assert(__is_trivially_copyable(T), "in-place reset is for plain protocol state only");
    state.~T();
    ::new (Ath10kInPlaceTag{}, static_cast<void *>(&state)) T();
}
}
