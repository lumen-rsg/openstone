streamin in;
streamout out;
float half=0.5;
float quarter=0.25;
float one=1;
float two=2;
jmp main;
scale:
mulps xmm0,half;
addps xmm0,quarter;
ret;
to_scale:
cmp in,0;
jz plain_return;
addps xmm0,one;
jmp scale;
plain_return:
ret;
main:
movaps xmm0,in;
call to_scale;
mulps xmm0,two;
call scale;
call to_finish;
call finish;
subps xmm0,one;
movaps out,xmm0;
jmp done;
to_finish:
addps xmm0,quarter;
jmp finish;
finish:
addps xmm0,one;
ret;
done:
