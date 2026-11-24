streamdoublein in;
streamdoublein rhs;
streamdoubleout out;
movapd xmm0,in;
maxpd xmm0,rhs;
movapd out,xmm0;
