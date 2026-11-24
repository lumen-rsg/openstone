streamdoublein in;
streamdoublein rhs;
streamdoubleout out;
movaps xmm0,in;
minpd xmm0,rhs;
movaps out,xmm0;
