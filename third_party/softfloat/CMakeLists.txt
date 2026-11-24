add_library(openstone_softfloat STATIC
    8086/s_commonNaNToExtF80UI.c
    8086/s_commonNaNToF32UI.c
    8086/s_commonNaNToF64UI.c
    8086/s_extF80UIToCommonNaN.c
    8086/s_f32UIToCommonNaN.c
    8086/s_f64UIToCommonNaN.c
    8086/s_propagateNaNExtF80UI.c
    8086/softfloat_raiseFlags.c
    extF80_add.c
    extF80_div.c
    extF80_mul.c
    extF80_rem.c
    extF80_roundToInt.c
    extF80_sub.c
    extF80_to_f32.c
    extF80_to_f64.c
    extF80_to_i32.c
    f32_to_extF80.c
    f64_to_extF80.c
    i32_to_extF80.c
    s_addMagsExtF80.c
    s_approxRecip32_1.c
    s_approxRecip_1Ks.c
    s_countLeadingZeros64.c
    s_countLeadingZeros8.c
    s_mul64To128.c
    s_normRoundPackToExtF80.c
    s_normSubnormalExtF80Sig.c
    s_normSubnormalF32Sig.c
    s_normSubnormalF64Sig.c
    s_roundPackToExtF80.c
    s_roundPackToF32.c
    s_roundPackToF64.c
    s_roundToI32.c
    s_shiftRightJam128.c
    s_subMagsExtF80.c
    softfloat_state.c
)
set_target_properties(openstone_softfloat PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(openstone_softfloat PUBLIC SOFTFLOAT_FAST_INT64 PRIVATE INLINE_LEVEL=5)
option(OPENSTONE_SOFTFLOAT_PORTABLE "Disable compiler-specific SoftFloat integer intrinsics" OFF)
if(OPENSTONE_SOFTFLOAT_PORTABLE)
    target_compile_definitions(openstone_softfloat PRIVATE OPENSTONE_SOFTFLOAT_PORTABLE)
endif()
target_include_directories(openstone_softfloat PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/8086")
