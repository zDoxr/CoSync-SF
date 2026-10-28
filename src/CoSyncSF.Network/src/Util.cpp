#include "cosync/net/Util.h"

#include <Windows.h>
#include <bcrypt.h>

#include <chrono>

namespace cosync::net
{
    std::uint64_t SteadyNowMs()
    {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

    bool FillRandom(std::span<std::uint8_t> out)
    {
        const NTSTATUS status = BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                                                BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        return BCRYPT_SUCCESS(status);
    }
}
