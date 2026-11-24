streamin in;
streamout out;
float half=0.5;
float bias=0.25;
float one=1;
movaps xmm0,in;
call entry;
adjust:
cmp edx,0;
jz returned;
body:
mulps xmm0,half;
addps xmm0,bias;
sub edx,1;
jnz body;
returned:
ret;
entry:
pop rax;
push rax;
mov edx,in[0];
cmp edx,0;
mov edx,2;
jnz firstReady;
mov edx,0;
firstReady:
call rax;
pop rax;
addps xmm0,one;
mov edx,3;
call rax;
movaps out,xmm0;
