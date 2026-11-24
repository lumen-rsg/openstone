streamin in;
streamout out;
float offset=10;
movaps xmm0,in;
jmp 7;
addps xmm0,offset;
movaps out,xmm0;
