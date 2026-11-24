streamin in;
streamout out;
float half=0.5;
float one=1;
call entry;
descend:
cmp edx,0;
jng returned;
push edx;
sub edx,1;
call rax;
pop edx;
movd xmm1,edx;
cvtdq2ps xmm1,xmm1;
mulps xmm0,half;
addps xmm0,xmm1;
returned:
ret;
entry:
pop rax;
movaps xmm0,in;
cvttps2dq xmm1,in;
movd edx,xmm1;
call rax;
addps xmm0,one;
movaps out,xmm0;
