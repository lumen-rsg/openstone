streamdoublein in;
streamdoublein rhs;
streamdoubleout out;
movapd xmm0,in;
cmppd xmm0,rhs,5;
andpd xmm0,in;
movapd out,xmm0;
