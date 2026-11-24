streamin in;
streamin rhs;
streamout out;
movaps xmm0,in;
subps xmm0,rhs;
movaps out,xmm0;
