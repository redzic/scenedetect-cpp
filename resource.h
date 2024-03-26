#pragma once

#include <memory>

// TODO is there an alternative to unique_ptr
// that compiles to the most efficient code possible?
template <typename T, auto Alloc, auto Free> auto make_resource() {
    return std::unique_ptr<T, decltype([](T* ptr) { Free(&ptr); })>(Alloc());
}

// TODO how do I disable warnings?
#ifdef NDEBUG
constexpr inline void DvAssert(bool /*unused*/) {}
#else // DEBUG
#include <cassert>
#define DvAssert assert
#endif