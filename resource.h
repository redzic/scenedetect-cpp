#pragma once

#include <memory>

template <typename T, auto Alloc, auto Free> auto make_resource() {
    return std::unique_ptr<T, decltype([](T* ptr) { Free(&ptr); })>(Alloc());
}
