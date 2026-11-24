streamin in;
streamin rhs;
streamout out;
movaps xmm0,in;
maxps xmm0,rhs;
movaps out,xmm0;
