#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace cosync
{
    // Multi-producer/multi-consumer FIFO with a hard capacity. Never blocks and
    // never grows past `capacity`; TryPush reports overflow so the producer can
    // decide how to recover (e.g. request a full resync).
    template <class T>
    class BoundedQueue
    {
    public:
        explicit BoundedQueue(std::size_t capacity) : _capacity(capacity) {}

        bool TryPush(T value)
        {
            std::scoped_lock lock(_mutex);
            if (_items.size() >= _capacity) {
                return false;
            }
            _items.push_back(std::move(value));
            return true;
        }

        // Moves up to `max` items into `out`; returns how many were moved.
        std::size_t Drain(std::vector<T>& out, std::size_t max)
        {
            std::scoped_lock lock(_mutex);
            std::size_t n = 0;
            while (n < max && !_items.empty()) {
                out.push_back(std::move(_items.front()));
                _items.pop_front();
                ++n;
            }
            return n;
        }

        void Clear()
        {
            std::scoped_lock lock(_mutex);
            _items.clear();
        }

        [[nodiscard]] std::size_t Size() const
        {
            std::scoped_lock lock(_mutex);
            return _items.size();
        }

        [[nodiscard]] std::size_t Capacity() const noexcept { return _capacity; }

    private:
        mutable std::mutex _mutex;
        std::deque<T> _items;
        std::size_t _capacity;
    };

    // Keeps only the newest value per key. Used for state where intermediate
    // values are worthless (e.g. remote transforms while the game thread is
    // stalled on a loading screen); memory is bounded by the number of keys.
    template <class Key, class Value>
    class LatestValueMailbox
    {
    public:
        void Put(const Key& key, Value value)
        {
            std::scoped_lock lock(_mutex);
            _values.insert_or_assign(key, std::move(value));
        }

        void Erase(const Key& key)
        {
            std::scoped_lock lock(_mutex);
            _values.erase(key);
        }

        void TakeAll(std::vector<Value>& out)
        {
            std::unordered_map<Key, Value> taken;
            {
                std::scoped_lock lock(_mutex);
                taken.swap(_values);
            }
            for (auto& [k, v] : taken) {
                out.push_back(std::move(v));
            }
        }

        void Clear()
        {
            std::scoped_lock lock(_mutex);
            _values.clear();
        }

    private:
        std::mutex _mutex;
        std::unordered_map<Key, Value> _values;
    };
}
