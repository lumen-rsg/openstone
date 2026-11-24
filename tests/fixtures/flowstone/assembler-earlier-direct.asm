streamin in;
streamout out;
float half=0.5;
float bias=0.25;
float one=1;
jmp entry;
adjust:
mulps xmm0,half;
addps xmm0,bias;
ret;
twice:
call adjust;
addps xmm0,one;
call adjust;
ret;
entry:
movaps xmm0,in;
call twice;
movaps out,xmm0;
