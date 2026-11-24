streamin in;
streamout out;
float half=0.5;
float bias=0.25;
float one=1;
float two=2;
float four=4;
movaps xmm0,in;
mov edx,in[0];
cmp edx,0;
call outer;
subps xmm0,one;
call leaf;
mulps xmm0,two;
movaps out,xmm0;
jmp done;
outer:
push edx;
jz outer_tail;
call middle;
outer_tail:
addps xmm0,four;
pop edx;
ret;
middle:
mulps xmm0,two;
call leaf;
subps xmm0,one;
ret;
leaf:
mulps xmm0,half;
addps xmm0,bias;
ret;
done:
