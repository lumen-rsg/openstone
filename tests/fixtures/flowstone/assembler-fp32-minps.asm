streamin in;
streamin rhs;
streamout out;
movaps xmm0,in;
minps xmm0,rhs;
movaps out,xmm0;
