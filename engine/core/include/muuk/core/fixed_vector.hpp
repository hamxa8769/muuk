#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <initializer_list>

namespace muuk::core {

// A stack-allocated, fixed-capacity vector. Used everywhere the engine needs
// a growable collection (orders, commands, ...) without touching the heap
// on the hot path. Capacity is a compile-time constant so callers must size
// it from RiskLimits (max orders) up front.
template <typename T, std::size_t Capacity>
class FixedVector {
public:
    using value_type = T;
    using iterator = typename std::array<T, Capacity>::iterator;
    using const_iterator = typename std::array<T, Capacity>::const_iterator;

    FixedVector() = default;

    void push_back(const T& value) {
        assert(size_ < Capacity && "FixedVector capacity exceeded");
        data_[size_++] = value;
    }

    void push_back(T&& value) {
        assert(size_ < Capacity && "FixedVector capacity exceeded");
        data_[size_++] = std::move(value);
    }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        assert(size_ < Capacity && "FixedVector capacity exceeded");
        data_[size_] = T(std::forward<Args>(args)...);
        return data_[size_++];
    }

    void erase_at(std::size_t index) {
        assert(index < size_);
        for (std::size_t i = index; i + 1 < size_; ++i) {
            data_[i] = data_[i + 1];
        }
        --size_;
    }

    void clear() { size_ = 0; }

    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] constexpr std::size_t capacity() const { return Capacity; }
    [[nodiscard]] bool full() const { return size_ == Capacity; }

    T& operator[](std::size_t index) { return data_[index]; }
    const T& operator[](std::size_t index) const { return data_[index]; }

    iterator begin() { return data_.begin(); }
    iterator end() { return data_.begin() + static_cast<std::ptrdiff_t>(size_); }
    const_iterator begin() const { return data_.begin(); }
    const_iterator end() const { return data_.begin() + static_cast<std::ptrdiff_t>(size_); }

private:
    std::array<T, Capacity> data_{};
    std::size_t size_ = 0;
};

}  // namespace muuk::core
