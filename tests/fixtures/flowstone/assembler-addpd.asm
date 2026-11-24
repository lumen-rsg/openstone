streamdoublein in;
streamdoubleout out;
double coefficient = 0x3fe0000000002000;
movapd xmm0,in;
addpd xmm0,coefficient;
movupd out,xmm0;
