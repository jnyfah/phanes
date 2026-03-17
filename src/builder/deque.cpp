module;

#include <cstddef>
#include <memory>
#include <stdexcept>

module builder:deque;

template <typename T>
class LockFreeDeque
{
  public:
    LockFreeDeque() : data(std::make_unique<T[]>(capacity)) {}

    auto push_back(const T& item) -> bool
    {
        if (size == capacity)
        {
            grow();
        }

        const std::size_t index = (front + size) % capacity;
        data[index] = item;
        ++size;
        return true;
    }

    auto pop_back() -> T
    {
        if (size == 0)
        {
            throw std::out_of_range("deque is empty");
        }

        const std::size_t index = (front + size - 1) % capacity;
        T value = data[index];
        --size;
        return value;
    }

    auto pop_front() -> T
    {
        if (size == 0)
        {
            throw std::out_of_range("deque is empty");
        }

        T value = data[front];
        front = (front + 1) % capacity;
        --size;
        return value;
    }

    [[nodiscard]] auto empty() const -> bool { return size == 0; }

    [[nodiscard]] auto Size() const -> std::size_t { return size; }

  private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
    std::unique_ptr<T[]> data;
    std::size_t capacity{16};
    std::size_t front{0};
    std::size_t size{0};

    void grow()
    {
        std::size_t new_capacity = capacity * 2;
        auto new_data = std::make_unique<T[]>(new_capacity);

        for (std::size_t i = 0; i < size; ++i)
        {
            new_data[i] = data[(front + i) % capacity];
        }

        data = std::move(new_data);
        capacity = new_capacity;
        front = 0;
    }
};