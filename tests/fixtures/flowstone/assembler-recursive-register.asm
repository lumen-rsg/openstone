streamin in;
streamout out;
float half=0.5;
float one=1;
movaps xmm0,in;
cvttps2dq xmm1,in;
movd edx,xmm1;
mov eax,descend;
call eax;
addps xmm0,one;
movaps out,xmm0;
jmp done;
descend:
cmp edx,0;
jng returned;
push edx;
sub edx,1;
call eax;
pop edx;
movd xmm1,edx;
cvtdq2ps xmm1,xmm1;
mulps xmm0,half;
addps xmm0,xmm1;
returned:
ret;
done:
