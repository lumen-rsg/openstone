streamdoublein in;
streamdoublein rhs;
streamdoubleout out;
movapd xmm0,in;
cmppd xmm0,rhs,3;
orpd xmm0,rhs;
movapd out,xmm0;
