streamin in;
streamout out;
float half=0.5;
float bias=0.25;
float one=1;
call entry;
adjust:
mulps xmm0,half;
addps xmm0,bias;
ret;
entry:
pop rax;
movaps xmm0,in;
push rax;
call rax;
pop rax;
addps xmm0,one;
call rax;
movaps out,xmm0;
