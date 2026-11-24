#pragma once

#include <cmath>
#include <concepts>
#include <limits>

namespace openstone::math {

template<std::floating_point Scalar>
[[nodiscard]] Scalar logarithm(Scalar value, Scalar base) noexcept
{
    if (base <= Scalar{0} || base == Scalar{1} || !std::isfinite(base)) {
        return std::numeric_limits<Scalar>::quiet_NaN();
    }
    return std::log(value) / std::log(base);
}

}
