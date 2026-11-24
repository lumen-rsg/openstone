streamin in;
streamout out;
float half=0.5;
float bias=0.25;
float one=1;
float two=2;
call bind;
target:
mulps xmm0,half;
addps xmm0,bias;
ret;
bind:
pop rax;
movaps xmm0,in;
call outer;
addps xmm0,one;
call outer;
mulps xmm0,two;
movaps out,xmm0;
jmp done;
outer:
addps xmm0,one;
jmp eax;
done:
