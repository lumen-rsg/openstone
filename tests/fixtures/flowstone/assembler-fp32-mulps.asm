streamin in;
streamin rhs;
streamout out;
movaps xmm0,in;
mulps xmm0,rhs;
movaps out,xmm0;
