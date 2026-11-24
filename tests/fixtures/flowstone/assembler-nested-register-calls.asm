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
mov eax,outer;
call eax;
subps xmm0,one;
mov eax,leaf;
call eax;
mulps xmm0,two;
movaps out,xmm0;
jmp done;
outer:
push edx;
jz outer_tail;
push eax;
mov eax,middle;
call eax;
pop eax;
outer_tail:
addps xmm0,four;
pop edx;
ret;
middle:
mulps xmm0,two;
push eax;
mov eax,leaf;
call eax;
pop eax;
subps xmm0,one;
ret;
leaf:
mulps xmm0,half;
addps xmm0,bias;
ret;
done:
