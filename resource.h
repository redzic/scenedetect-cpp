#pragma once

#include <memory>

// TODO is there an alternative to unique_ptr
// that compiles to the most efficient code possible?
template <typename T, auto Alloc, auto Free> auto make_resource() {
    return std::unique_ptr<T, decltype([](T* ptr) { Free(&ptr); })>(Alloc());
}

// TODO make this as always-inline function that doesn't call extra
// constructors and stuff
#define make_file(variable_name, file_name, file_args)                         \
    std::unique_ptr<FILE, decltype(&fclose)> variable_name(                    \
        fopen(file_name, file_args), fclose)

// TODO how do I disable warnings?
#ifdef NDEBUG
constexpr inline void DvAssert(bool /*unused*/) {}
#else // DEBUG
#include <cassert>
#define DvAssert assert
#endif