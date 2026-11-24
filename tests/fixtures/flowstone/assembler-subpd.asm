streamdoublein in;
streamdoubleout out;
double coefficient = 0x3fe0000000002000;
movupd xmm0,in;
movapd xmm1,coefficient;
subpd xmm0,xmm1;
movapd out,xmm0;
