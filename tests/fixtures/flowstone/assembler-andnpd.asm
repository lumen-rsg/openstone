streamdoublein in;
streamdoublein rhs;
streamdoubleout out;
movapd xmm0,in;
cmppd xmm0,rhs,1;
movapd xmm1,rhs;
andnpd xmm0,xmm1;
movapd out,xmm0;
