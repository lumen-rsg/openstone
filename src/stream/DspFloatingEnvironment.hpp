#pragma once

#include <cfenv>
#include <cstdint>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

namespace openstone::stream::detail {

// DSP rounding and explicit denormal operations must not inherit a plug-in host's
// rounding, flush, default-NaN or exception-trap settings. Restore its whole
// environment afterward, including sticky exception flags. No global state.
class DspFloatingEnvironment {
public:
    DspFloatingEnvironment() noexcept : saved_(std::feholdexcept(&environment_) == 0)
    {
        if (!saved_) return;
        std::fesetround(FE_TONEAREST);
#if defined(__aarch64__) || defined(__arm64__)
        std::uint64_t control;
        __asm__ volatile("mrs %0, fpcr" : "=r"(control));
        control &= ~((std::uint64_t{1} << 24) | (std::uint64_t{1} << 25) | (std::uint64_t{1} << 19));
        __asm__ volatile("msr fpcr, %0" : : "r"(control));
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
        _mm_setcsr(_mm_getcsr() & ~0x8040U); // Disable FTZ and DAZ; bytecode implements them per instruction.
#endif
    }
    ~DspFloatingEnvironment() { if (saved_) std::fesetenv(&environment_); }
    DspFloatingEnvironment(const DspFloatingEnvironment&) = delete;
    DspFloatingEnvironment& operator=(const DspFloatingEnvironment&) = delete;
private:
    std::fenv_t environment_;
    bool saved_;
};

}
