#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <span>

namespace cosync::net
{
    // Monotonic milliseconds. Injected into session classes so tests control time.
    using Clock = std::function<std::uint64_t()>;
    [[nodiscard]] std::uint64_t SteadyNowMs();

    // Cryptographically secure random bytes (BCryptGenRandom). Returns false on failure.
    [[nodiscard]] bool FillRandom(std::span<std::uint8_t> out);

    // Classic token bucket: `ratePerSecond` sustained with bursts up to `burst`.
    class TokenBucket
    {
    public:
        TokenBucket() = default;
        TokenBucket(double ratePerSecond, double burst, std::uint64_t nowMs) :
            _rate(ratePerSecond / 1000.0), _burst(burst), _tokens(burst), _last(nowMs)
        {}

        bool TryTake(std::uint64_t nowMs, double cost = 1.0)
        {
            if (nowMs > _last) {
                _tokens = std::min(_burst, _tokens + static_cast<double>(nowMs - _last) * _rate);
                _last = nowMs;
            }
            if (_tokens < cost) {
                return false;
            }
            _tokens -= cost;
            return true;
        }

    private:
        double _rate = 0.0;  // tokens per ms
        double _burst = 0.0;
        double _tokens = 0.0;
        std::uint64_t _last = 0;
    };
}
