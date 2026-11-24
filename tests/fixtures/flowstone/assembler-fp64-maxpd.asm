streamdoublein in;
streamdoublein rhs;
streamdoubleout out;
movaps xmm0,in;
maxpd xmm0,rhs;
movaps out,xmm0;
