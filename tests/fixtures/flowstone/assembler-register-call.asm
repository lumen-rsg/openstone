streamin in;
streamout out;
float half=0.5;
float one=1;
float step=0.125;
float calls;
movaps xmm0,in;
mov eax,adjust;
call eax;
addps xmm0,one;
call eax;
addps xmm0,calls;
movaps out,xmm0;
jmp done;
adjust:
mulps xmm0,half;
movaps xmm1,calls;
addps xmm1,step;
movaps calls,xmm1;
ret;
done:
