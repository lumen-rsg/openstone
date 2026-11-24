streamdoublein in;
streamdoublein rhs;
streamdoubleout out;
movaps xmm0,in;
mulpd xmm0,rhs;
movaps out,xmm0;
