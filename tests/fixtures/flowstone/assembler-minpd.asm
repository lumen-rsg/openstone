streamdoublein in;
streamdoublein rhs;
streamdoubleout out;
movapd xmm0,in;
movapd xmm1,rhs;
minpd xmm0,xmm1;
movapd out,xmm0;
