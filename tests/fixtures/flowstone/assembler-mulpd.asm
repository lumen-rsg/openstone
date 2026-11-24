streamdoublein in;
streamdoubleout out;
double coefficient = 0x3fe0000000002000;
movaps xmm0,in;
movaps xmm3,xmm0;
mulpd xmm3,coefficient;
movaps out,xmm3;
