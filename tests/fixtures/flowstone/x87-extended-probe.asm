// OpenStone's full 80-bit model: 1, 1, 1, 0, 1, 4, 1, 0.
// Actual FlowStone readouts reveal its effective x87 precision setting.
streamout keep24;
streamout keep53;
streamout keep63;
streamout limit64;
streamout recoverHigh;
streamout recoverLow;
streamout keepAfterStore;
streamout storedDouble;
double big24 = 0x4170000000000000;
double big53 = 0x4340000000000000;
double big63 = 0x43e0000000000000;
double big64 = 0x43f0000000000000;
double huge = 0x7fe0000000000000;
double tiny = 0x0010000000000000;
double two = 2;
double half = 0.5;
double saved;

dfld big24; fld1; fadd; dfld big24; fsub; fstp keep24;
dfld big53; fld1; fadd; dfld big53; fsub; fstp keep53;
dfld big63; fld1; fadd; dfld big63; fsub; fstp keep63;
dfld big64; fld1; fadd; dfld big64; fsub; fstp limit64;
dfld huge; dfld two; fmul; dfld half; fmul; dfld huge; fdivp; fstp recoverHigh;
fld1; dfld tiny; fmul; dfld tiny; fmul; dfld huge; fmul; dfld huge; fmul; fstp recoverLow;
dfld big53; fld1; fadd; dfst saved; dfld big53; fsub; fstp keepAfterStore;
dfld saved; dfld big53; fsub; fstp storedDouble;
