streamdoublein in;
streamdoublein rhs;
streamdoubleout out;
movapd xmm0,in;
cmppd xmm0,rhs,5;
movapd xmm1,in;
xorpd xmm0,xmm1;
movapd out,xmm0;
